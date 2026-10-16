# Phycat 资源内容校验：操作内共享与流式摘要

日期：2026-10-16。Windows Release、Qt 6.11.1、offscreen。固定完整 Phycat Abyss、本仓库 VLOOK Fancy Dark 和内置 GitHub。原始数据、资源/二进制摘要、异常及逐对校验见 [JSON](theme-resource-validation-2026-10-16.json)。

## 根因和边界

上一轮已经减少字体准备和 VLOOK 快照计算，但 Phycat 的公共定义读取仍约 **145ms**。`ThemeManager::setTheme()` 校验资源后，同步 `themeChanged` 回调中的 `MainWindow::applyTheme()` 又调用 `currentTheme()` 和 `definition(name)`，重复验证同一个主题。三个读取边界各自正确，却在一次切换中重复读取两份字体及 CSS。

新增默认关闭的打开、读取和摘要 scopes。修改前 Phycat 的一份详细记录中，重复选择 **145.66ms**：摘要 **135.50ms**、读取 **8.58ms**、打开 **0.34ms**；每次完整校验四份资源。模拟主窗口读取链路的切回 **418.40ms**，三次校验合计 **416.80ms**，摘要 **387.96ms**、读取 **24.88ms**。因此主要热点是重复内容摘要，并非目录枚举或磁盘读取。

本轮生命周期基准在同步通知内执行与 `applyTheme` 相同的 editor snapshot/chrome definition 读取，再投影视口。它不创建主窗口、文档布局或测量输入/显示延迟；三主题、1/10/30 MiB 的显示与编号另用文档阶段基准检查。冷选择指新应用进程，未清空系统文件缓存。详细 scopes 相互包含，不能相加当作总 CPU 时间。

## 实现

### 同步选择操作共享已验证定义

`setTheme()` 和 `reloadCustomThemes()` 各自建立栈上的读取作用域。完成一次所选资源校验后，该操作的同步消费者共享同一份不可变主题定义；渲染快照、窗口配色和重载后的列表通知不再重复验证。作用域结束即释放，不跨事件循环持久缓存内容，也不扩大已有主题 LRU。

嵌套选择建立自己的作用域，结束后恢复外层；旧定义通过引用保持有效，不依赖可被 LRU 淘汰的裸指针。后续独立 `definition()`/`currentTheme()`、返回后派发的排队回调以及下一次选择仍执行完整资源内容校验。因此同大小、同时间戳的替换继续失效。未引入按时间窗口、mtime 或文件大小跳过内容校验的快捷路径。

### Windows 系统 SHA-256

新增通用 `themeResourceDigest()`，Windows 使用系统 CNG SHA-256；算法 provider 共享，每次校验拥有独立 hash state。以 **256 KiB** 缓冲流式读取，不保存整份字体，不增加第三方依赖或磁盘缓存。其他平台保留 Qt SHA-256；Windows provider/处理失败时从文件开头完整重试 Qt 路径，不接受部分摘要。缺失/不可读资源保持空标记，正常空文件具有标准 32 字节摘要。

新旧算法均为完整 SHA-256，资源清单语义未改。递归 CSS 导入、字体、图片、缺失依赖出现和资源删除继续参与失效；加载前/后的原有校验仍保留，期间变化的定义不进入持久缓存。没有为省去第二次冷加载校验而弱化这一保证。本轮仅替换资源清单摘要，字体私有家族的内容身份算法保持不变。

## 测量结果

正式生命周期矩阵前后各九个新进程，三主题各三轮；每份进程包含两次重复选择和两次切走/切回的同步回调。下表为关闭详细日志后的中位数，单位 ms：

| 主题 | 冷选择：前→后 | 重复选择：前→后 | 同步切回及 editor/chrome 读取：前→后 |
|---|---:|---:|---:|
| Phycat | 559.46 → 369.08 | 153.16 → 64.38 | 409.13 → 62.45 |
| VLOOK | 142.13 → 131.39 | 2.66 → 1.40 | 14.14 → 7.32 |
| GitHub | 22.56 → 21.12 | 0.14 → 0.31 | 0.92 → 1.21 |

Phycat 每次独立快照/视口读取仍重新验证，重复快照及投影 **140.73→66.21ms**，不是接近零成本。冷选择仍保留资源清单前后两次校验及字体加载；正式新版冷选择有 **508.16ms** 的较慢样本，同步切回最大 **79.04ms**，全部保留。

再用十二个新进程做两轮、三主题的交错 before/after，第二轮反转顺序。Phycat 重复选择 **140.12→68.00ms**、同步切回 **506.15→62.60ms**，支持主要收益可重复。VLOOK 同步切回 **17.25→9.03ms**；冷选择却是 **148.80→179.06ms**，不宣称其冷加载稳定提速。GitHub 无自定义资源校验可省，正式重复选择增加约 **0.17ms**，不宣称所有主题统一改善。

最终详细 Phycat 重复选择 **58.85ms**，其中摘要 **51.09ms**、读取 **6.49ms**；同步切回 **59.25ms**，只执行一次校验 **57.86ms**。另一份切回 **74.96ms** 也完整保留。新旧两种 callback scope 的校验次数逐项检查为 **3→1**；新版未进入 Qt fallback。真实应用仍有文档准备、排版、绘制和窗口配色成本，这些时间不包含在约 62ms 中。

正式前后各十八份完整文档记录，三主题 × 1/10/30 MiB × 两轮；另有前后各三份详细文档记录。每份初始和两次切回均保存像素摘要、块几何、四处光标矩形/命中。共 **63 组**前后对照完全一致，独立解码 **42 张**最终 PNG 并复算 BGRA SHA-256；全部进程完成独立全量计数器值和 generated-content fingerprint 对照。

合计采用 **78 份**有效进程：生命周期正式 18、详细 6，完整文档正式 36、详细 6，交错 12。另保留 **45 份**早期完整记录；它们在最终源码格式整理/全量重新链接之前测量，最终二进制记录替代其计时统计。没有产品算法的中间差异，仍不混合不同二进制记录。最终测量二进制与 Release 的摘要一致。

## 回归与内存

- 标准空文件/`abc` SHA-256 向量，以及独立 Qt 参考的二进制数据；覆盖 SHA 分块边界、256 KiB 流式边界、超过 1 MiB 的资源、中间字节变更和中文路径。
- 实际 Qt 内置资源、缺失与重建、目录不可读标记。完整字体用两份同大小的有效私有 SFNT 替换并恢复时间戳，验证独立读取更换 sourceSheet 和私有家族身份；旧快照不变。
- 同步回调中修改 CSS 时，editor/chrome 继续一致使用本次选中的定义；操作返回后立即发现修改。排队通知、嵌套选择、重载及列表通知的作用域边界分别回归。
- 既有深层导入、缺失图片出现、资源编辑、JSON、缓存淘汰、媒体环境和 native/fractional 字体测试继续运行；导入 CSS 的回归进一步保留文件大小和时间戳。

没有增加跨操作的文件内容缓存。操作内只暂存主题定义的共享值；每份资源的临时读取缓冲最多 256 KiB。主题缓存与字体注册预算未改。生命周期最后切回后的采样工作集（MiB）：Phycat **55.65→56.02**，VLOOK **51.73→52.26**，GitHub **27.30→27.82**；30 MiB 文档最后绘制后分别 **1159.08→1157.74**、**1161.63→1162.90**、**1107.35→1108.96**。没有一致的总内存下降，不把读取字节或 SHA 调用减少解释为驻留内存节省。此处不是完整 GUI/后台草稿排空后的内存验收。

## 复现与验收

```powershell
cmake --build --preset conan-release --target MuffinThemePerformanceProbe
python scripts/run_first_layout.py build/theme-resource-check --lifecycle --sizes 1 --repetitions 3
python scripts/run_first_layout.py build/theme-resource-detail --lifecycle --sizes 1 --repetitions 1 --detail
python scripts/run_first_layout.py build/theme-resource-visual --real-switch --repetitions 2
```

输出目录必须尚不存在；需要完整本机 Phycat Abyss CSS、导入文件与两份字体。生命周期记录包含两次同步读取回调，完整文档记录保留三次图像/编辑几何及独立全量编号参考。有效测量期间不并行构建、测试或其他基准。

初次构建时自动 include 排序将 bcrypt 放在 Windows 类型头文件之前，已修正依赖顺序并成功重建，失败日志保留在 `build/theme-validation-focused-build.log`。三项字体/主题重点测试 **3/3** 通过（**7.33 秒**），见 `build/theme-validation-focused-ctest.log`。

完整 Release 构建通过，完整 CTest **339/339** 全绿（**510.61 秒**），包括 native/fractional 字体、浏览器/像素、主题重载及 Mermaid 回归；见 `build/theme-validation-release-build.log`、`build/theme-validation-all-ctest.log`。独立核对全部 78 份原始记录，复算 **72** 项正式/交错统计，见 `build/theme-validation-independent-audit.log`。新增 C++ 范围格式、Python 语法和 `git diff --check` 通过。

dist 已刷新，Muffin.exe、MuffinCore.dll、MuffinUi.dll 与 Release SHA-256 一致；测量二进制与最终 Release 一致，固定 CSS/字体摘要未变。见 `build/theme-validation-dist.log`、`build/theme-validation-dist-hashes.json`、`build/theme-validation-final-verification.log`。本轮未 commit、未 push，没有恢复原生键盘长尾调查。
