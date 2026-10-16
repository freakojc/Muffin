# 无脚注文档的 cmark 脚注整理快速路径

本轮完成解析期脚注标记和无脚注快速路径。1/10/30 MiB 富行内文档的普通静态库总解析中位数下降 **13.9% / 21.7% / 19.9%**；30 MiB 从 **1926.43ms 降到 1542.20ms**。84 份交错测量的树、HTML、源码坐标指纹一致，636 组固定语义快照与修改前的库一致。原始样本、源文件/二进制摘要及独立审计见 [JSON](cmark-footnote-fast-path-2026-10-17.json)。

## 实现与边界

`cmark_parser` 增加每份文档的 `has_footnotes` 标记，在创建脚注定义或引用时置位；扩展通过 `cmark_parser_add_child` 创建脚注节点也会置位。`process_footnotes` 在标记未置位时直接返回，不再创建映射或执行两次全树遍历。没有增加判断用的全文扫描，也没有逐节点缓存或全局状态。

标记保持保守：已创建的节点即使随后被移除，也仍执行原整理。现有 `cmark_parser_reset` 清零整个解析器，下一份文档重新判断。只有未定义引用、没有定义的文档仍执行整理，将引用转换成普通文字；未使用定义的清理、定义移动、编号、重复引用计数和引用关联继续使用原算法。脚注选项及 table/autolink/tasklist/math 扩展保持开启。修改只涉及 vendored `parser.h`、`blocks.c`、`inlines.c`，私有结构和所有内置消费者一起重建。

## 原版语义对照

在修改 cmark 前，构建并冻结普通库及阶段诊断程序，保存原始三个源文件、生成诊断源、二进制及 SHA-256，位于 `build/footnote-baseline`。固定 oracle 的 provenance 记录原版源文件/程序/输入摘要。基准未拿“强制执行快速路径的新代码”代替旧库。

`MuffinParserDefinitionTest` 增加原版 oracle 校验，保持 CTest 总数为 339。159 份输入包括普通正文、正常和未定义脚注、无引用定义、定义顺序与引用顺序不同、重复定义/引用、中文标签、多行定义、列表/引用/脚注嵌套、自引用、表格、任务列表、数学、代码、HTML、转义、实体、CRLF 和无末尾换行；其中 128 份为固定种子的片段组合。

每份输入分别开启/关闭脚注，整段/逐字节 feed，并连续复用解析器，共 **636** 份结果。比较树的进入/退出顺序、节点类型、文字、链接、标题、四个源码坐标、标题/列表属性、脚注引用序号、定义引用次数、引用关联的定义，以及完整 HTML。修改前连续三次运行一致，新库与它们完全相同。

探索语料还发现一项原版问题：以 `[&#94;a]` 写脚注起始符时，输出在原版重复运行间就不稳定。最初的 oracle 与两次重跑有 **33 / 30** 项差异；原始输入/输出和初次新库结果全部留在 `build/footnote-baseline/oracle-initial*`、`build/footnote-original-repeat-*.json`、`build/footnote-candidate-oracle.json`。正式稳定对照保留实体文字和正常脚注，移除实体作为脚注起始符的组合；没有把这一原版问题算作通过，也没有在本轮扩大修改范围解决它。

## 测量方法

三轮新进程交错测量，每隔一轮反转样例和程序顺序，共 **84** 份记录：

- 1/10/30 MiB GUI 同款富行内文本、30 MiB 普通正文。
- 1 MiB 扩展混合，以及富行内文档末尾加入一个真实脚注或一个未定义引用。
- 每组分别运行冻结的普通库、冻结的阶段诊断、新普通库、新阶段诊断。

总解析比较使用没有计时插桩的普通静态库。阶段诊断单独归因；与上轮相同，只有独立手动目标使用两个粗阶段 hook 和行内容器计时，不链接进应用。计时排除输入生成/UTF-8 准备、语义指纹和 HTML 生成、节点释放，释放耗时另列。内存是在准备后、解析后、校验并释放后的 OS 采样，峰值读数也单独保留。

以下均为三次样本中位数，单位 ms；各阶段中位数不能相加还原总中位数。前后只比较本轮交错数据，不直接与上轮独立运行作差。

| 输入 | 原普通库总解析 | 新普通库总解析 | 降幅 | 原诊断脚注阶段 | 新诊断脚注阶段 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 MiB 富行内 | 61.66 | 53.09 | 13.9% | 13.2073 | 0.0002 |
| 10 MiB 富行内 | 658.57 | 515.56 | 21.7% | 138.7392 | 0.0002 |
| 30 MiB 富行内 | 1926.43 | 1542.20 | 19.9% | 443.0155 | 0.0002 |
| 30 MiB 普通正文 | 350.69 | 254.34 | 27.5% | 83.9554 | 0.0002 |
| 1 MiB 扩展混合 | 101.92 | 100.51 | 1.4% | 25.1849 | 22.2920 |
| 1 MiB 富行内＋脚注 | 66.47 | 63.96 | 3.8% | 14.5724 | 13.4882 |
| 1 MiB 富行内＋未定义引用 | 63.33 | 64.54 | −1.9% | 13.6103 | 13.6815 |

无脚注的诊断阶段最大值 **0.0005ms**，只有入口判断与计时噪声。含脚注的样例保留全部整理；少量总时间正负波动不作为它们的优化收益。原版 30 MiB 普通库三个样本 **1863.80 / 1926.43 / 2041.53ms**，新库 **1495.08 / 1542.20 / 1599.36ms**。扩展混合新库慢样本 **133.42ms** 等记录全部保留，未筛除。

30 MiB 富行内的节点释放中位数 **384.29→383.97ms**，没有把解析成本转移到释放。新阶段诊断的行内遍历仍约 **846.58ms**、autolink 后处理约 **449.67ms**，下一步应先研究 autolink 候选范围及重复遍历。

## 内存与验证

本轮不改变 AST 表示或增加逐节点数据。30 MiB 富行内普通库解析后工作集 **1051.65→1051.70 MiB**，私有提交 **1128.97→1128.99 MiB**；包括校验阶段的 OS 峰值工作集 **1134.30→1134.33 MiB**。释放后工作集 **103.79→103.88 MiB**，输入字符串仍保留；私有提交 **161.26→161.95 MiB**。这些采样没有显示显著新增内存，也不能证明长期无泄漏或代表 Muffin 整体峰值。

独立审计重读 84 份原始文件，核对时间关系、指纹、全部统计和源/二进制摘要，并验证诊断生成源仅有两个声明的 hook。它还校验 636 份快照与三次原版运行。完整 Release 构建、重点 **5/5（3.62 秒）**、完整 CTest **339/339（494.65 秒）**通过，日志 `build/footnote-release-build-final.log`、`build/footnote-focused-ctest-final.log`、`build/footnote-all-ctest.log`。dist 已刷新并核对三个应用二进制与 Release 摘要一致，日志 `build/footnote-dist.log`；最终源/打包摘要见 JSON 的 `verification`。Python 语法、JSON、C++ 格式及 `git diff --check` 通过。

本轮使用阶段基准，没有重新测量六组前台 GUI 打开至首屏；不能把解析阶段减少的时间直接当作首屏收益。原生键盘长尾调查继续暂缓，未 commit、未 push。

## 复查

保留本轮冻结的基线目录时：

```powershell
cmake --build --preset conan-release --target MuffinCmarkArchiveProbe MuffinCmarkPerformanceProbe MuffinCmarkFootnoteProbe
python -B scripts/run_cmark_footnote_performance.py --baseline build/footnote-baseline --output build/footnote-recheck --repeats 3
python -B scripts/audit_cmark_footnote_performance.py --summary build/footnote-performance/summary.json --output build/footnote-audit.json
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
cmake --build --preset conan-release --target dist
```

手动程序使用 Conan Qt 的运行环境；Python 基准脚本会读取生成 preset 的 Qt 路径。输出目录需使用新名称。独立审计当前固定读取本轮 636 份语义原始记录；重新测量的性能 summary 可另传，历史结果保留。全新 checkout 要重建旧库时，按 oracle provenance 的原版源文件在独立目录构建，不能用已经应用快速路径的当前库生成新的“原版”oracle。
