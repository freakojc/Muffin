# Muffin 内存生命周期检查（2026-10-23）

检查基线：`5fc609fa`。下文的“已复现的问题”和原始 JSON 记录修复前的行为；后续修复与复验记录见文末，保留基线便于对照。

已通过代码审查和运行复现确认四处内存保留：图片缓存没有容量上限、编辑缓冲没有压缩、异步打开保留过期文本、全局资源表保留已销毁 Mermaid 缓存的条目。这些对象仍可被程序访问，属于生命周期过长或无界增长；本次没有用堆泄漏检测器证明存在“分配后丢失所有指针”的泄漏，也不能排除其他路径中的此类泄漏。

## 已复现的问题

| 优先级 | 位置 | 实测证据 | 对用户的影响 |
| --- | --- | --- | --- |
| P1 | [ImageLoader::store](../../src/render/ImageLoader.cpp#L64) | 依次打开并关闭 24 篇笔记，每篇引用一张不同的 1024×1024 图片；24 个标签页全部销毁后，缓存仍持有 **96 MiB** 像素数据 | 浏览含大量不同图片的笔记时，内存随访问过的图片数量增长，关闭标签页不会回收这部分数据 |
| P2 | [PieceTable::replace](../../src/document/PieceTable.cpp#L95) | 在同一 PieceTable 中反复插入 512 Ki 个字符后删除，共 32 次；可见文本只剩 1 个字符，底层字符串容量仍为 **67,108,848 字节，约 64 MiB** | 持续增量编辑会保留已删除的内容；清理撤销记录本身不能回收该缓冲 |
| P2 | [DocumentSession::finishAsyncParse](../../src/document/DocumentSession.cpp#L954) | 异步打开约 16 MiB 的 UTF-16 文本，解析完成后替换为空文档；旧字符串仍有共享所有者，直到会话析构才释放 | 一个仍存活的会话可能在替换内容后继续保留旧文件文本 |
| P2 | [LayoutResources::publish](../../src/render/LayoutResources.cpp#L47) | 创建、渲染并销毁 100 个 MermaidRenderCache；100 个场景对象均销毁，但 **100 条全局资源记录仍在** | 每个缓存实例使用独立 UUID，反复打开图表或编辑不同内容会持续增加资源元数据 |

### 图片缓存

`ImageLoader` 是进程级对象，`cache_` 使用 `QHash<QString, QImage>`，没有按字节计量的淘汰策略，也没有由文档释放资源的接口。本地路径的 `localFiles_`、`fileIdentities_` 和文件监视记录也缺少随缓存退出的清理流程。

像素数据的测量结果如下，数值不包含哈希表、路径字符串、文件监视和解码临时空间：

| 已打开并关闭的不同图片笔记数 | 图片缓存像素数据 |
| --- | --- |
| 6 | 24 MiB |
| 12 | 48 MiB |
| 18 | 72 MiB |
| 24 | 96 MiB |

该单例被设计为不执行静态析构，源文件说明这是为绕开 Qt 退出顺序及 Windows ARM64 loader lock 等问题。因此不能简单把单例改成静态对象来解决；需要在应用仍有效时管理重资源的生命周期，保留必要的退出兼容性。

建议先实现图片缓存的字节预算和 LRU，再处理活动文档的资源引用、异步下载完成、文件监视退出等边界。淘汰后必须仍能正确重新加载和刷新图片。

### 编辑缓冲

`PieceTable::replace()` 把新内容追加到 `changes_`，删除只移除 piece 对旧片段的引用，不回收 `changes_` 中的字符。这是 PieceTable 的常见实现取舍，但当前没有后续压缩流程。`MarkdownDocument::replaceTopLevelRange()` 的增量路径直接使用这个缓冲。

本次复现直接调用 PieceTable，没有创建 UndoStack，所以这 64 MiB 不能归因于撤销历史。累计插入、删除的内容为 16,777,216 个 UTF-16 code unit，即 32 MiB 文本载荷；字符串扩容后的容量约 64 MiB。

这不表示所有编辑操作都会永久累积同一缓冲：全文重建 PieceTable、替换文档或销毁会话可以释放它。需要解决的是持续走增量编辑路径时的容量增长。建议按无效内容占比、总字节数和空闲时机触发压缩，并验证撤销、共享文本切片和源码位置索引仍然正确；仅限制撤销条数不够。

### 异步解析文本

异步开始时，`parseAndStore()` 把输入放入 `pendingText_`；完成发布后没有清空它，`newDocument()` 的同步替换也没有重置这份状态。

检查使用 `QString::isDetached()` 判断是否仍存在共享所有者：探针开始时独占文本，空文档替换后仍共享，销毁会话后恢复独占。这能区别真实所有权保留与分配器未把空闲页交还操作系统。未修改文档时，这份字符串通常与文档共享缓冲，并不额外复制一整份文本；问题在于替换后仍托住旧缓冲。

另外，过期异步结果的分支直接返回，没有取走 watcher 的结果；已完成 future 可能继续持有其解析结果。这一分支本轮只做了静态分析，尚未单独量化，不计入上述 16 MiB 的测量。

修复时应同时处理成功、被更新操作覆盖、取消和重新解析，并注意信号处理中的重入，避免清掉下一次任务刚设置的状态。

### Mermaid 全局资源记录

MermaidRenderCache 自身有容量限制，销毁后的场景 `weak_ptr` 全部失效，因此本轮没有发现场景对象随已关闭缓存持续存活。

问题在更长寿命的 `LayoutResources::entries_`：资源 key 包含缓存 UUID 和内容 hash，发布后没有注销接口。缓存清空、淘汰或销毁都不会清理全局记录。单条占用较小，但条目数量没有上限。图片资源同样使用这张全局表。

建议给资源生产者增加生命周期管理，注销无使用者的记录；同时保证版本变化仍能使旧布局失效，避免简单删除后版本重新从零开始，导致旧依赖误判为有效。

## 本轮未发现明显持续增长的路径

运行探针使用真实 MainWindow、DocumentTab、DocumentSession 和相关控件，等待线程池任务与 DeferredDelete 后采样：

- 预热打开/关闭普通、富文本笔记各 10 次，再打开/关闭普通笔记 60 次、富文本笔记 30 次。对文档、会话、控制器、撤销栈、实时编辑视图、源码编辑器共做 **660 次 QPointer 销毁检查**，全部通过。
- 搜索与快速打开各 30 轮，覆盖被新搜索替代、窗口关闭时扫描仍在进行等情况。搜索结束后的 watcher 已删除。
- 创建并销毁偏好设置窗口 10 次，每次切换全部导出格式。
- 实际执行 HTML、PDF、PNG 导出各 20 次，均成功。
- 上述普通文件、富文本、搜索、设置、导出阶段，每个采样点的窗口子 QObject 数量为 562，应用 QWidget 数量为 150；字体注册数量保持 8。不把这些短循环等同于对所有输入和长期运行的证明。

图片阶段窗口子 QObject 数量从 562 增至 570，QWidget 数量仍为 150，标签页销毁检查通过。这一小幅变化尚未单独归因；图片像素缓存增长则已直接计量，不依赖 QObject 总数推断。

静态检查还覆盖草稿写入线程的停止和 join、搜索 watcher、导出临时对象、Lexbor 文档、Yoga 树、HarfBuzz 对象及 cmark 分配池等释放路径。既有 ParserAllocationTest 的分配/释放检查通过。

## 仍需关注，但本轮没有动态确认的增长点

- [DecorationPainter::svgIcon](../../src/render/DecorationPainter.cpp#L121) 的进程级 SVG renderer 缓存没有容量限制。不同 SVG 内容可能持续增加条目。
- [registerPrivateFont](../../src/theme/ThemeFontDecoder.cpp#L322) 按内容去重已注册的私有字体，但没有注销旧字体。重复使用同一字体不会一直增长；不断导入新字体或字体修订版的生命周期仍值得单独测试。
- 本轮没有覆盖长期慢速网络图片下载、多窗口整晚运行或所有第三方库的堆分配。

## 检查方式与复现

环境：Windows x64、MSVC Release、Conan Qt 6.11.1。完整探针分别在两个新进程中执行，原始数据见 [Native 文字后端 / offscreen QPA](memory-lifecycle-audit-2026-10-23-native.json) 和 [Fractional 文字后端 / Windows QPA](memory-lifecycle-audit-2026-10-23-windows-fractional.json)。两个组合均复现上述四处保留，并通过对象销毁检查。另一个较早进程在加入编辑缓冲和导出探针前，已复现图片、异步文本和资源记录三处问题。这里的 Native 指文字排版后端，不代表 offscreen 测试使用了 Windows 窗口插件。

诊断实现在 [MemoryLifecycleAudit.h](../../scripts/MemoryLifecycleAudit.h)，由 `MuffinDocumentTabsTest --memory-audit` 显式调用，不进入默认 CTest 流程。它记录可达对象、共享字符串所有权、场景弱引用、实际缓存像素及进程统计。进程 working set/private commit 仅用作背景数据；不能单凭任务管理器数值未回落判定泄漏。

从仓库根目录运行，以下环境变量只影响当前 PowerShell 会话：

```powershell
cmake --build --preset conan-release --target MuffinDocumentTabsTest
$auditPreset = (Get-Content build/generators/CMakePresets.json -Raw | ConvertFrom-Json).configurePresets[0].environment
$env:QT_PLUGIN_PATH = $auditPreset.QT_PLUGIN_PATH
$env:PATH = (Join-Path $auditPreset.QT_HOST_PATH bin) + ';' + $env:PATH
$env:QT_QPA_PLATFORM = 'offscreen'
$env:MUFFIN_TEXT_LAYOUT_BACKEND = 'native'
$env:MUFFIN_MEMORY_AUDIT_OUTPUT = "$PWD/build/memory-audit.json"
.\build\Release\MuffinDocumentTabsTest.exe --memory-audit
```

也可以把 `QT_QPA_PLATFORM` 改成 `windows`、文字后端改成 `fractional`，在新进程中复测。默认测试数据、设置和草稿使用临时目录。

基线检查阶段的验证记录：Release 全量构建通过；DocumentTabs、ParserAllocation、PieceTable、MermaidRenderCache、ResourceIncrementalLayout、DocumentSessionAsyncParse 六项相关 CTest 全部通过。该阶段没有重复运行整个 345 项测试套件；修复后的全量验证另记于下。

现有 [Linux ASan CI](../../.github/workflows/ci.yml#L202) 显式设置 `detect_leaks=0`。它可以检查越界和释放后使用，但通过并不说明没有泄漏。本机没有可用的 Linux LeakSanitizer 或 Windows 堆泄漏分析器，因此本次结论是“已确认上述保留问题”，不是“已排除其他堆泄漏”。后续适合增加独立的泄漏检测任务，针对已知 Qt/进程级对象做精确抑制，而不是全局关闭检测。

## 修复与复验（同日）

按图片、异步文本、编辑缓冲、资源表的顺序完成修复。原始基线 JSON 保留，修复后的数据另存。

| 检查 | 修复前 | 修复后（两种后端组合结果相同） |
| --- | --- | --- |
| 关闭 24 篇图片笔记后的像素缓存 | 96 MiB、24 张图片 | 0 字节、0 张图片 |
| 空文档会话持有旧异步文本 | 16,777,242 字节 | 0 字节 |
| 插入/删除 32 轮后的一字符缓冲 | 67,108,848 字节 | 4 字节（含结尾字符） |
| 100 个 Mermaid 缓存销毁后的遗留版本记录 | 100 条 | 0 条 |

实现和边界：

- 图片采用共享使用凭据，多个窗口或布局共享同一资源。文档布局保留已访问图片的凭据，允许延迟排版回收块详情，而不丢失仍在打开的文档资源。最后一个使用者退出后，在当前事件轮结束时移除像素、路径身份、文件/目录监视，并取消尚未完成的下载；连续测量与最终排版之间不会发生立即清除。没有使用者的预加载缓存默认限制为 32 MiB、256 项，并按 LRU 淘汰。正在使用的文档资源仍按实际需要占用内存，不受预加载缓存预算强制驱逐。退出时清理重资源，保留原有兼容 Qt 退出顺序的单例外壳。
- 每个异步解析任务拥有独立 watcher 和版本标识，完成时取走结果并销毁 watcher。成功发布后清空待发布文本，过期结果直接释放；同步替换也重置待处理状态。信号回调中发起的新任务不会被旧任务的状态清理覆盖。
- PieceTable 废弃字符达到 512 Ki code unit（约 1 MiB）、且不少于剩余可见字符数时重建缓冲及其索引；全删时立即清空。压缩成本由足够多的删除操作摊销，普通输入仍然只追加。外部快照和 AST 使用的共享不可变字符串继续有效，逻辑源码偏移保持不变；撤销仍持有自己所需的增删文本。
- 全局资源表区分生产者与布局依赖。Mermaid 淘汰、清空、析构时释放生产者所有权，仍在使用场景的布局保留小型版本状态；最后一个使用者退出后，状态和弱索引项一并回收。纯缓存淘汰不触发布局失效，显式重置会失效；版本使用全局递增编号，避免同名资源重新创建后误命中旧布局。

新增回归覆盖图片 LRU、共享读者、文件监视退出、慢下载取消、重新加载、资源版本注销/重建、异步任务被覆盖和信号重入、缓冲压缩后的快照与索引，以及真实输入路径中的大段粘贴、删除、撤销、重做。原有图片尺寸/像素刷新、隐藏视图、延迟排版和 Mermaid 场景复用测试继续保留。

修复后完整探针分别使用 Native / offscreen 和 Fractional / Windows QPA，在独立进程中各执行一轮，四项结果完全一致。每组仍通过 660 次标签页相关对象销毁检查、100 个 Mermaid 场景销毁检查，以及搜索、设置和导出循环；图片阶段每 6 次采样的缓存、使用凭据、文件监视、目录监视和待完成下载数均回到 0。

原始结果：[修复后 Native](memory-lifecycle-audit-2026-10-23-fixed-native.json)、[修复后 Windows / Fractional](memory-lifecycle-audit-2026-10-23-fixed-windows-fractional.json)。这证明本次四个复现用例已消除相应保留，不代表对所有第三方分配的完整堆泄漏认证。

修复后的验证：`cmake --build --preset conan-release` 通过；`ctest --preset conan-release --output-on-failure` **345/345 通过**，总耗时 531.84 秒；clang-format 检查通过。已运行 `dist` 目标刷新分发包，确认 Muffin.exe、MuffinCore.dll、MuffinUi.dll 的 SHA-256 与 Release 构建一致。分发包启动检查时传入 `--version` 后仍出现窗口，随后通过关闭窗口正常退出（退出码 0），因此该检查只验证启动与关闭，不作为命令行版本输出验证。
