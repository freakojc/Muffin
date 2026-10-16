# 行内分隔符后处理：稳定索引与延迟物化

本轮接续打开性能调查，优化 `==`、`~~`、`~`、`^` 共用的分隔符后处理。旧实现每次配对都会删除 token 数组中的一个区间，反复搬移尚未处理的后缀；token 构建阶段还会提前创建文本节点。30 MiB GUI 定位样本中，约 34.5 万次配对搬移了约 2,449 万个 token，后处理约 1,057ms，数组删除本身约 221ms。

## 实现

- token 使用稳定索引的前后连接。完成配对后替换 opener 并连接后继，直接跳过已消费区间，不搬移后缀。后续 flank 检查继续看到最新的相邻节点，保留原来的嵌套及空白规则。
- 普通片段借用 `QStringView`，不在每个 token 中嵌入 `InlineNode`。完成的格式节点存入独立临时数组；仅在配对和最终提交时物化文本、移动不变的子树。
- 输入 inline 数组在匹配期间保持稳定；分片视图依赖的原始 Text 不被提前移动。没有成功配对时不提交，保留原节点及所有范围。成功提交时，普通 Text 的 source/content 范围按旧规则重建。
- 匹配过程中维护活动 token 数量，最终输出按此预留容量；临时数据在每个块处理完成后释放，不增加文档级缓存。
- `MUFFIN_DELIMITER_PROFILE=1` 开启聚合诊断，分别记录候选、token 构建、匹配、子节点物化、区间更新和提交，以及 token/配对/搬移数量。正常解析不读取逐块或逐 token 时钟。`match_ms` 包含 `materialize_ms`、`compact_ms`，不可重复相加。

## 交错阶段基准

使用同一个 `MuffinDelimiterPerformanceProbe.exe`，分别加载保存的旧版与最终新版 Core DLL。每次启动新进程，奇数轮先旧后新、偶数轮反序；三轮，每种条件各三份，合计 30 份。关闭既有逐行内诊断，仅收集粗粒度解析阶段。造文本及释放 AST 均不计入解析时长。它测量解析工作，不代表 GUI 显示延迟。

| 文本 / 每段重复数 | 旧删除线后处理中位数 | 新中位数 | 旧总解析中位数 | 新总解析中位数 |
| --- | ---: | ---: | ---: | ---: |
| 1 MiB / 12 | 38.88ms | 19.62ms | 168.21ms | 150.14ms |
| 10 MiB / 12 | 333.76ms | 162.65ms | 1472.29ms | 1305.40ms |
| 30 MiB / 12 | 1052.56ms | 499.88ms | 4520.30ms | 4060.31ms |
| 1 MiB / 192 | 161.67ms | 16.96ms | 293.76ms | 135.68ms |
| 1 MiB / 3072 | 2527.37ms | 29.47ms | 2698.84ms | 190.78ms |

普通段落的删除线阶段下降约 50%–53%；总解析仍受 cmark 和 AST 适配限制。长段落样例用于暴露反复搬移的增长，不能当作所有文档的统一加速倍数。全部样本，包括较慢样本，见 `build/delimiter-interleaved/summary.json`。

另八份开启详细诊断的交错样本位于 `build/delimiter-details/summary.json`；它们包含额外时钟成本，独立于上述正式阶段基准。30 MiB 对应约 **417 万个 token、34.5 万次配对**：token 构建约 **468→164ms**，匹配约 **409→165ms**，其中区间更新约 **221→6ms**，搬移计数 **2449.9 万→0**。提交约 **143→211ms**、子节点物化约 **43→75ms**，部分工作被延后，最终阶段总量仍下降；不把构建阶段的全部下降当作净收益。新版 Windows x64 临时 token 为 80 字节。长段落样例旧版还搬移约 2.26 亿个 token，新版为零。

最初 GUI 定位前后两份记录为 `build/delimiter-before-profile.json`、`build/delimiter-after-profile.json`；后一份 AST 适配明显变慢，整体打开反向波动，不能用这一对证明整体打开提速。

## 真实 GUI 验收

六组 **1/10/30 MiB × 单页/三页**，修改前每格一份正式进程，修改后每格两份，各另两份关闭打开诊断的对照及一份启动样例，共 24 份 GUI 进程。全部实际 Windows 前台、Qt 激活及可见检查通过。前后主矩阵为顺序测量，并未交错；每次新进程，三页行合并三次打开，原始样本全部保留。以下为新文档安装完成后的首次完整视口 QWidget Paint，并非 DWM 呈现或原生键盘延迟。

| 文档 / 标签页 | 旧打开至首屏中位数 | 新中位数 | 旧删除线阶段中位数 | 新中位数 |
| --- | ---: | ---: | ---: | ---: |
| 1 MiB / 1 | 319.66ms | 330.18ms | 33.99ms | 18.95ms |
| 1 MiB / 3 | 279.57ms | 269.02ms | 33.52ms | 16.89ms |
| 10 MiB / 1 | 2056.63ms | 2051.06ms | 342.13ms | 185.17ms |
| 10 MiB / 3 | 2048.07ms | 1832.73ms | 376.07ms | 169.68ms |
| 30 MiB / 1 | 6066.91ms | 5686.17ms | 1031.39ms | 506.54ms |
| 30 MiB / 3 | 6020.60ms | 5703.67ms | 1215.67ms | 553.31ms |

30 MiB 总等待下降约 **5%–6%**，依然约 5.7 秒；cmark、AST 转换和共享文本准备仍存在。GUI 安装没有统一下降，30 MiB 三页 **251→269ms**，本轮改的是后台后处理。1 MiB 单页整体反向波动，关闭诊断的 1 MiB 三页对照也没有一致改善；不宣称每种文档或交互都提速。主题切换、连续输入、光标、滚动及切页仍保留全部时长，包含新版一次约 **681ms** 的主题切换样本，不重启已暂缓的原生键盘长尾调查。

### 显示与状态

VLOOK、GitHub 和 Phycat 的字体、块几何及光标命中前后 **84/84** 一致，核对全量计数器及生成内容、隐藏页无绘制、切页后输入。静态审计仅在计时结束后发送标准 Qt Leave；不移动系统鼠标，不更改计时阶段交互。

按对应轮次比较，像素 **81/84** 完全一致；另外三对来自两份旧版记录的第二轮 Phycat 截图变化，这两份基线自身的重复截图也不同。新版 **45/45** 重复像素一致；所有 24 份进程合计 **70/72** 重复一致。另用各主题已通过全量编号核对的首轮基线作为固定参考，新版全部 **84/84** 像素一致。两种参考规则和原始差异均保留，不删除基线漂移记录。

独立解码 **168 张 PNG**，复算 **2790 项统计、4500 个时钟差和 54 条打开时间线**。详情见[全部记录和审计](inline-delimiters-2026-10-16.json)，审计脚本为 `build/audit-inline-delimiters.py`、日志 `build/delimiter-audit.log`。

### 内存

文档/AST 对象数、共享文本、最终布局及可计数字符载荷前后相同。30 MiB 三页最后一次打开的采样工作集峰值约 **2774.75→2775.64–2776.39 MiB**；草稿排空后 **2637.12→2637.63–2637.88 MiB**，总体持平。所有原文档页关闭后对象已销毁，5 秒工作集 **355.32→354.94–355.83 MiB**，私有提交 **254.92→256.32–257.99 MiB**。不把临时 token 缩小等同于整体峰值下降，也不以这组短生命周期数据证明没有长期泄漏。

峰值按 20ms 采样，可能漏掉短峰；对象清点不包括分配器、Qt 和 OS 全部成本。大峰值仍发生在 cmark/AST 阶段，先于分隔符后处理。

## 语法与坐标

用旧版 DLL 生成固定语料指纹，再让新版运行同一个测试程序。273 份语料 × 16 种开关组合，共 4,368 次解析，比较块结构、行内类型/文本/元数据/全部范围及 Markdown 序列化。包括嵌套、相邻及不同长度的标记、转义、实体、代码、链接、HTML、数学、表格、中文、emoji 和长段落。旧新 SHA-256 均为：

`697eed90533d805976e8263c078a298686f8f92f149f4040275d497117504d00`

指纹已固定在 `SubscriptSuperscriptParseTest`，另外直接检查嵌套内外删除线的源码区间。完整 Release 构建、重点 **6/6（8.72 秒）**及全量 **339/339（461.57 秒）**通过，日志分别为 `build/delimiter-release-build.log`、`build/delimiter-focused-ctest.log`、`build/delimiter-all-ctest.log`。GUI 测量与构建/CTest 分开执行。

dist 已刷新，`Muffin.exe`、`MuffinCore.dll`、`MuffinUi.dll` 与最终 Release SHA-256 一致；正式 GUI 和交错解析测量的二进制、固定 CSS/字体摘要保持一致。见 `build/delimiter-probes-dist-build.log`、`build/delimiter-final-verification.json`。Python 语法及 `git diff --check` 通过。本轮未 commit、未 push。

## 复现

```powershell
cmake --build --preset conan-release --target MuffinDelimiterPerformanceProbe MuffinGuiPerformanceProbe
python -B scripts/run_delimiter_performance.py --before build/delimiter-before-bin --after build/Release --output build/delimiter-interleaved-new --repeats 3
python -B scripts/run_gui_theme_chain.py --output-directory build/delimiter-gui-new --cycles 4 --repetitions 2 --memory-census --static-visual-audit --open-profile stages
```

旧版目录需要保存对应 DLL 和同一份解析 probe。GUI 测量要求实际前台、Qt 激活和可见，关闭构建及 CTest；静态视觉规范化只发生在计时之后。矩阵文件刚生成即打开，文件缓存温热，不代表冷盘读取。
