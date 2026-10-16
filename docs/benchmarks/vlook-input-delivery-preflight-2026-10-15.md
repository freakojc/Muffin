# Windows 与 Qt 输入对照：前台预检及测量工具

本轮尚未取得有效输入对照，**已验收 0/12 份计划记录**。Windows 前台已能读到非零句柄，但它不是临时 Muffin 基准窗口。第一次原生保护检查直接停止；第二次等待前台与编辑区焦点稳定，**120 秒超时**。两次均发送 **0** 个按键，不能判断长尾是否也存在于 Windows 原生输入队列。

computer-use 在重置后仍报 `failed to write kernel assets: ... (os error 3)`，无法通过该工具选择和激活基准窗口。未修改桌面或系统策略，也没有向其他窗口投递输入。全部两份失败记录和三份工具功能检查保存在[原始数据](vlook-input-delivery-preflight-2026-10-15.json)。

## 对照已准备的内容

- `MUFFIN_GUI_REQUIRE_FOREGROUND=1` 在阶段计时开始前等待至多 120 秒，要求 Win32 前台、Qt 激活/暴露及编辑区焦点连续稳定 500ms。超时保留记录并结束；原生生产线程仍在每次发送前检查前台。
- `MUFFIN_GUI_TYPING_ONLY=1` 使 Windows 与 Qt 两种方式使用相同的 1 MiB 单页、80 次 `x`、50ms 间隔和结束草稿排空。原生输入是 SendInput 虚拟键，不是物理键盘或 IME。
- off/minimal/detailed 三种模式全部保留计划投递、实际投递、派发、处理结束、paint 开始/结束时刻。新增字段在阶段结束时序列化，不在 off/minimal 输入过程中追加详细事件日志。
- 每个输入记录 `pending_inputs_at_dispatch`、`pending_inputs_after_handler` 和 `subsequent_inputs_before_paint`，区分开始时已经积压、处理期间新到达及绘制前继续处理的输入。这是已观察的基准输入数，不是 Windows/Qt 内部队列总长度。
- 新运行器先做 4 次原生预检，再进行两种投递方式 × 三种记录模式 × 两轮。第二轮反转模式及投递顺序，保留失败和全部长尾；任一运行无效即停止，避免继续产生无效矩阵。
- 新汇总独立检查时钟差值、50ms 计划间隔、积压计数、分位数、文档版本变化和草稿确认。缺少前台观察不能验收；不完整对照明确列出缺失的组合。

前台稳定 500ms 与阶段心跳仍是抽样检查，不能证明两次观察间从未短暂失焦。原生每次发送保护也保留原有检查与发送之间的时间窗口；记录完整性检查用于排除异常，不能宣称软件输入具有物理键盘的全部特性。

## 复现入口

在可用交互桌面的 PowerShell 中，先按 Release preset 构建，再启动对照：

```powershell
cmake --build --preset conan-release --target MuffinGuiPerformanceProbe
.\scripts\run_input_comparison.ps1
```

每个新窗口等待前台就绪后才输入；输出到新的时间戳目录。`comparison.json` 保留原始数据、每份记录的拒绝原因，以及同步处理、投递至派发、处理后等待绘制、输入至绘制的 p50/p95/p99/最大值和积压数量。工作集在最新草稿确认后读取。脚本不覆盖已有结果目录。

有效输入对照完成后，再运行完整 GUI 矩阵：

```powershell
.\scripts\run_gui_performance.ps1 -OutputDirectory build/gui-after-input-comparison
python scripts/summarize_gui_performance.py build/gui-after-input-comparison build/gui-after-input-comparison.json
```

完整矩阵运行器也启用前台等待，拒绝残留的 `NATIVE_INPUT`、`INPUT_ONLY`、`TYPING_ONLY` 环境开关，防止把只有输入阶段的运行当作完整矩阵。本轮没有在已知前台不满足条件时执行六组矩阵。

## 本轮实际验证与限制

手动 GUI 目标重建通过，日志 `build/input-compare-probe-final-build.log`。三种模式各执行了 4 次 Qt 输入的功能检查，均只有 typing 阶段、4 次实际编辑、最新草稿确认；全部新增时钟和积压字段复算一致。它们没有通过前台条件，**不计入延迟分布、性能改善或稳定性验收**。

`python scripts/test_input_comparison.py` 通过三个确定性测试，包括两种投递方式、20 类无效数据及完整/缺失对照集合；其中构造数据仅用于校验器测试，不是测量数据。PowerShell/Python 语法和 C++ 格式检查通过。完整运行器尚未在有效前台执行，原生实际送键及等待后就绪分支仍需交互桌面验证。

产品代码及二进制本轮未改，沿用上一轮完整 Release/CTest **339/339** 基线（444.99 秒），没有重复宣称新的一轮全量测试。分发包三份应用二进制与 Release 哈希仍一致。本轮未 commit、未 push。

后续仍先补有效原生/Qt 对照：若两者都有积压，再细分最坏输入的解析、投影、行内样式、菜单和绘制；若只有合成方式有积压，先修正基准投递模型。当前数据不足以选择其中一个结论。

## 续测：窗口归属与就绪等待

再次预检确认 Qt active/exposed、编辑区焦点都为真，Win32 前台条件仍不成立。发现原有加载后激活重试只看 Qt 状态，因此补上一次普通 `requestActivate()`；该请求在计时前执行，测量中不抢占前台。请求后仍读取到前台句柄 **0**，没有完成送键。新增元数据确认 expected HWND 有效、可见、未最小化，且等于其顶层祖先和当前线程的 active/focus HWND；本次失败不能归咎于把某个子窗口句柄误当作顶层。

前台不可用的具体桌面原因仍未知。续测的两次短预检、一次就绪功能检查及一次完整运行器的失败路径检查均保留在 JSON 的 `followup_preflight`，实际发送仍为 0；加上原记录共保留 9 份完整运行数据，没有新增有效对照。

临时窗口等待时会显示明确标题 `Muffin input benchmark | Windows / off | click editor to start 4 keys`，并在结果路径旁写出 `.ready.json`，记录等待状态、限制和窗口归属。运行器新增 `-ForegroundWaitSeconds`（默认 120，上限 3600），可为人工聚焦留出时间；等待就绪分支不会因此降低激活和焦点要求。1 秒超时功能检查确认标题、就绪文件、句柄关系及零发送；GUI 目标无警告重建通过（`build/input-resume-final-build.log`），校验器测试和脚本语法通过。产品二进制未变。

运行编排统一在 `run_input_comparison.py`，原 PowerShell 脚本作为参数转发入口。可以直接运行以下命令，为人工切到临时窗口留出十分钟；无需修改 PowerShell 执行策略：

```powershell
python scripts/run_input_comparison.py --foreground-wait-seconds 600
```

`.ready.json` 会随状态更新为 waiting/recording/result_written/foreground_timeout，避免结束后仍误报等待。运行器的真实 1 秒前台失败检查正常退出为 1，保留原始数据、拒绝原因、全部 12 个缺失组合，未执行后续运行；该记录只是失败流程验证。六个确定性工具测试通过，覆盖环境隔离、顺序反转、失败即停、旧结果保护及损坏 JSON 保留；其中模拟的 12/12 数据完全不计入真实 GUI 验收。最终手动目标构建日志 `build/input-resume-status-build.log`，工具测试日志 `build/input-resume-runner-unit.log`。

## 较长等待的实际结果

`build/input-resume-live/comparison.json` 已结束，仍为 **0/12**。这次在等待约 **155 秒**后成功取得 Windows 前台，整个 typing 预检也保持前台，因而此次停止原因已经不同于前面的前台超时。

Win32 SendInput 的 4 次按键对提交均返回成功，未触发失焦/发送失败保护；但 spontaneous Qt KeyPress 观测为 **0**，没有原始输入样本，文档版本仍为 **2 → 2**。运行器按不完整输入停止，没有继续十二组对照。输出中的同步/绘制 p95=0 只是空样本的日志默认值，**不是测得了零毫秒延迟**。原始完整对照记录保存在同一诊断 JSON 的 `live_comparison_attempt`。

该 off 记录没有独立统计 Win32 键盘消息的到达情况，因此尚不能判定输入停在哪一层，也不能断言是编辑器吞键或 Windows 投递失败。下一步先核对 WM_KEYDOWN/WM_CHAR 到达、Qt 转发及编辑处理，恢复原生预检；此前 370ms 长尾、有效原生/Qt 对照及六组 GUI 矩阵均未验收。未编辑的预检文档没有生成草稿，ack=0/revision=2 不能单独解释为后台保存故障。
