# 字体内容摘要统一与主题切换整体对照

日期：2026-10-16。Windows Release、Qt 6.11.1、offscreen、960×900、DPR 1。固定完整 Phycat Abyss、本仓库 VLOOK Fancy Dark 与内置 GitHub，1/10/30 MiB 文档。全部样本、资源/二进制摘要和逐项核对见 [JSON](font-content-digest-2026-10-16.json)。

## 结果与实现

本轮把解码字体的内存 SHA-256 接入上一轮的系统摘要后端。详细样本中 Phycat 内容摘要 **123.06→51.23ms**，字体准备/注册 **168.53→96.97ms**；独立 ThemeManager 生命周期矩阵冷选择 **342.39→268.79ms**。首次可见绘制与重复主题切换未取得统一改善，主要收益发生在冷字体准备。

新增 `themeContentDigest(QByteArrayView)` 与文件校验共用 CNG SHA-256 provider、独立 hash state 和 finish 路径。调用只借用内存视图，按最多 256 KiB 的切片输入 CNG，不复制或保留整份字体，也不会将超过 ULONG 的长度直接截断。空输入仍返回标准 SHA-256。其他平台使用 Qt；Windows 原生后端失败时丢弃部分状态，Qt 从完整输入重新计算。

字体解码位于 MuffinCore，因此共享实现、导出和 Windows bcrypt 链接依赖从 MuffinUi 移到 Core，避免 Core 反向依赖 Ui。文件资源验证继续使用同一后端；没有修改原有按操作共享验证的范围，也没有新增内容缓存。字体私有家族名称及注册键的小摘要保持原算法和输入顺序，内容 SHA-256 的结果完全相同，原有字重、斜体、回退与内容失效语义保留。

通用 `prepareThemeFont()` 还服务于内置 Mermaid 字体。VLOOK 首次文档准备的一份详细样本中，四份共同字体的内容摘要 **85.78→34.32ms**，第一次行高度量 scope **211.73→152.04ms**。这说明其他主题的新进程首次准备也可能受益；它不是每次切换都会发生的成本。未改变字体度量、换行或排版算法。

## 整段计时方法

手动 probe 新增 `--manager-document`：先建立已有 Markdown 文档，再从 ThemeManager 选择所选主题，在同步通知中读取 editor snapshot 和 chrome definition，投影视口；随后惰性文档准备、首次可见排版并绘制。两次切走/切回继续使用 ThemeManager、独立快照与文档语义交接，增加从选择开始到绘制结束的连续计时。

连续计时包含阶段间的诊断记录和测量记账，不只把分项相加。已核对 **162** 条连续计时均覆盖相应四个阶段。它没有创建真实主窗口或记录 Qt 事件派发、窗口系统显示、输入延迟；不能将下表当作真实 GUI 手感验收。窗口多标签页的正确性由真实 MainWindow 的 offscreen 集成测试另行覆盖。

冷指新应用进程，未清空系统文件缓存。计时前完成文档解析、主题列表扫描；GitHub 因为默认已选中，会先选 Night，保证首次选择有通知。所选主题仍冷加载，但这一组提前注册了内置 Night 共用的部分资源，不能把它称为系统全部字体的完全冷启动。首次文档准备包含本进程第一次字体/度量初始化；真实编辑器的已有窗口切换还应与下表重复切回分开理解。

修改前后使用同一个最终 probe EXE，修改前保留本轮开始时 Core/Ui DLL 的原始摘要，内部已有 API/结构布局未改变。新内存摘要 API 仅被新版 Core 内部调用，probe 不使用新增符号。两组都嵌入相同内置 CSS/字体。有效测量期间不并行构建、测试或其他基准。详细 scopes 为 inclusive，不相加解释成 CPU 时间。

## 正式矩阵

前后各十八个新进程，三主题 × 三尺寸 × 两轮。每格冷选择两个样本、重复切回四个样本，单位 ms，中位数：

| 主题 | MiB | 冷选择及快照/投影：前→后 | 冷选择至首屏绘制：前→后 | 重复选择至首屏绘制：前→后 |
|---|---:|---:|---:|---:|
| Phycat | 1 | 385.56 → 318.92 | 686.71 → 612.40 | 149.59 → 149.89 |
| Phycat | 10 | 372.81 → 307.52 | 754.69 → 632.25 | 160.52 → 160.17 |
| Phycat | 30 | 381.37 → 335.07 | 958.80 → 882.76 | 200.10 → 204.97 |
| VLOOK | 1 | 255.36 → 206.13 | 587.80 → 501.13 | 113.69 → 105.38 |
| VLOOK | 10 | 203.83 → 188.30 | 580.13 → 521.28 | 118.12 → 108.03 |
| VLOOK | 30 | 205.01 → 201.15 | 759.08 → 697.82 | 155.62 → 146.37 |
| GitHub | 1 | 35.13 → 35.12 | 291.20 → 240.14 | 41.75 → 40.58 |
| GitHub | 10 | 35.38 → 38.97 | 349.81 → 321.67 | 43.33 → 44.69 |
| GitHub | 30 | 37.83 → 39.32 | 522.00 → 443.19 | 59.21 → 57.71 |

在此模型中 Phycat 的冷选择/快照/投影下降约 **46–67ms**；整段下降约 **74–122ms**，包括共同字体第一次初始化的变化及系统波动。重复切回没有统一下降，Phycat 30 MiB **200.10→204.97ms**、GitHub 10 MiB **43.33→44.69ms** 等反向结果完整保留。

首次可见排版、绘制分别保留分项，不把字体摘要收益转移到下游后仍称成功。例如 Phycat 1 MiB 首次排版 **52.19→54.39ms**、绘制 **39.36→39.36ms**；30 MiB 排版 **58.11→63.89ms**、绘制 **41.99→42.48ms**。VLOOK 1 MiB 首次排版 **63.00→80.10ms** 有较慢样本，完整保留；其他格没有统一上升或下降。结合稳定下降的字体摘要与整段计时，没有发现节省的主要成本转移到排版/绘制，但不能宣称消除阶段波动。

### 交错与独立生命周期

再用十二个新进程，三主题 × 1/30 MiB × before/after，逐对反转顺序：

| 主题 | 冷选择及快照/投影：前→后 | 冷选择至首屏绘制：前→后 | 重复选择至首屏绘制：前→后 |
|---|---:|---:|---:|
| Phycat | 391.87 → 327.25 | 851.87 → 722.72 | 175.77 → 171.23 |
| VLOOK | 233.73 → 243.68 | 698.11 → 658.77 | 129.33 → 128.94 |
| GitHub | 35.89 → 36.92 | 389.75 → 350.92 | 46.62 → 49.24 |

Phycat 冷加载收益仍可复现；VLOOK 选择阶段有反向波动，没有将其他主题的冷选择全面提速归因于本轮。

独立生命周期前后各六份新进程，不建立文档：Phycat 冷选择 **342.39→268.79ms**；重复选择 **60.06→61.33ms**、重复快照及投影 **61.43→65.37ms**。这些重复操作不再准备字体，所以本轮没有取得提速证据。VLOOK 冷选择 **117.60→124.06ms**；GitHub **21.04→20.02ms**，均保留。

## 显示、失效、隐藏页与内存

正式十八对、直接加载详细三对、管理器详细三对、交错六对，每对三次快照：**90 组**前后像素、块几何及四处光标矩形/命中完全相同，各进程两次切回与首次一致。独立解码 **60 张**最终 PNG 并复算 BGRA SHA-256；全部文档进程完成独立全量计数器值和 generated-content fingerprint 对照。

新增内存摘要回归检查标准空输入和 `abc`、独立 Qt SHA-256、SHA 分块和 256 KiB 切片边界、非对齐子视图、范围外数据不参与摘要、源字节不可变以及内容修改后的摘要变化。既有解码字体摘要对照、字重/斜体、中英文/回退、同大小同时间戳的字体与 CSS 替换、旧快照不可变继续验证。

`MuffinDocumentTabsTest` 的隐藏页回归通过：非活动页保留旧布局和 pendingTheme，连续切换合并为最后主题，激活只应用最新快照，文档 revision 和光标身份不变。此处为功能验证，没有新增大文档多标签页的真实 GUI 延迟或草稿排空测量。

没有新增磁盘或跨操作缓存。内存摘要仅借用输入，provider 共享、hash state 按调用释放，文件校验仍最多 256 KiB 临时缓冲。30 MiB 最后切回绘制后的工作集（MiB）：Phycat **1160.59→1160.99**、VLOOK **1166.43→1165.93**、GitHub **1109.50→1111.71**。没有一致的总内存下降；阶段采样不是峰值或完整 GUI 内存验收。

## 复现、样本与验收

```powershell
cmake --build --preset conan-release --target MuffinThemePerformanceProbe
python scripts/run_first_layout.py build/font-content-matrix --manager-document --repetitions 2
python scripts/run_first_layout.py build/font-content-detail --real-switch --sizes 1 --repetitions 1 --detail
python scripts/run_first_layout.py build/font-content-lifecycle --lifecycle --sizes 1 --repetitions 2
```

输出目录必须尚不存在，使用完整本机 Phycat Abyss 资源。采用 **72 份**有效进程：正式 36、直接加载/管理器详细各 6、独立生命周期 12、交错 12。第一次新增基准错误比较原始定义与独立投影的 stylesheet 身份，首个 VLOOK 切回即停止；改为比较原始 ThemeDefinition 身份后重新采集，没有采用不完整样本。初次构建也发现摘要组件在 Ui 导致 Core 反向链接，移至 Core 后恢复正确依赖。失败日志与原因保留。

最终完整 Release 构建通过，四项字体/主题/标签页重点测试 **4/4**（**10.60 秒**），见 `build/font-digest-release-build-final.log`、`build/font-digest-focused-ctest.log`。完整 CTest **339/339** 全绿（**454.68 秒**），包括 Mermaid 字体/像素、native/fractional 字体、主题重载及隐藏标签页回归，见 `build/font-digest-all-ctest.log`。

全部 72 份原始文件、**234** 项统计、资源及二进制摘要独立复核通过，见 `build/font-digest-independent-audit.log`；新增 C++ 范围格式、Python 语法及 `git diff --check` 通过。dist 已刷新，Muffin.exe、MuffinCore.dll、MuffinUi.dll 与 Release SHA-256 一致，测量版与最终 Release 一致，见 `build/font-digest-dist.log`、`build/font-digest-dist-hashes.json`、`build/font-digest-final-verification.log`。

本轮完成可重复阶段基准和 offscreen 窗口功能验收，真实桌面的主题切换显示延迟仍需后续单独验证。本轮未 commit、未 push，未恢复原生键盘长尾调查。
