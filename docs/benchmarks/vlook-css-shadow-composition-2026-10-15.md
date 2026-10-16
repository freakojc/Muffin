# VLOOK 首次绘制的 CSS 阴影与效果合成（2026-10-15）

本轮确认剩余热点来自页面盒阴影，不是上轮文字发光，也不是滤镜/遮罩。修复共用 `DecorationPainter::paintBoxShadow()`，保留 CSS 声明、模糊内核、最终图片和绘制顺序。未按主题名称特判，未修改安装主题、固定 CSS 或字体。[完整前后阶段记录](vlook-css-shadow-composition-2026-10-15.json)。

## 成本拆分

默认关闭的 `muffin.perf.paint` 增加盒阴影 mask、blur、回贴，稀疏滤镜 source/surface/drop-shadow/blur、backdrop-filter 与遮罩 scopes。记录盒阴影输入尺寸、源矩形、半径和缓存命中；这些 scopes 是包含子调用的墙钟，不能相加或直接当作 CPU 时间。

固定 VLOOK Fancy Dark 的 `content>#write` 使用 `box-shadow:0 0 5px #0000001a`。当前实现对应 blur radius=3、halo=11。首屏只有一个有效盒阴影，其输出为 **982×922 = 905,404 像素**，源矩形在图片中为 **(11,11,960,915)**。大量内部纯色行列与边界外透明区域都被三轮模糊反复遍历。

两份详细诊断首次绘制 **238.87 / 129.42ms**；其中盒阴影 blur **108.69 / 89.60ms**，mask **88.68 / 8.97ms**，整次盒阴影栅格生成 **198.06 / 99.29ms**。第一份 mask 明显较慢，保留样本，没有将该差异直接归因于某种初始化或调度因素，也没有把所有首次差异归因于 blur。两次小型稀疏滤镜仅分配 **50 像素**、最大单张 **25 像素**，总 scope 约 **0.05–0.06ms**，没有有效 drop-shadow 或 mask 成本。未因共用 Shadow 名称而继续优化无关效果。

热绘制及两次切回的页面阴影已经命中原有缓存，因此本轮不重复增加一层静态缓存，也不宣称“发现每帧重复生成页面阴影”。真正要优化的是新尺寸或新样式首次 miss 的生成成本。

## 通用修复

圆角矩形在远离角点和完整模糊支撑后，中间的行/列完全相同。按每个轴的有效圆角半径、三轮卷积支撑及抗锯齿 guard，保留角点和过渡带，重复区域只保留一个样本。对该紧凑遮罩执行原来的三轮 box blur，再用原始像素复制恢复重复行/列。

这不是缩放或降采样：边缘、角点及全部模糊样本维持原始分辨率，复原不插值。没有足够重复区域的小形状自然保持原尺寸。最终仍是原来的完整图片，一次回贴到 QPainter，变换、opacity、clip 和叠加顺序不变，避免分片绘制的接缝。

VLOOK 对应紧凑遮罩为 **45×30 = 1,350 像素**，模糊处理像素减少约 **99.85%**。最终 **905,404 像素**的输出仍需分配和缓存，因此不能把这个比例解释为总绘制内存节省。保留原有 **16 MiB/绘制线程**最终栅格缓存，仍按颜色、尺寸、子像素相位、圆角、实际模糊半径和可见裁剪范围区分结果；滚动/延迟页高归一化规则不变。颜色或几何变化生成新结果，没有新增 AST 或主题对象驻留。

## 显示与缓存验证

独立测试保留修改前的完整画布 mask/卷积算法，与优化结果做严格 QImage 比较，不扩大容差。覆盖 **5 种形状 × 4 组圆角/模糊 × 3 档 DPR × 3 种裁剪/变换 = 180 组**，每组再检查缓存命中像素。包含超高页面、负坐标、源超出设备、窄矩形、大圆角、正负 spread、半透明颜色、分数定位、缩放旋转及 opacity。

原有像素/浏览器效果、远距离滤镜阴影、滚动、裁剪、颜色/半径失效、整数平移复用和延迟页高缓存回归继续执行。新增工作量断言验证大型纯色内部不参加完整卷积，最终缓存预算仍有界。

## 正式阶段测量

修改前后各 **18 个新进程**：固定 VLOOK Fancy Dark、完整本机 Phycat Abyss 和内置 GitHub，**1/10/30 MiB × 两轮**，每个进程包含首次、热绘制与两次切回。CSS/字体 SHA-256 在前后保持相同，详细 trace 关闭，原始样本全部保留。以下为两次值的中位数，单位 ms；不以两次样本宣称稳定长尾分布，也不以 offscreen 墙钟代替 GUI 显示/输入延迟。

| 主题 | MiB | 首次绘制：前 → 后 | 热绘制：前 → 后 |
| --- | ---: | ---: | ---: |
| VLOOK | 1 | 100.14 → 27.73 | 7.43 → 7.69 |
| VLOOK | 10 | 100.24 → 30.75 | 7.52 → 9.13 |
| VLOOK | 30 | 100.85 → 31.75 | 7.31 → 7.48 |
| Phycat | 1 | 39.15 → 41.15 | 7.68 → 10.51 |
| Phycat | 10 | 39.49 → 40.68 | 7.82 → 7.71 |
| Phycat | 30 | 40.53 → 42.09 | 7.64 → 7.97 |
| GitHub | 1 | 13.53 → 13.59 | 7.20 → 7.83 |
| GitHub | 10 | 13.25 → 16.32 | 7.44 → 7.27 |
| GitHub | 30 | 14.75 → 15.15 | 7.62 → 7.53 |

没有宣称热绘制或其他主题统一提速。主矩阵 **18 对**前后记录各三次视觉快照全部相等，包含像素 SHA-256、块几何、四处光标矩形与命中。进程内两次切回与首次完全一致，完整计数器参考通过。VLOOK 上轮恢复的右对齐标题阴影继续保留，本轮没有新的预期像素变化。

修改后另用两个新进程开启详细 scope，首次 **31.44 / 38.57ms**。盒阴影首次生成 **0.82 / 0.95ms**：mask **0.047 / 0.048ms**，blur 均约 **0.109ms**，完整图片复原 **0.64 / 0.76ms**；回贴 **0.47 / 1.06ms**。首次输出仍是 **905,404 像素**，缓存预算记账 **3,621,888 字节**（按 KiB 向上取整，约 3.45 MiB），热绘制及两次切回均命中，不生成新 mask。

两轮首次绘制后的 VLOOK 工作集，1 MiB 从 **140.42/140.25 → 139.49/139.29 MiB**，10 MiB **443.39/444.26 → 443.99/444.11 MiB**，30 MiB **1123.88/1122.09 → 1124.02/1123.47 MiB**。未观察到一致的进程工作集下降，也不能据这些点采样排除泄漏。算法减少首次临时遮罩/卷积工作，最终图片、缓存和文档主体仍占内存；没有将紧凑遮罩像素比例当成总内存收益。

全量测试结束后另做 **16 个交错顺序新进程**：1 MiB、VLOOK/Phycat、四轮，交替新旧版执行顺序。VLOOK 首次中位数 **105.64 → 30.05ms**，新四份 **30.13/30.11/29.99/27.15ms**；热绘制 **7.78 → 7.64ms**，两次切回 **20.78 → 20.56ms**、**20.66 → 20.72ms**。首次收益可重复，已有命中路径基本不变。Phycat 首次 **38.97 → 41.14ms**，热绘制 **8.22 → 8.12ms**，未显示统一收益；保留全部波动，不将其他主题结果隐藏。各进程三次视觉哈希、几何、光标与版本内固定参考一致，计数器参考通过。数据在 JSON 的 `interleaved_repeat`。

### 复现

```powershell
cmake --build --preset conan-release
cmake --build --preset conan-release --target MuffinThemePerformanceProbe
python scripts/run_first_layout.py build/vlook-effects-repeat --repetitions 2
python scripts/run_first_layout.py build/vlook-effects-detail-repeat --themes vlook --sizes 1 --repetitions 2 --paint-detail
ctest --preset conan-release --output-on-failure
cmake --build --preset conan-release --target dist
```

修改前二进制保存在 `build/vlook-effects-before-bin`；正式记录位于 `build/vlook-effects-formal-before`、`build/vlook-effects-formal-after`，详细记录位于 `build/vlook-effects-detail-before`、`build/vlook-effects-detail-after`。原始 JSON 保留全部阶段、样本及资源/二进制摘要。

## 验收完成

完整 Release 构建通过，完整 CTest **339/339** 全绿（**480.42 秒**），日志 `build/vlook-effects-release-build.log`、`build/vlook-effects-all-ctest.log`。dist 已刷新，Muffin.exe、MuffinCore.dll、MuffinUi.dll 的 SHA-256 与 Release 一致，见 `build/vlook-effects-dist.log`、`build/vlook-effects-dist-hashes.json`。

正式前后 **36** 份、详细前后 **4** 份、交错复测 **16** 份，共 **56 个进程**原始阶段记录保存在 JSON；工程中的初步 smoke 单独留在 build，不混入最终统计。复核全部中位数、像素/几何/计数器、资源与二进制摘要、紧凑 mask 与输出尺寸、缓存预算，Python 语法、手动 C++ 基准格式和 `git diff --check` 通过。未 commit、未 push，本轮不恢复暂缓的原生键盘长尾或完整前台 GUI 验收。
