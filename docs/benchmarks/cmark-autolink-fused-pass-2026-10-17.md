# autolink 候选范围与合并遍历

本轮将邮箱候选处理复用到核心的相邻文字合并遍历，减少重复全树遍历，并避免普通文字缓冲区的重复复制。自动链接功能、邮箱识别和标点规则保持原样。最终数据与独立审计见 [JSON](cmark-autolink-fused-pass-2026-10-17.json)。

## 热点与实现

修改前独立诊断发现，30 MiB 富行内样例的 autolink 后处理会先再次调用 `cmark_consolidate_text_nodes`，随后访问 **4,917,720** 次节点事件，并为 **2,099,220** 个文字节点执行文字后处理；其中邮箱候选为 **0**。该次细分为重复合并 **144.40ms**、文字处理 **162.21ms**、整个后处理 **504.21ms**，其余约 **197.60ms** 包括遍历、判断和计时/计数开销。逐文字计时会扰动结果，这些细分只用于定位，不作为产品收益。

`http/https/ftp/www` 的识别位于行内解析阶段，后处理负责邮箱。新实现包含：

- 私有的 `cmark_consolidate_text_nodes_with_callback` 在合并完当前相邻文字后，向访问器提供已有链接外的文字。公开的普通文字合并函数保留原有语义。
- 扩展可声明私有 `postprocess_text_func`，完整替代其第一次全树后处理。核心仅允许第一个后处理调用复用合并遍历，不判断扩展名称。如果更早的扩展已可能修改 AST，仍调用原有完整后处理。
- 同一个扩展对象重复挂载时，只合并第一次调用；更晚的调用仍按原顺序运行，能处理中间扩展新增的文字。
- 候选判断在实体解码、脚注整理和相邻文字合并之后进行，只有包含 `@` 的文字进入邮箱识别。因此 `a&#64;example.com` 和拆成相邻文字节点的地址不会被原文粗筛漏掉。代码、数学和已有链接继续使用原先的节点边界。
- 没有候选列表、逐节点标记或全文缓存。普通文字仍在解析内按需取得 owned/C-string 缓冲区；已经 owned 的文字不再被无意义地分离和复制，借用的文字仍在这一阶段完成分配。不会把成本移到之后的 `cmark_node_get_literal()`。

新访问器上下文位于解析栈上，扩展定义不可变，不引入产品全局可变状态。修改涉及私有扩展结构，核心、扩展和消费者统一重建；兼容说明在 `third_party/cmark-gfm/MUFFIN-PATCHES.md`。

## 语义与生命周期验证

修改前冻结普通静态库与阶段诊断程序，以及旧的 `blocks.c`、`iterator.c/.h`、`syntax_extension.h`、`autolink.c` 和生成诊断源，位于 `build/autolink-baseline`。这一基线已经包含上一轮脚注快速路径，不能与更早的未优化脚注版本混比。provenance 保存输入、源文件和二进制 SHA-256。

固定语料 **217** 份，包含 128 份固定种子组合。每份分别使用生产扩展、先修改树的扩展、重复挂载 autolink、关闭 autolink，整段/逐字节 feed，连续复用解析器，共 **1736** 份快照。修改前三次重复运行稳定，新版本逐项一致。

比较包括树进入/退出顺序、节点类型、文字、链接、标题、源码行列、标题/列表属性、脚注序号和关联，以及完整 HTML。语料覆盖网址、邮箱、mailto/xmpp、协议大小写、中文、标点与括号、无效域名/邮箱、实体和转义、软换行、CRLF、NUL、代码、HTML、数学、图片、已有链接、任务和表格、脚注，以及相邻解码文字合并。

另有“autolink → 修改树的扩展 → 再次 autolink”的回归检查，确保后一次挂载识别新增的 `injected@example.com`。在调用 `get_literal()` 之前直接检查已有链接外文字的缓冲区所有权，防止性能测量通过延后分配获得表面收益。636 组既有脚注 oracle 也继续执行。

## 测量方法

正式矩阵使用最终代码，三轮新进程交错/反序运行 **132** 份 cmark 样本：1/10/30 MiB 富行内、各尺寸末尾含真实/实体邮箱、30 MiB 普通正文，以及 1 MiB 扩展混合、无效 `@` 候选、真实脚注和未定义引用。每组运行冻结普通库、冻结细分诊断、新普通库、新细分诊断，全部产品扩展保持开启。正式原始目录为 `build/autolink-performance-attachments`；早期 132 份探索矩阵在 `build/autolink-performance-final` 单独保留，不混入正式统计。

产品收益仅比较无计时插桩的普通静态库总解析。新诊断的 `consolidate_ms` 已包括合并、候选判断、缓冲区准备和邮箱识别；`autolink_text_ms` 是其中的子集，不能重复相加。旧诊断的 `autolink_postprocess_ms` 包括重复合并、文字处理与遍历；新版本没有独立后处理调用，不代表邮箱识别没有成本。

输入生成/UTF-8 准备、指纹和 HTML 校验、节点释放不计入总解析；释放单列。OS 内存在准备后、解析后、校验并释放后采样，峰值读数包含校验阶段。另用同一个 `MuffinAstPerformanceProbe.exe` 分别加载冻结/新 Core DLL，交错测量完整 `DocumentSession` 准备、AST 转换、绑定和索引，以检查收益是否延续到后续阶段。

## 正式结果

以下为三次样本中位数，单位 ms；各阶段中位数不能相加还原总中位数。只比较本轮交错的修改前后记录，不用上一轮的独立运行计算收益。

| 输入 | 修改前普通库总解析 | 修改后普通库总解析 | 降幅 |
| --- | ---: | ---: | ---: |
| 1 MiB 富行内 | 49.27 | 40.20 | 18.4% |
| 10 MiB 富行内 | 526.57 | 417.98 | 20.6% |
| 30 MiB 富行内 | 1690.69 | 1201.09 | 29.0% |
| 1 MiB 富行内＋邮箱 | 50.52 | 42.55 | 15.8% |
| 10 MiB 富行内＋邮箱 | 539.76 | 402.89 | 25.4% |
| 30 MiB 富行内＋邮箱 | 1539.35 | 1201.15 | 22.0% |
| 30 MiB 普通正文 | 250.94 | 252.50 | −0.6% |
| 1 MiB 扩展混合 | 98.66 | 88.02 | 10.8% |
| 1 MiB 富行内＋无效邮箱候选 | 49.89 | 39.29 | 21.2% |
| 1 MiB 富行内＋脚注 | 63.08 | 55.22 | 12.5% |
| 1 MiB 富行内＋未定义引用 | 65.97 | 56.17 | 14.9% |

30 MiB 富行内普通库样本完整保留：修改前 **1721.37 / 1690.69 / 1612.27ms**，修改后 **1287.07 / 1200.57 / 1201.09ms**。普通正文的新版本 **262.15 / 237.04 / 252.50ms**，没有显示稳定收益；本轮不宣称所有文本都改善。

最终 30 MiB 富行内诊断：原核心合并 **239.05ms**、另一次 autolink 全树后处理 **535.83ms**；新合并＋候选处理 **338.26ms**，没有额外 autolink 全树遍历。邮箱候选为零，完整邮箱识别调用 **2,099,220→0**；候选判断复用已有的合并遍历，不表示完全不访问文字。含邮箱的大文档仅对末尾的一个合并文字节点执行邮箱识别，原始/实体邮箱仍全部生成正确链接。扩展混合样例有大量真实邮箱，指纹也一致。

新版本 30 MiB 富行内的行内遍历诊断中位数仍约 **887.56ms**，是 cmark 内部下一项主要成本。

## 完整 Core 准备

另三轮 **30** 份同程序/不同 Core DLL 对照覆盖 1/10/30 MiB ASCII 富行内、1 MiB Unicode、1 MiB 实体/转义文本。所有 AST 指纹、块/行内节点数量、共享原文节点数量和字符数一致。

| 输入 | 完整 DocumentSession 准备：修改前→后 | 其中 cmark | AST 转换 |
| --- | ---: | ---: | ---: |
| 1 MiB ASCII | 139.95→127.14 | 49.33→39.21 | 45.73→44.34 |
| 10 MiB ASCII | 1436.61→1295.52 | 526.39→399.36 | 452.23→444.72 |
| 30 MiB ASCII | 4292.46→3871.89 | 1588.94→1205.91 | 1331.40→1314.85 |
| 1 MiB Unicode | 159.19→149.67 | 46.76→37.29 | 68.78→69.98 |
| 1 MiB 实体/转义 | 151.87→132.79 | 60.43→42.82 | 49.43→45.85 |

30 MiB 完整准备减少约 **420.57ms（9.8%）**。AST 转换没有出现与解析收益相当的上升，缓冲区所有权回归也直接检查未延后分配；收益延续到了完整准备。Unicode 转换中位数有小幅正向波动，没有把它写成统一改善。Core 原始目录 `build/autolink-core-preparation`；这里仍是同步 Core 准备，不是 GUI 打开至首屏。

## 内存、释放与验收

30 MiB 富行内普通 cmark 库解析后工作集 **1051.70→1051.58 MiB**、私有提交 **1129.03→1128.87 MiB**；包含校验阶段的峰值工作集 **1134.31→1133.77 MiB**。释放后工作集 **103.91→103.82 MiB**，输入字符串仍保留。节点释放中位数 **402.87→387.47ms**，没有新增释放成本。没有逐节点候选索引，未观察到显著内存增加；这些 OS 采样不能证明长期无泄漏。

Core 准备与 AST 校验之后的工作集：30 MiB **758.06→759.02 MiB**、10 MiB **264.35→264.33 MiB**。最终 `DocumentSession` 销毁的 30 MiB 中位数 **224.66→231.96ms**，比前版约高 7.3ms，全部记录保留；它不抵消约 421ms 的准备收益，也不能直接归因到某个分配器行为。

`scripts/audit_cmark_autolink_performance.py` 独立重读 **132** 份 cmark 和 **30** 份 Core 原始记录，重算全部时间/内存中位数，核对嵌套阶段关系、计数、所有语义/AST 指纹和源/二进制 SHA-256，并还原生成诊断源，确认只有声明的计时 hook。1736 份 oracle 与三次修改前运行一致。完整 Release 构建、重点 **5/5（4.34 秒）**、全量 **339/339（467.07 秒）**通过，日志 `build/autolink-release-build.log`、`build/autolink-focused-ctest-final.log`、`build/autolink-all-ctest.log`。dist 已刷新并核对三个应用二进制与 Release SHA-256 一致，日志 `build/autolink-dist.log`；最终源/打包摘要见 JSON 的 `verification`。Python 语法、JSON、C++ 格式和 `git diff --check` 通过。

本轮没有前台 GUI 六组矩阵复测，不把 Core 准备收益等同于用户首屏延迟。既有实体编码脚注起始符的不稳定问题继续单独保留，未在本轮扩大修改范围。未 commit、未 push，原生键盘长尾调查仍暂缓。

## 复查

保留本轮冻结程序和 DLL 时：

```powershell
cmake --build --preset conan-release --target MuffinCmarkArchiveProbe MuffinCmarkPerformanceProbe MuffinCmarkAutolinkProbe MuffinAstPerformanceProbe
python -B scripts/run_cmark_autolink_performance.py --baseline build/autolink-baseline --output build/autolink-recheck --repeats 3
python -B scripts/run_ast_performance.py --before build/autolink-baseline --after build/autolink-ast-candidate --output build/autolink-core-recheck --repeats 3
python -B scripts/audit_cmark_autolink_performance.py --summary build/autolink-performance-attachments/summary.json --ast-summary build/autolink-core-preparation/summary.json --output build/autolink-audit.json
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
cmake --build --preset conan-release --target dist
```

输出目录需使用新名称。两个 Core 测量目录必须使用同一个 probe EXE，并各有对应版本的 DLL；脚本检查摘要。全新 checkout 重建旧版时，按 provenance 的源文件快照在独立目录构建，不能用已优化的当前库重写旧 oracle。独立审计默认核对本轮固定路径的语义记录，最终构建/打包另见 JSON 的 `verification`。
