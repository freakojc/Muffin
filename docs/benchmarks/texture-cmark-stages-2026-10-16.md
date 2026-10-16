# VLOOK 网格相位复现与 cmark 阶段拆分

本轮先复现上一轮唯一的 VLOOK 背景异常，再测量 cmark。产品只修复通用 `background-position` 的 CSS-wide 值展开；没有修改 cmark 解析算法、关闭产品扩展或更改缓存策略。完整原始记录和独立审计见 [JSON](texture-cmark-stages-2026-10-16.json)。

## 背景异常的原因与修复

固定 VLOOK Fancy Dark 中有 `background-position:var(--d-bi-pos),0 0`，但主题没有定义 `--d-bi-pos`。整个声明在计算值阶段无效，应按 `unset` 使用两个轴的初始值 **0% 0%**，而不是恢复此前的声明。旧展开逻辑把 `unset` 当成单词形式的位置，并自动补上第二个轴的 `center`；最终得到 X=0%、Y=50%。

背景图本身是 120×120 PNG，内部网格周期 40px。当延后排版改变全页估算高度 40px 时，居中的背景锚点变化 20px：可见文字、光标和首屏块的位置都可以保持不变，但背景网格相位移动。这解释了上一轮 39,429 个像素仅落在水平网格线上的异常，不涉及 AST 共享文本生命周期或图片缓存失效。

`scripts/texture_phase_probe.cpp` 直接取真实主题的 `#write` 计算样式，隔离改变页面高度、画布偏移和绘制区域。旧版真实计算结果 Y=50%；高度 **1,000,000→1,000,040px** 的两张图稳定复现 **20px** 相位差。分段与整块绘制的图相同，缩小了排查范围。

修复在公共声明展开路径中处理 `initial/unset/inherit/revert/revert-layer`，让值同时作用于 X/Y，避免凭空生成 Y=center；合法单值位置仍沿用原来的 CSS 规则。回归覆盖未定义变量、变量循环、有合法 fallback、继承、显式值被无效变量覆盖，以及百万像素页面高度变化后的精确像素相等。

修复验证：

- 7 种高度 × 2 种画布偏移 × 整块/分段绘制，共 **28/28** 场景在相同偏移下像素完全相同。
- Chromium **3/3** 独立样例的计算值均为 `0% 0%, 0% 0%`，不同高度截图相同；与 Qt 在公共裁剪区域的最大通道差为 **1**，网格位置一致。这里保留 Skia/Qt 合成的一档色差，没有宣称跨引擎逐像素完全相同。
- 10 MiB 文档的 Windows QPA `EditorView` 连续八轮 GitHub→VLOOK 切换，**8/8** 截图完全相同，首块几何、scroll=0 和总高度一致。窗口使用 `WA_DontShowOnScreen`，仍走 Windows 的 Lazy 排版路径；这是显示正确性检查，不是实际前台的延迟测量。

旧异常原图和上一轮统计继续保留，没有把原来的 **83/84** 改写为全绿。旧独立复现位于 `build/texture-isolated`，修复后独立及 EditorView 图在 `build/texture-native-fixed`，浏览器图在 `build/texture-browser`。

## cmark 测量方法

应用继续使用普通 cmark 静态库。新增两个 `EXCLUDE_FROM_ALL` 手动目标：

- `MuffinCmarkArchiveProbe`：直接链接普通、未修改的静态库，提供总解析耗时和树/HTML 指纹对照。
- `MuffinCmarkPerformanceProbe`：只在独立诊断程序中编译 block-parser 对象，重定向行内容器解析与文本合并调用，分别包围行内遍历和脚注整理，计时 autolink 后处理。生成源只比 vendored `blocks.c` 多两处明确的阶段 hook；不写回第三方源码，也不链接进应用 DLL。

每次使用新进程，三轮交错/反序运行。正式矩阵为 1/10/30 MiB GUI 同款富行内文本、30 MiB 普通正文，以及 1 MiB 表格/任务/数学/脚注/自动链接混合样例。保留完整扩展和关闭 autolink 的诊断对照，混合样例另有无扩展对照。完整集与应用一样启用脚注，挂载 table/autolink/tasklist/math；删除线仍由 Muffin 的后处理负责，未挂载 cmark strikethrough。

共 **63 份**最终样本：30 份普通库、30 份阶段诊断、3 份诊断对象关闭计时的对照。首轮只拆行内、合并和 autolink 的 **63 份探索记录**另存 `build/cmark-stages`；加入脚注和整体行内遍历后重新测量，最终数据在 `build/cmark-stages-final`，两组不混用。

计时不含输入生成/UTF-8 准备、树/HTML 指纹和释放；释放另列。`feed` 包含块解析和块扩展回调；`inline_walk` 包含行内容器解析及遍历，`inline_ms` 是其子集，不能相加。每个行内容器的时钟会扰动诊断值，因此同时保留普通库及关闭诊断的对照；阶段数据用于定位，不宣称优化收益。关闭扩展的模式可能改变语义，不能作为产品的优化方案。

## cmark 结果

以下是三次样本的阶段中位数，单位 ms。各阶段中位数来自不同样本，不能逐列相加还原总中位数。

| 样例 | 普通库总解析 | 诊断总解析 | 块读取 feed | 行内遍历（含解析） | 脚注整理 | 文本合并 | autolink 后处理 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 MiB 富行内 | 84.94 | 70.53 | 1.30 | 30.79 | 15.28 | 8.97 | 17.58 |
| 10 MiB 富行内 | 716.74 | 732.15 | 12.31 | 297.91 | 160.10 | 81.77 | 179.61 |
| 30 MiB 富行内 | 2154.30 | 2242.08 | 40.03 | 889.34 | 432.46 | 227.32 | 483.69 |
| 30 MiB 普通正文 | 344.61 | 363.68 | 36.83 | 154.58 | 82.46 | 77.88 | 13.72 |
| 1 MiB 扩展混合 | 109.13 | 102.03 | 26.17 | 27.01 | 20.58 | 8.36 | 20.96 |

30 MiB 富行内有 **29,281** 个行内容器，容器解析中位数 **886.79ms**，与包括遍历的 **889.34ms** 接近。三份 30 MiB 富行内样本的 `finish` 扣除行内遍历、脚注、合并和 autolink 后，残留只有 **0.006–0.008ms**。主要成本已经归属，不需要把未知等待归到块解析上。

该富行内文档**没有脚注**，但 `CMARK_OPT_FOOTNOTES` 仍使 `process_footnotes` 执行收集和引用遍历。约 **432ms** 是下一轮可单独研究的明确成本；必须保留真正脚注的编号、定义移动、未解析引用和嵌套结构行为，不能直接关闭脚注支持。

富行内样例链接均为显式 Markdown 链接。关闭 autolink 的普通库总解析中位数：1/10/30 MiB 为 **51.33/515.15/1415.07ms**，与完整模式的树及 HTML 指纹相同；这组输入没有需要新识别的裸链接，但仍支付全树后处理成本。扩展混合样例关闭 autolink 后**指纹不同**，证明不能把这一诊断对照推广为删除功能。下一轮应考虑收紧实际候选或复用遍历，覆盖网址、邮件、标点、HTML/代码/已有链接和跨节点文本。

正式结果存在顺序/调度波动，30 MiB 阶段诊断总时间 **1949.89、2242.08、2427.33ms**，全部保留；autolink 单次曾达 **652.92ms**。关闭诊断对象的 1/10/30 MiB 总时间为 **68.04/662.24/2079.19ms**（各一次），不能据此估计诊断扰动的稳定百分比，更不能把诊断与普通库的差异解释成优化收益。

## 独立验证与边界

`scripts/audit_texture_cmark.py` 独立读取原始记录，重算中位数和时长关系，检查生成诊断源仅有两处声明的插桩，核对全部源/二进制摘要、63 份树和 HTML 指纹，以及原图像素。诊断程序与同扩展模式的普通库结果全部一致；不同模式的语义差异单独保留。

完整 Release 构建通过，重点 **2/2（0.65 秒）**、全量 **339/339（573.76 秒）**通过，日志 `build/texture-release-build.log`、`build/texture-focused-ctest.log`、`build/texture-all-ctest.log`。dist 已刷新，三个应用二进制与 Release SHA-256 一致，日志 `build/texture-dist.log`。Python/JavaScript 语法、独立审计和 `git diff --check` 通过。本报告不把独立 QImage/隐藏窗口结果当作六组前台 GUI 复测，也不把 cmark 单阶段时间当作打开至首屏。原生键盘长尾调查保持暂缓，未 commit、未 push。

## 复现

```powershell
cmake --build --preset conan-release --target MuffinTexturePhaseProbe MuffinCmarkPerformanceProbe MuffinCmarkArchiveProbe
$env:QT_QPA_PLATFORM='windows'
.\build\Release\MuffinTexturePhaseProbe.exe tests/fixtures/theme/vlook-fancy-dark/vlook-fancy-dark.css build/texture-recheck
node scripts/probe_background_position.mjs build/texture-browser-recheck
python -B scripts/run_cmark_performance.py --output build/cmark-recheck --repeats 3
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
cmake --build --preset conan-release --target dist
```

运行手动程序时使用 Conan preset 的 Qt `bin`/插件环境，产物目录需使用新名称。图片审计脚本默认读取本报告的原始路径，可用 `python -B scripts/audit_texture_cmark.py --output build/texture-cmark-audit.json` 复查本次记录；报告中的最终构建与打包验证另见 `verification` 字段。
