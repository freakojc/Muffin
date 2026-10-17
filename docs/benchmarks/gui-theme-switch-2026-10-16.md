# Windows GUI 主题切换验收（2026-10-16）

**续测已完成：** 用户恢复桌面后完成 **12/12 正式进程和 2/2 关闭诊断对照**，见[续测结果与剩余热点](gui-theme-switch-completed-2026-10-16.md)。以下保留首次尝试的状态和失败原因，不代表最新完成状态。

本轮已完成测量工具和短预检，**完整 GUI 验收尚未完成**。最终工具的正式矩阵为 **0/12**；早期探索运行有三份通过当时的前台校验，但没有最终新增的整视口绘制和精确光标回查字段，不混入正式结果。不据此报告性能改善或完整稳定性通过。

## 验收方法

使用 `MuffinGuiPerformanceProbe` 的实际 `MainWindow`、`DocumentTab` 和主题菜单 QAction 绑定，Windows QPA，可见原生窗口。设置、工作文件及复制的主题资源隔离在每次运行独有的目录，用户主题文件不被修改。初始主题为 Night；每轮依次切换固定 VLOOK Fancy Dark、内置 GitHub、Phycat Abyss。

目标矩阵是 **1/10/30 MiB × 单页/三页 × 两次新进程**，每进程六轮、十八次主题操作。首次选择与重复选择分开，保留原始时钟和全部异常样本，使用 nearest-rank p50/p95/p99/max。重复选择每主题每尺寸/页数共十个样本，属于有限样本的经验分位数，不代表总体长尾。

输入投递是独立线程按 1500ms 间隔向 Qt 投递的标准菜单操作；后续按键只用于切换/切页后的编辑功能检查，**不作为原生键盘性能验收**。时间终点是 QWidget backing-store 绘制完成，不是 DWM 或屏幕实际呈现。

最终工具增加以下检查：

- 每个阶段开始前至少 500ms 稳定前台、激活、可见及编辑区焦点；测量过程中持续检查激活/前台/可见状态。
- 主题完成必须是覆盖整个编辑视口的 Paint，局部光标脏矩形不计为首屏完成。
- 分开记录投递、派发、处理结束、绘制开始、绘制完成；每次操作保留 CSS、计数器、布局、背景、阴影和文字的独占阶段时间，以及最小范围的具名 scope 区间。具名 scope 是包含子阶段的时间，不能跨嵌套求和。
- 每次切换确认文档版本和选区不变；隐藏页保留原样式表、布局高度及文档版本，主题延后应用，隐藏页不得绘制。
- 三页模式检查延后主题的切页、随后的输入及重复切页；最终请求并等待所有修改页的异步草稿确认。
- 计时及草稿排空后的内存检查完成后，另做三主题两轮截图、可见块/行内光标几何、字体和颜色检查；实际可见光标回查须返回偏移 5，并与独立主题实例的全量计数器/生成内容对照。
- 二进制及固定 CSS/字体资源在运行前后进行 SHA-256 校验。工作集按 20ms 采样，既不是堆大小，也不保证覆盖瞬时峰值。

字体样例包含中英文、标题、粗体、斜体、删除线、行内代码、链接、kbd、行内数学、引用、嵌套有序列表及任务列表。重复几何检查验证主题切换时的失效行为，不等同于重新完成所有 CSS 与浏览器及主题参考渲染的像素兼容验收。Phycat 保留动画，截图像素可随动画时刻变化，不能将这种变化直接判定为缓存错误。

## 实际进展与阻塞

最初的短预检取得有效 Windows 前台、激活、可见及编辑区焦点；单页和三页探索样例通过当时的重复几何、字体/颜色、全量编号对照和草稿排空检查。人工查看 VLOOK、GitHub、Phycat 截图，能区分三套主题的正文/标题/行内样式，VLOOK 网格和标题条、Phycat 发光与行内装饰可见。

早期完整运行在第四个组合（1 MiB 三页）的 `theme_switch` 阶段失去激活和前台，运行器保留四份原始记录后停止，前三份通过当时校验。最初三份期间进行了工具校验/图像分析，也仅作为探索记录保留。

在用户确认可保持前台后，最终工具重新运行。1 MiB 单页在 `theme_switch` 期间失去前台，后续 `typing_after_theme` 等待就绪超时，正式矩阵停止为 **0/12**。结果中：

| 观察项 | 实际值 |
| --- | --- |
| Win32 foreground HWND | `0` |
| Qt active / editor focus | `false` / `false` |
| 窗口有效、可见、未最小化 | `true` / `true` / `true` |
| 窗口 exposed | `true` |
| OpenInputDesktop | 失败，Win32 error `5` |
| thread desktop / window station | `Default` / `WinSta0` |

输入桌面访问被拒绝、前台句柄为 0 的具体原因尚未确认；不能仅据此断言是用户切窗、锁屏、UAC 或产品问题。已请求用户手动返回普通桌面后续测，没有绕过安全桌面、修改系统设置或降低前台有效性要求。

computer-use 技能已读取并尝试初始化，但服务返回 `failed to write kernel assets: The system cannot find the path specified. (os error 3)`，没有取得可控制的窗口。GUI 测量使用项目自身的 Qt 基准窗口，没有用其他 UI 自动化模拟前台通过。

此次**没有**完成关闭诊断的 GUI 对照、完整六组分位数及内存验收；没有恢复暂缓的原生键盘长尾调查。[全部探索/失败原始记录](gui-theme-switch-2026-10-16.json)完整保留，不用缺失样本的零值代表延迟。

## 工具与复测

```powershell
cmake --build --preset conan-release --target MuffinGuiPerformanceProbe
python -B scripts/run_gui_theme_acceptance.py --output-directory build/gui-theme-acceptance-resumed --cycles 6 --repetitions 2
python -B scripts/run_gui_theme_acceptance.py --output-directory build/gui-theme-acceptance-off --cycles 6 --repetitions 2 --profile off --pilot
python -B scripts/summarize_gui_theme_acceptance.py build/gui-theme-acceptance-resumed docs/benchmarks/gui-theme-switch-completed.json --off-control build/gui-theme-acceptance-off
```

默认使用用户已有的 `phycat-abyss.css` 及其递归依赖副本；可用 `--phycat` 指定同一固定样例的其他位置。输出目录必须是新目录，避免覆盖失败记录。正式运行不并行执行构建、测试、其他基准或图像分析；若失去前台或草稿未排空则停止，保留原始数据和拒绝原因。

手动目标最终构建通过（`build/gui-theme-acceptance-final-build.log`）；两份 Python 工具语法和十四种无效数据拒绝检查通过（`build/gui-theme-acceptance-validator-checks.log`）。拒绝检查使用构造的校验器输入，不计入测量结果。`git diff --check` 通过。

产品源码和应用二进制本轮未变；Muffin.exe、MuffinCore.dll、MuffinUi.dll 与上一轮完整 Release/CTest **339/339**（454.68 秒）所验证的 SHA-256 一致，沿用该基线，没有宣称本轮重跑全量测试。dist 与 Release 仍一致。本轮未 commit、未 push。
