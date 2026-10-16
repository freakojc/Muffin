# Phycat 绘制约 0.9 秒的定位（2026-10-15）

主要开销来自粗体的 `text-shadow`：每个词重复创建、着色并模糊整段大小的图片。它也发生在热绘制中，并非仅在首次切换主题时发生。[完整阶段记录与对照](phycat-text-shadow-paint-diagnostic-2026-10-15.json)保留所有样本。

## 触发路径

完整 Phycat Abyss 导入的 `phycat.dark.css` 给普通粗体设置：

```css
#write strong {
    text-shadow: 0 0 2px rgba(0, 243, 255, .3);
}
```

`InlineLayout::paint()` 为每个与宿主阴影不同的行内 run 调用 `paintTextShadow()`。后者依据整个 `InlineLayout::size_` 创建 mask 和 shadow。传入的词级矩形只限制 mask 的绘制范围，没有缩小图片，也没有缩小后续 `boxBlur()` 的遍历范围。每次重绘重新生成，不复用静态结果。

本次固定视口有三段正文，每段 12 个粗体，合计 **36 次**阴影调用。一个词的裁剪矩形约 **35.9063×22px**，每次却处理 **903×296px** 的图片，blur=2、pad=4。

- 每帧单份 mask 或 shadow 累计处理 **9,622,368 像素**；裁剪矩形面积累计约 **28,438px²**。前者约为后者的 338 倍，裁剪矩形并不等同于实际字形像素。
- mask 与 shadow 两份 ARGB32 图片累计约 **73.4 MiB** 的图片容量；这是每帧累计创建量，不是峰值驻留内存，未计分配器复用和其他临时对象。
- `boxBlur()` 默认三轮，每轮横/纵遍历整图，包括大片透明区域。既有 `raster_allocated_pixels` 只统计稀疏 CSS filter 的分配，未包含这里的 text-shadow，不能拿那个小数值排除文字阴影。

## 细分证据

增加默认关闭的 `muffin.perf.paint` scopes，细分行内内容、伪元素、合成裁剪、阴影 mask、着色、模糊和回贴。阶段计时是包含子调用的墙钟时间，不能相加，也不代表精确 CPU 时间。

以下为两份最终详细诊断中的第二份首次绘制及两份热绘制；第一份首次绘制为 **1032.8ms** 的较慢样本，同样保留在 JSON 中。

| 阶段 | 第二份首次 | 第一份热绘制 | 第二份热绘制 |
| --- | ---: | ---: | ---: |
| 整次绘制 | 866.3ms | 860.3ms | 840.8ms |
| 36 次 text-shadow（含子项） | 841.7ms | 851.6ms | 832.5ms |
| 其中 blur | 749.2ms | 765.8ms | 748.9ms |

早期细分的两份首次记录中，mask 约 **60–61ms**，着色约 **23–33ms**，回贴约 **1.8ms**，普通行内内容约 **5.7–5.8ms**，生成伪元素约 **0.9ms**。没有进入语义行内整体合成分支。初始总阶段统计中的背景/盒阴影均小于 1ms、首次 CSS 求值约 2–3ms；热绘制 CSS 求值为零，仍有约 0.8–0.9 秒。

## CSS 单项对照

用临时 wrapper 导入原始完整主题，只附加实验声明；不修改安装目录或仓库主题。每种三次独立新进程，逐轮轮换执行顺序。关闭详细 trace，保留首次、热绘制及两次切回。中位数如下：

| 临时样例 | 首次绘制 | 热绘制 |
| --- | ---: | ---: |
| 原主题 | 931.1ms | 878.4ms |
| 只去掉 strong 的 text-shadow | 23.8ms | 7.6ms |
| 去掉全部 text-shadow | 24.1ms | 7.5ms |
| 只去掉链接伪元素的 mask | 851.0ms | 850.6ms |

原主题三次首次值为 **1067.5 / 931.1 / 855.9ms**，较慢的第一份没有删除。所有 CSS 对照的块几何、四个位置的光标矩形/命中一致，各自两次切回的像素与首次一致，完整计数器对照通过。不同 CSS 对照的装饰像素按实验设计会变化；去掉阴影的时间不能直接当作“保留视觉效果的修复”结果。

这组对照与源码路径、独立阶段计时相互支持，确认粗体阴影是该样例的主要瓶颈；链接遮罩并非这次的主因。

## 文档大小与真实 example.md

最终关闭详细 trace，完整 Phycat 三尺寸各两个新进程：

| 合成文档 | 两次首次绘制 | 两次热绘制 |
| --- | ---: | ---: |
| 1 MiB | 847.5 / 891.4ms | 828.8 / 813.3ms |
| 10 MiB | 859.1 / 843.0ms | 807.1 / 822.2ms |
| 30 MiB | 841.3 / 840.8ms | 849.1 / 858.4ms |

固定相同视口时，主要开销取决于可见的粗体数量及所在段落面积，未随全篇大小同比增长。这是 offscreen 图片绘制测量，不是 GUI 显示、键盘或系统调度验收。

另用仓库实际 `example.md`（**17,214 字符**）补测两个新进程，首屏只产生一次该阴影调用：首次 **29.5 / 27.0ms**，热绘制 **14.7 / 13.9ms**；其中阴影首次 **7.1 / 6.6ms**。该文件的摘要、实际字符数及文档路径保存在 JSON。此处输出文件名沿用手动基准的 `1MiB` 参数命名，文件输入时该合成大小参数不生效；没有将真实 example 当成 1 MiB 文档。不能将密集长段落样例的约 0.9 秒直接推广到所有文档。

## 后续修复范围

本轮完成定位和诊断工具，保留原绘制算法。建议下一轮按以下顺序实现通用 text-shadow 修复：

1. 按实际阴影源范围创建局部 mask，跨行时使用独立有效区域；将源坐标和回贴坐标准确映射回来。最终阴影范围不与视口/有效裁剪相交时先跳过，判断需包含偏移与模糊外扩。透明区域不参加模糊。裁剪 halo 要覆盖模糊实现的完整支撑范围、偏移和抗锯齿，不能直接照搬现有 pad=4：三次 radius=2 的 box blur 支撑可达 6px。
2. 给静态阴影增加有字节预算的内存缓存。由排版/文本投影、字体与缩放、设备像素比例、裁剪范围及阴影值确定有效性；主题、编辑、换行和 marker 展开需正确失效。动画中相同字形可复用覆盖形状，按实际颜色/半径更新效果；保留元素绘制顺序，不随意合并重叠透明阴影。
3. 固定图片对照保留发光像素，验证行内/宿主、嵌套继承、跨行、滚动、缩放、主题切换和 hover 动画。复测此处 36 个粗体、真实 example 与三尺寸，再评估是否还需优化模糊内核。

修改安装主题以关闭发光并非产品修复。本轮的结果尚不能证明上述裁剪/缓存已经实现或提速。

## 复现与验证

26 个诊断/对照进程及原始行数据全部保留，含两轮初始细分、两轮阴影细分、两轮带覆盖范围的最终细分、三尺寸六份、CSS 对照十二份、真实 example 两份。12 份未变更 CSS 的合成样例与上轮同尺寸记录的像素哈希、块几何、编辑坐标逐项相等；所有记录均通过完整计数器对照。CSS/字体摘要在每组运行前后校验，没有修改外部 Phycat 资源。

```powershell
cmake --build --preset conan-release --target MuffinThemePerformanceProbe
python scripts/run_first_layout.py build/phycat-paint-repeat --themes phycat --sizes 1 --repetitions 2 --paint-detail
python scripts/run_phycat_paint_diagnostic.py build/phycat-paint-controls --repetitions 3
python scripts/run_first_layout.py build/phycat-example-repeat --themes phycat --sizes 1 --document example.md --paint-detail
```

完整 Release 构建通过，完整 CTest **339/339** 全绿（**486.14 秒**），日志 `build/phycat-paint-release-build.log`、`build/phycat-paint-all-ctest.log`。dist 已刷新，Muffin.exe、MuffinCore.dll、MuffinUi.dll 的 SHA-256 与 Release 一致，见 `build/phycat-paint-dist.log`、`build/phycat-paint-dist-hashes.json`。独立重算 26 份数据与全部中位数、图片/几何对照和阴影尺寸，复核主题/字体、CSS wrapper 及真实输入摘要；Python 语法、手动基准 C++ 格式和 `git diff --check` 通过。本轮未提交、未推送。
