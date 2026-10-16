# VLOOK 投影重建与 GUI 排队诊断（2026-10-15）

本轮定位上一轮 30 MiB 单页上下移动的长尾，并细分每次输入的稳定开销。已经确认草稿快照会在 GUI 线程阻塞输入，主题签名被每个排版 pass 重算，菜单状态查询重复构建编辑上下文。本轮添加默认关闭的诊断，没有修改上述调度或缓存行为，也没有将跳过草稿回调的基准对照作为产品优化。

## 方法与数据

沿用[热视口文字绘制基线](vlook-hot-text-performance-2026-10-15.md)的固定 CSS、Qt 6.11.1、Noto Serif 16 px、96 DPI/DPR 1、1100×820 窗口、1086×761 单页视口和屏幕报告的 32 Hz。机器为 i7-10700、64 GiB、Windows 11；CSS SHA-256 为 `dc9ce7c464f4ddf8644040fb77c7c7e408a70f1bee3ba2c9f9a8ac746901f3cc`。日期使用 Asia/Shanghai，单组时间戳为 UTC。

对容易出现长尾的 30 MiB 单页做三对顺序新进程测量：每对先正常运行，再仅阻止临时文档的草稿定时器信号。保留真实 MainWindow/DocumentTab/EditorView、菜单绑定、主题动画、词数刷新、定时器对象与原输入节奏；不并发构建或测试。用户文件和设置隔离，自动保存与更新检查关闭，临时窗口置顶并通过激活/暴露边界检查。基准使用隔离的 INI QSettings，设置查询的绝对耗时不能直接推广到其他设置后端。

每个连续阶段 80 次事件，输入和方向键间隔 50 ms，冷/热滚动间隔 100 ms，主题交替 12 次、间隔 1500 ms。事件由独立线程投递到 Qt；绘制完成是 QWidget paint handler 返回，不是物理输入到 DWM/显示器呈现。p95/p99 使用 nearest-rank，80 次样本的 p99 等于最大值。三对样本用于因果诊断，不是总体延迟保证。

最初的一次探索运行保留在 `build/projection-diagnostic-normal-1/`，不进入正式汇总。前两对细分投影、块重建、菜单与草稿，最后一对增加主题签名、编辑上下文、行内格式查询和草稿 I/O 子阶段。每对内部使用相同诊断；跨对不合并为一个“优化前后”分位数。六组均通过窗口校验，没有截断时间线。

[汇总与完整原始 JSON](vlook-projection-queue-diagnostic-2026-10-15.json)保留全部操作、作用域分布、事件时间线和派生排队关联。复测工具为 `scripts/gui_performance_probe.cpp`、`scripts/run_gui_performance.ps1` 和 `scripts/summarize_projection_queue.py`。

## GUI 阻塞来自草稿快照

30 MiB 样例的草稿间隔为 15,360 ms，首轮输入启动定时器，恰好在上下移动阶段触发。调用链为 `MainWindow::snapshotDraft()` → `DraftRecovery::snapshotSession()` → 全文 materialize → UTF-8 编码 → `QSaveFile` 写入和提交正文/元数据，全部位于 GUI 线程。按 revision 跳过未变化快照只降低了空闲成本，无法降低发生保存时的阻塞。

下表为同一上下移动阶段；时间单位 ms。所有绘制分位数保留原来的“下一次视口绘制”口径，未剔除可能没有实际移动的按键。

| 运行 | 同步 p95 | 计划到绘制 p95 / 最大 | 投递到分发最大 | 草稿 Timer handler | 投递后被草稿阻塞的输入数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 正常 1 | 30.8 | 316.8 / 516.8 | 272.5 | 274.4 | 6 |
| 跳过草稿 1 | 29.7 | 92.2 / 102.5 | 8.5 | — | 0 |
| 正常 2 | 30.0 | 92.1 / 122.3 | 44.9 | 66.8 | 1 |
| 跳过草稿 2 | 30.1 | 91.2 / 94.7 | 12.1 | — | 0 |
| 正常 3 | 30.5 | 965.9 / 1165.9 | 857.5 | 875.3 | 13 |
| 跳过草稿 3 | 29.8 | 90.4 / 93.0 | 18.9 | — | 0 |

第三次正常运行的第 67 个输入（从零计数）在阶段 3381.204 ms 已投递；草稿 Timer 从 3363.046 ms 一直运行到 4238.342 ms。该输入到 4238.744 ms 才开始分发，实际排队 **857.541 ms**，自身同步处理 **22.374 ms**，绘制到 4546.056 ms 才完成。其后尚有一批方向键等待处理，共享后续绘制，因此计划到绘制达到 **1165.855 ms**。该阶段 producer 最大抖动仅约 **2.3 ms**，不能解释这段等待。

最后一对的草稿子阶段显示：全文 materialize **12.8 ms**、UTF-8 编码 **9.2 ms**、正文写入及 `QSaveFile::commit()` **819.4 ms**、元数据准备/写入/提交 **28.0 ms**，外层 snapshotSession **875.1 ms**。这些是嵌套计时，不能把正文提交再与外层保存相加。它们确定了同步文件路径是主要阻塞点，但未进一步区分 OS flush、文件系统、存储或外部文件扫描的贡献。

此前未带这套事件时间线的 376 ms 长尾不能逐事件追溯。本轮复现了同类、幅度更大的排队，并通过回调抑制对照与实际区间重叠确定了这一来源。三次正常保存耗时差异很大，不能保证每次都复现固定数值；也不能据此把所有输入波动归因于草稿。比如正常 1 的输入阶段绘制 p95 为 157.7 ms，此时草稿尚未触发，仍包含其他绘制/调度波动。

## 稳定重建开销中，主题签名比塑形更贵

最后一对上下移动的作用域中位数，时间单位 ms。作用域包含子阶段，不逐行相加。

| 作用域 | 正常 3 | 跳过草稿 3 | 含义 |
| --- | ---: | ---: | --- |
| `layout.replace` | 16.81 | 16.78 | 复用签名检查和新布局构建 |
| `block.formattingSignature` | 9.68 | 9.63 | 当前块及主题的复用签名 |
| `formatting.hashTheme` | 8.48 | 8.43 | 整份 CSS 序列化、变量排序及 SHA-256 |
| `block.buildFresh` | 7.08 | 7.08 | 实际新块构建 |
| `inline.build` | 6.28 | 6.28 | 投影、语义样式、塑形、行内盒 |
| `inline.semanticStyles` | 2.58 | 2.60 | 行内语义样式与格式段 |
| `inline.textLayout` | 1.76 | 1.76 | Qt 文本布局 |
| `inline.boxes` | 1.13 | 1.12 | 行内盒构建 |

`BlockFormattingCache::beginPass()` 清空 `themeSignature_`；接着 `themeSignature()` 重新序列化全部规则、声明、变量和字体定义。主题未改变的连续操作仍重复这一工作，包括 VLOOK 的大变量值/内嵌资源。最后一对每个上下移动阶段都有 **79 次** `formatting.hashTheme`，对应 79 次实际新块构建。普通输入阶段也有 80 次整份主题签名计算。这不是 CSS 匹配器的实际求值，因而此前的“CSS work 为零”不能排除它。

左右移动阶段则正确使用已有布局：80 次检查全部复用，`layout.rebuildBlock` 中位数约 **0.011 ms**，没有执行主题哈希或 `inline.build`。上下移动还可能同时更新离开和进入的块；每阶段 86 次 rebuild 请求中有 7 次复用，不能把 79 次新构建理解成简单的按键命中率。

这给出了明确顺序：先让不可变 CSS 快照的主题签名跨 pass 保留，再判断剩余约 6.3 ms 行内构建中哪些数据可以复用，不能直接把 17–20 ms 全算成塑形或投影算法的成本。

## 菜单刷新重复求编辑上下文

最后一对左右移动阶段，每组 80 次按键各产生 **720 次** `context.fill` / `projection.build`，即每次约 9 次。`currentInlineFormats()` 被调用 **480 次**，分别供加粗、斜体、下划线、行内代码、行内数学、删除线六个 checked predicate 使用。每次查询都会重新求编辑上下文；已有 `CommandContextSnapshot` 缓存了部分基础条件，却没有共享这份行内格式结果。

正常 3 / 跳过草稿 3 的中位数分别为：菜单刷新 **7.42 / 7.58 ms**，predicate 应用 **6.52 / 6.65 ms**，基础 command context 捕获 **0.86 / 0.88 ms**。单次编辑上下文填充约 **0.81 / 0.82 ms**，其中智能标点 QSettings 查询约 **0.51 ms**；单次 `InlineProjection` 构造体内约 **0.09 ms**。`projection.build` 从构造函数体开始，未包含 initializer 中的纯文本展开，不能作为完整构造成本。块源码标签和状态栏更新只有约 **0.003–0.007 ms**，不构成本例的主要开销。

因此优先共享同一次状态刷新的格式结果与编辑上下文，并按设置 generation 读取投影选项，比只优化投影内部循环更直接。不能将这些上下文永久缓存到所有命令中：编辑、选择、设置和文档切换后必须正确失效。

## 诊断边界与下一步

新增 `muffin.perf.detail` 默认关闭。GUI probe 用 `MUFFIN_GUI_TRACE=1` 启用细分计时及时间线，`MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT=1` 仅用于临时基准文档的对照。时间线记录作用域 ≥0.1 ms、Timer/MetaCall/Paint/UpdateRequest handler ≥1 ms、心跳额外延迟 ≥16 ms，最多 16,384 条/阶段。scope 的结束时间在日志 handler 中采样，开始时间由耗时反推，包含少量日志边界误差；事件则在 `notify()` 前后直接采样。父子 scope 和嵌套 Qt 事件不相加。

跳过草稿并不意味着所有事件都在约 90 ms 内真正完成可见变化：例如第 35、71 个方向键自身仅需约 0.04–0.09 ms，没有 ≥0.1 ms 的操作子 scope，下一次绘制被较后的输入共享。现有 probe 未记录光标是否实际变化，因此保留这些样本，并把“无变化输入仍等下一次绘制”列为测量限制。随后应记录 selection/scroll/revision 的前后状态，区分无变化输入、同步变化和真实等待绘制；不能按耗时阈值直接删除样本。

六组上下移动 CSS 实际求值 work 均为零；计数器和预取不是本次草稿排队的证据来源。大文档内存仍约 1.3 GiB，新增 trace 自身也占内存，本轮不据此声明内存优化。完整原始数据同时保留冷/热滚动和主题切换，但本轮仅针对单页交互诊断，未重新覆盖完整 1/10/30 MiB × 1/3 页矩阵。

接下来按证据优先级实现：

1. 草稿异步持久化：先获得安全、不可变且成本受控的文档快照，再在后台编码/写入。按文档串行合并待写 revision，仅在完成确认后记录成功版本；保存、关闭、切页和销毁须协调，防止旧任务在 markClean 后重新写回过期草稿。保留现有恢复与原子单文件提交语义。
2. 主题签名跨 pass 复用：按不可变 CSS 快照身份及 generation 缓存，保留拥有者以避免地址复用；CSS 重载、主题/字体资源变化及块几何依赖仍分别触发对应失效，不能省去节点和资源约束检查。
3. 命令刷新共享上下文：每次刷新只查询一次 `currentInlineFormats`，共享必要的 `BlockEditContext`，将投影设置读取与构造分开。验证源码模式、列表/表格、选区、六种行内格式及设置变化。
4. 补充输入状态变化标记后重复这三对诊断，再回到 1/10/30 MiB × 1/3 页的完整 GUI 对照，分别报告实际变化与原兼容口径。随后继续处理活动页全量主题更新和预取/回收切片边界。

## 验证

六组数据校验通过：窗口、环境、输入节奏、全部绘制覆盖和未截断时间线一致；独立重算输入时钟差与 **540 个分位数**，核对汇总中的完整 raw 与六份输入 JSON 相同，检查对照没有执行草稿快照。诊断日志为 `build/projection-diagnostic-validation.log`、`build/projection-independent-validation.log`。验证工具拒绝时钟不一致、时间线截断及错误分位数的输入；Python/PowerShell 语法、基准 C++ 格式和 `git diff --check` 检查通过。

完整 Release 构建通过，完整 CTest **339/339** 通过（**437.60 秒**），日志为 `build/projection-release-build.log`、`build/projection-release-confirm.log`、`build/projection-all-ctest.log`。未新增性能阈值测试或放宽原断言。`dist` 已刷新，Muffin.exe、MuffinCore.dll、MuffinUi.dll 的 SHA-256 全部与 Release 相同；打包日志和哈希见 `build/projection-dist.log`、`build/projection-dist-hashes.json`。

复现示例（先完成 Release 构建和手动 probe 目标构建，GUI 运行时不并发其他构建/测试）：

```powershell
cmake --build --preset conan-release --target MuffinGuiPerformanceProbe
$env:MUFFIN_GUI_TRACE = '1'
$env:MUFFIN_GUI_PIN_WINDOW = '1'
foreach ($round in 1..2) {
    $env:MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT = $null
    .\scripts\run_gui_performance.ps1 -SizesMiB 30 -TabCounts 1 -OutputDirectory "build/projection-normal-repeat-$round"
    $env:MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT = '1'
    .\scripts\run_gui_performance.ps1 -SizesMiB 30 -TabCounts 1 -OutputDirectory "build/projection-control-repeat-$round"
}
python scripts/summarize_projection_queue.py build/projection-summary.json `
    build/projection-normal-repeat-1/gui-30MiB-1tabs.json `
    build/projection-control-repeat-1/gui-30MiB-1tabs.json `
    build/projection-normal-repeat-2/gui-30MiB-1tabs.json `
    build/projection-control-repeat-2/gui-30MiB-1tabs.json
Remove-Item Env:MUFFIN_GUI_TRACE, Env:MUFFIN_GUI_PIN_WINDOW, Env:MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT
```
