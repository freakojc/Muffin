# 文字阴影局部栅格与静态缓存（2026-10-15）

本轮把 Phycat 定位出的瓶颈修复到通用 `text-shadow` 路径，保留主题原有发光声明。实现位于 `src/render/TextShadowPainter.cpp`，由 `InlineLayout` 调用，其他主题共用。未修改本机安装的主题或字体，也没有通过关闭阴影获得提速。[完整前后样本与校验](text-shadow-sparse-cache-2026-10-15.json)。

## 有效范围与绘制

- 行内阴影根据源文字范围生成局部 mask。分离的行保持稀疏；模糊支撑范围相交时先合并，再卷积一次，避免重叠透明阴影重复叠加。宿主/合成阴影保留完整文字、字体外伸、描边和装饰。
- 三轮 box blur 的 halo 使用 `3 × 物理模糊半径 + 2`，留下透明边缘。保持原有模糊算法和分辨率，不通过降采样或缩短发光范围省时。
- 可见性判断包含阴影偏移及完整 halo；完全不可见的源不栅格化。超过一百万像素的区域进一步限制为设备范围与显式绘制 clip，加上完整模糊支撑，避免大宿主制造巨幅透明图。
- 同时把局部范围传给 `TextLayout::draw()`，让 Qt 跳过无关行。仅缩小 QPainter clip 仍会遍历整段文字，初版首次绘制约 105ms，进一步限制文字行后约 40ms；两份工程诊断保留在 `build/text-shadow-sparse-smoke`、`build/text-shadow-sparse-clipped`，不混入正式统计。
- 高 DPI 使用物理像素创建 mask、计算模糊及回贴，统一源原点的设备像素对齐。避免 Qt 在零点两侧对半像素图片坐标取整不同，使裁剪前后相差一像素。

## 内存缓存与失效

缓存仅在内存中存在，GUI 线程上的视图共享 **16 MiB 图片预算、最多 512 项**。字节数统计图片存储，不包括 Qt 对象和键的元数据。超过预算的单张图片不驻留。

每个不可变排版持有独立的缓存 owner。重建排版或销毁布局会立即移除其残留条目；移动布局保留原 owner。文字编辑、marker 展开、字体、缩放、换行和主题变化都随排版重建换代，不持有 AST，也不把大段源码重复写入缓存键。

同一排版内按 DPR、实际物理模糊半径、阴影颜色、源裁剪范围、绘制格式覆盖和栅格区域区分结果。颜色/半径动画不会误用静态结果；相同动画采样可命中。偏移只改变回贴坐标，普通小区域可跨滚动位置复用；大区域按实际需求裁剪时，区域坐标参与键。保留各元素原有绘制次序，不将不同元素的阴影任意合并。

## 显示与失效回归

新增测试包含独立大画布参考：完整 mask、相同三轮卷积及源裁剪，然后与局部算法逐像素比较。覆盖 DPR **1 / 1.5 / 2**、blur **0 / 2 / 5**、宿主/行内、跨行间隙、半透明颜色、分数偏移、格式覆盖、重叠源裁剪、负坐标滚动、偏移后进入视口，以及大区域与小脏区。普通文字要求像素完全一致；带描边路径经 Qt 平移栅格化后，允许每个预乘通道最多 1 级舍入差，缓存命中仍须完全一致。统一像素锚点由参考明确规定，不以旧大画布跨零点时的取整偏差作为预期。

另用真正的 `InlineLayout` 验证重复绘制、文字编辑、字体/主题替换、缩放、换行、Markdown marker 展开和 hover 阴影变化，核对与新建布局相同的像素、光标矩形和命中。预算压力与 owner 重置/销毁测试确认缓存有界且能及时释放。

### VLOOK 右对齐标题的缺失阴影

前后比较发现 VLOOK 的标题边缘有少量变暗。原因不是发光被削弱：旧 `size_.width()` 使用自然文字宽度，忽略对齐后的实际位置。标题字形位于布局 **x≈716**，旧算法却只生成 **126×54px** 的遮罩（padding=3、blur=0）；字形落在遮罩外，主题声明的 `text-shadow:1px 1px #0003` 根本没有画出来。旧二进制独立 trace 的两个图片尺寸与源码、固定几何相互印证，原始数据见 `build/text-shadow-vlook-old-detail`。

新算法根据 `naturalTextRect()` 的实际坐标恢复标题阴影。增加一个 835px 宽、文字宽度小于 200px 的右对齐宿主回归，与整画布直接文字绘制要求逐像素一致。正式对照对 VLOOK 单独验证变化均为标题范围内的阴影变暗；其余区域、块几何和编辑坐标必须不变。不能把该修正写成“所有主题与旧版截图完全相同”。

## 正式测量

关闭详细 trace，修改前后各 **18 个新进程**：固定 VLOOK Fancy Dark、完整本机 Phycat Abyss、内置 GitHub，**1/10/30 MiB × 两轮**。每个进程记录首次、热绘制和两次切回；固定同一 960×900 视口，完整计数器参考独立核对。这是 offscreen 阶段墙钟，不代表 GUI 显示或原生输入延迟。所有正式样本均保留，未排除慢样本。

以下为两份原始样本的中位数，单位 ms。两次测量不足以代表稳定长尾分布。

| 主题 | MiB | 首次：修改前 → 后 | 热绘制：修改前 → 后 |
| --- | ---: | ---: | ---: |
| Phycat | 1 | 866.93 → 53.06 | 817.69 → 9.73 |
| Phycat | 10 | 870.25 → 40.14 | 856.19 → 10.01 |
| Phycat | 30 | 881.31 → 44.30 | 816.16 → 9.55 |
| VLOOK | 1 | 100.96 → 147.89 | 7.86 → 9.87 |
| VLOOK | 10 | 117.60 → 108.13 | 10.81 → 8.00 |
| VLOOK | 30 | 101.47 → 141.66 | 7.64 → 9.81 |
| GitHub | 1 | 18.22 → 14.23 | 7.24 → 7.55 |
| GitHub | 10 | 13.92 → 14.86 | 7.07 → 7.64 |
| GitHub | 30 | 14.45 → 16.23 | 7.55 → 8.51 |

VLOOK 修改后 1 MiB 两份首次为 **101.76 / 194.03ms**，30 MiB 为 **111.41 / 171.91ms**。两个慢样本的 `ThemePerfStage::Shadow` 独占墙钟为 **160.08 / 142.24ms**，相邻正常样本约 80–88ms；text 阶段仍约 5–6ms。该阶段属于其他 CSS 阴影/效果路径，并非新 `paint.textShadow` scope。阶段证据能缩小调查方向，但不能排除系统调度或宣称所有主题都提速。

Phycat 额外两个新进程开启详细绘制 scopes，首次 **39.96 / 40.67ms**，热绘制 **7.90 / 8.85ms**。阴影调用仍有 **36** 次，其中 **7** 次完全不可见而提前跳过，实际 **29** 张局部图累计 **62,049 像素**，最大单张 **2145 像素**，缓存驻留 **248,196 字节（约 242 KiB）**。相比旧单份 mask/shadow 的累计 **9,622,368 像素**，减少约 **99.36%**；并非把每一张可见阴影的字形削小。

详细 scopes 相互包含，不能相加。首次全部 text-shadow 约 **16.91–17.09ms**，其中 mask **11.34–11.44ms**、blur **4.59–4.76ms**、着色 **0.30–0.34ms**。热绘制 **29 次命中、零 miss、零新增栅格像素**，整个 text-shadow 约 **0.24–0.26ms**。主题切走后缓存字节归零；切回因排版重建重新生成，随后可继续复用。没有用已失效的旧主题图片掩盖首次开销。

实际仓库 `example.md`（**17,214 字符**）另测两份新进程，首次 **18.40 / 21.90ms**、热绘制 **8.04 / 7.99ms**；旧轮对应首次 **29.5 / 27.0ms**、热绘制 **14.7 / 13.9ms**，两份输入摘要、全部像素与编辑几何相同。不能把它当成 1 MiB 文件，也不能由这个短文件推出大型 GUI 连续输入已验收。

主矩阵 Phycat/GitHub 的 **12 对**前后记录各三次视觉哈希完全一致；VLOOK 的 **6 对**只在两条标题范围恢复阴影，首屏 **1213 像素**变暗，单通道最大差 **7**，其余像素不变。全部 **18 对**块几何、四处光标矩形/命中相同，进程内两次切回与首次完全一致，完整计数器对照通过。实际 example 两对也完全一致。原始输入、CSS 和字体 SHA-256 在前后保持相同。

全量测试结束后再做 **16 个交错顺序新进程**：1 MiB、VLOOK/Phycat、四轮，每轮交换新旧版先后顺序。全部样本和版本内视觉参考对照写入 JSON 的 `interleaved_repeat`。Phycat 首次中位数 **862.96 → 39.51ms**、热绘制 **874.38 → 7.73ms**，两次切回约 **848.30 → 37.28ms**、**833.40 → 37.15ms**；保留旧版首次 **1181.08ms** 的较慢样本。VLOOK 首次 **99.36 → 103.41ms**、热绘制 **7.74 → 7.86ms**，未再次复现原矩阵的 172–194ms，但原慢样本没有删除。这组支持 Phycat 收益可重复，不能证明 VLOOK 永无波动或所有阴影效果已优化。

### 复现

```powershell
cmake --build --preset conan-release
cmake --build --preset conan-release --target MuffinThemePerformanceProbe
python scripts/run_first_layout.py build/text-shadow-repeat --repetitions 2
python scripts/run_first_layout.py build/text-shadow-detail-repeat --themes phycat --sizes 1 --repetitions 2 --paint-detail
python scripts/run_first_layout.py build/text-shadow-example-repeat --themes phycat --sizes 1 --document example.md --repetitions 2 --paint-detail
ctest --preset conan-release --output-on-failure
cmake --build --preset conan-release --target dist
```

修改前二进制保存在 `build/phycat-shadow-before-bin`；正式矩阵位于 `build/text-shadow-formal-before`、`build/text-shadow-formal-after-final`，源码与字体依赖摘要、每个原始阶段和所有对照结果随 JSON 保留。工程中间结果与最终统计分开保存。

## 验证完成

完整 Release 构建通过，完整 CTest **339/339** 全绿（**490.71 秒**），包含 native/fractional 文字路径。日志为 `build/text-shadow-release-build-final.log`、`build/text-shadow-all-ctest.log`。`dist` 已刷新，Muffin.exe、MuffinCore.dll、MuffinUi.dll 的 SHA-256 与 Release 一致，见 `build/text-shadow-dist.log`、`build/text-shadow-dist-hashes.json`。

正式前后矩阵 **36** 份、最终细分 **2** 份、实际文件 **2** 份、旧 VLOOK 尺寸诊断 **1** 份、交错复测 **16** 份，共 **57** 个进程记录随 JSON 保留；其中实际文件还引用上一轮两个对应原始记录。独立检查资源摘要、全部中位数、像素/几何和缓存预算/切走释放，Python 语法、新增 C++ 与手动基准格式、`git diff --check` 通过。未提交、未推送。本轮不进行原生键盘或完整前台 GUI 验收。
