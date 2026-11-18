# Notepad3 CE LSP 客户端加固与优化设计

日期：2026-11-18　状态：已实现（LSP 三项测试与 Release x64/Win32 完整构建通过；GUI 手动冒烟、版本测试基线失败与 docs 基线错误见下）

## 1. 目标与范围

把 2026-11-18 的源码分析结论落成可实施变更，覆盖四类工作：

1. 稳定性：UI 线程不再同步等待 teardown；请求有超时；管道事件句柄无关闭竞态；stderr 不污染 LSP 帧流。
2. 吞吐：文档增量同步、服务端按变更范围刷新诊断、semanticTokens 按版本缓存、补全防抖、容量超限显式降级。
3. 配置与体验：补齐默认值/INI 示例；让 inlayHint/codeLens 开关真实可控；诊断可列表查看和跳转；服务器路径/参数可配置且支持长路径。
4. 验证：新增 LSP 端到端 harness，并跑现有针对性测试与构建。

非目标：不改 vendored Scintilla/Lexilla；不把 LSP 改成多文档工作区服务器；不新增完全自绘的浮层 UI；不改变用户已确认的 AA/LSP/CE_DOC 行为策略；本轮不新增 LSP 设置对话框 UI，开关继续通过 INI 控制（与 2026-11-18 分析中“GUI 后续再做”的建议一致）。

## 2. 不可变更的既有约束

以下来自用户已确认的策略，实施时不得静默改变：

- LSP teardown 超时后不强制杀工作线程；由线程退出时自行释放其管道句柄。进程仍可在宽限期后 `TerminateProcess`。
- 只接受当前会话代次、当前 URI、且不旧于当前版本的诊断；其他诊断丢弃并清屏。
- 启动失败使用时间戳冷却重试；文档打开/设置变更/显式重试立即清零；连续崩溃退避。
- `.lua` 仍走 CE lexer，未知扩展名也保持 CE；首行语言按扩展名判断。
- CE 编辑器不被屏蔽；不得取 CE 的编辑锁 `mr.beginEdit()`。
- 遵守 `docs/architecture/codebase-and-conventions.md`：Scintilla 调用走 `SciCall.h`；批量编辑用 undo 宏；`HSTRINGW`/`HPATHL` 优先；不新增项目级全局结构体。

## 3. 现状根因（源码锚点）

### 3.1 同步 teardown

`src/CeLspClient.c:810-912` 的 `_Teardown()` 在 UI 线程依次等待进程 300ms、两个线程各 500ms；服务退出后 `1355-1359` 会自动重启，所以卡顿不只在程序退出时发生。

### 3.2 请求无生命周期

`src/CeLspClient.c:28,534-562,1269-1279`：`CELSP_MAX_PENDING=32`，只在满槽时丢同类最早项；没有请求超时。补全取消只覆盖补全一类（`src/CeLspFeatures.c:435-441,520-528`）。

### 3.3 句柄竞态与 stderr 污染

`src/CeLspClient.c:284-286` 在锁外读 `s_hEvent`，`878-888` 在 teardown 中关闭；`713-719` 把 stderr 与 stdout 合并到同一管道，而非协议输出会破坏 `390-450` 的分帧解析。

### 3.4 全量同步与重复复制

`src/CeLspClient.c:153-179,465-506` 每次编辑完整复制文档并发送全量文本；服务端 `ce_lsp_server/CeLspServer.c:3421-3434` 再复制并全量执行 `534-719` 的诊断；`src/CeLspFeatures.c:1927-1940` 每次同步请求 `semanticTokens/full`；`269-275` 用 1MB 上限兜底。

### 3.5 配置缺口

`src/TypeDefs.h:926-934` 定義 8 个 LSP 设置；`src/Config/Config.cpp:1244-1247,2205-2208` 读写，但 `src/CeSettings.c:351-360` 的 `CeSettings_ResetDefaults()` 漏掉后 4 个 bool。`Build/Notepad3.ini:80-84` 只列出基础键；没有 LSP 设置 UI。`src/CeLspFeatures.c:1935-1937` 明确不请求 inlayHint/codeLens，但配置仍然保存，开关无效果。

## 4. 设计方案

### 4.1 异步 teardown 与事件句柄所有权

- 把 `s_hEvent`/`s_hStopEvent` 改为**会话生命周期内复用、只在 final stop 时关闭**。重启时只 `ResetEvent()`，不再关闭/重建，从而消除 `_Enqueue()` 锁外读取已关闭句柄的竞态。
- 新增 `_TeardownSession()` worker：`_Teardown(false)` 在 UI 线程只做状态切换、generation 递增、入队 `shutdown/exit`、置事件，然后把原来的等待/关闭/清队列逻辑搬到工作线程；完成后 `PostMessageW(WM_CELSP_STATUS, CELSP_STATUS_TEARDOWN_DONE, generation)`。
- `WM_CELSP_STATUS` 的 `TEARDOWN_DONE` 分支只在 generation 仍匹配时清理 UI 侧 LSP 装饰；不再触发重启。真正的自动重启仍由读线程 `CELSP_STATUS_EXITED` 驱动。
- `CeLspClient_Stop()`（final）保留同步路径，但这是进程退出路径，可接受一次等待；仍不强制杀线程。
- 新增 `CELSP_STATUS_TEARDOWN_DONE` 状态枚举与 `IDT_TIMER_CELSP_TEARDOWN` 定时器常量；定时器仅作为 worker 未回投时的兜底，避免无限期悬挂。

### 4.2 请求超时与 latest-wins

- 扩展 `CELSP_PENDING`：增加 `ULONGLONG deadline`、`int generation`。
- 新增 `CELSP_REQUEST_TIMEOUT_MS`：普通请求 8 秒，completion/hover/signature 3 秒。
- 新增 `CeLspClient_OnRequestTimeoutTimer()`，由 `IDT_TIMER_CELSP_TIMEOUT` 每秒触发；遍历 pending，过期则发 `$/cancelRequest` 并移除。
- `_AddPending()` 改为：同 kind 请求只保留最新一条；不同 kind 共满 32 条时丢弃最早 deadline。
- `s_nextId` 溢出时从 1 重新开始并清空 pending，避免 id 回绕命中旧响应。

### 4.3 stderr 隔离

- 启动时创建第三条管道；`si.hStdError` 指向独立写端。
- 新增 `_StderrThread`：用 `WaitForSingleObject(s_hStopEvent, 0)` 退出，读到的文本经 `WM_CELSP_STATUS`/`CELSP_STATUS_ERROR` 回投主线程，复用现有日志路径。
- stdout 写端仍错误地指向 stderr 的情况不再存在；服务端若输出非协议文本只进入日志。

### 4.4 增量文档同步

- 在 `Notepad3.c` 的现有 SCN_MODIFIED 路径中调用新增 `CeLspClient_OnDocumentRangeChanged(DocPos pos, DocPos len, int linesAdded, const char* text)`，把 UTF-8 字节范围累积进客户端静态数组（上限 256 段，超出降级全量）。
- `_FlushChanges()` 根据累积范围生成 `contentChanges`：有范围且会话为 UTF-8 时发送 `{range, text}`；utf-16 会话、文档重开、范围超限或无法定位时保持现有全量 `text` 行为。
- 服务端 `_HandleDidChange()` 支持两种格式：无 `range` 时 `_SetText()`；有 `range` 时按 LSP Position 转 UTF-8 偏移，替换 `g_docText` 对应区间并更新 `g_docLen`。`rangeLength` 不信任，只按 range 计算。
- `didOpen` 仍发送完整文档；`didSave` 在版本未变化时跳过重复诊断发布。

### 4.5 服务端诊断与 semanticTokens 缓存

- `_PublishDiagnostics()` 拆出 `_PublishDiagnosticsForRanges(startLine, endLine)`：didChange 携带变更范围时只重算受影响行；didOpen/didSave/全量变更仍全量扫描。
- 诊断按行缓存到服务端静态数组；范围刷新时只替换对应行的诊断，其他行保留。跨行状态（section/alloc/label）变化时强制全量重算。
- semanticTokens：服务端保存 `{version, result}` 缓存；同一版本重复请求直接回缓存。客户端 `RefreshDecorations()` 在版本未变化时不再发请求。

### 4.6 补全与容量保护

- `CeLspClient_RequestCompletion()` 增加 80ms 防抖：保存 `root/wordStart/triggerKind/triggerChar` 到文件静态结构，由 `IDT_TIMER_CELSP_COMPLETION` 到期后真正发请求；新的击键覆盖旧请求。
- `AutoC` 取消/选择变化时取消防抖定时器。
- semanticTokens legend 超过 32 类时不初始化该能力、记录日志并回退本地着色；`CELSP_ITEM`/符号/链接等固定上限超限时截断并记录一次日志，避免静默错误。

### 4.7 配置、路径与参数

- `CeSettings_ResetDefaults()` 补齐 4 个 LSP bool 默认值：`semanticTokens=true`、`inlayHints=false`、`codeLens=false`、`docDiagnostics=true`。
- `Build/Notepad3.ini` 增加全部 8 个键的说明。
- `Settings2.CeLspServerPath` 改为 `HPATHL`；`CreateProcessW` 命令行用动态 `WCHAR*` 按路径长度+参数长度分配；新增 `Settings2.CeLspServerArgs[256]` 并纳入 INI 读写与默认值。
- `CeLspClient_Start()` 使用 `Path_Get()` 读取路径，支持长路径；无参数时行为与当前完全一致。

### 4.8 诊断列表与开关语义

- 在 `CeLspFeatures.c` 缓存当前文档/当前版本的诊断项（severity、range、message、source），上限 512 条；`OnDiagnostics()` 同时刷新 marker/indicator 与缓存。
- 新增命令 `IDM_CE_LSP_DIAGNOSTICS` 与菜单项（中英文资源），打开一个复用现有 `_SymWndProc` 风格的 modeless 列表窗口；双击条目跳转并选中范围。无诊断时显示占位文本。
- 保留“只显示 severity 1/2 的 squiggle”现状；列表窗口显示全部 severity，从源头绕开 Tab 风格提示刷屏的问题。
- `RefreshDecorations()` 在 `CeLspInlayHints`/`CeLspCodeLens` 为真时发起对应请求；默认仍为 false，避免改变现有脚本显示。渲染继续走现有 `_RenderAnnotations()`，不引入新浮层。

### 4.9 测试 harness

本轮不引入 Win32/HWND/Scintilla stub，也不把 `CeLspClient.c` 的可测试性重构混入产品改动。测试分两层：

- 纯策略层：把 generation 过滤、诊断 URI/版本过滤、pending deadline/latest-wins、增量范围合并抽成可独立编译的 helper（沿用 `TestCeLsp.ps1` 对纯函数做文本抽取的模式），在 `test/TestCeLsp.cpp/.ps1` 中断言。
- 协议/服务端层：新增 `test/TestCeLspEndToEnd.ps1` 只驱动真实 `CeLspServer.exe`（若存在）或脚本化假服务器的 stdio 帧，覆盖增量 didChange 后服务端文本与全量发送结果一致、同版本重复 semanticTokens 返回缓存、诊断按范围刷新后未变更行保留。客户端 UI 副作用（陈旧 completion 不显示、诊断列表跳转）由手动冒烟覆盖，不由该 harness 假称自动验证。

## 5. 实施顺序与风险控制

1. 先改句柄所有权、异步 teardown、超时；这三处互相耦合，需同一批完成。
2. 再做 stderr 隔离与增量同步；增量同步必须客户端/服务端同批上线，禁止只改一侧。
3. 然后做诊断范围刷新、缓存、防抖和容量保护。
4. 最后做配置/UI/测试；UI 资源改动需同步中英文菜单与命令 ID。

风险与回退：

- 异步 teardown 若 worker 不回投，用 `IDT_TIMER_CELSP_TEARDOWN` 在 UI 线程兜底清理，但不得强制杀线程。
- 增量同步实现错误会导致服务端文本漂移；harness 必须对比增量与全量最终文本；出现不一致时客户端自动降级全量同步。
- `HPATHL` 改造影响配置结构布局；只改 `Settings2` 中该字段，保留 INI 键名兼容。
- 诊断列表窗口沿用符号窗口模式，避免自绘浮层与主题/本地化冲突。

## 6. 验证结果

通过：

- `pwsh -NoProfile -File test\TestCeLsp.ps1`：all checks passed。
- `pwsh -NoProfile -File test\TestCeLspList.ps1`：all checks passed。
- `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1`：all checks passed（包含真实 `CeLspServer.exe` 的增量/全量文本一致性、诊断行保留与同版本 semanticTokens 结果比较）。
- `Notepad3.sln` Release x64 完整构建：exit 0，生成 `Notepad3.exe` 与 `CeLspServer.exe`。
- `Notepad3.sln` Release Win32 完整构建：exit 0，生成 `Notepad3.exe` 与 `CeLspServer.exe`。

未通过或基线问题：

- `Test-Docs.ps1`：仍有两条与本设计无关的基线错误（`domain/configuration-and-ini.md`、`domain/pathlib.md` 要求 docs 父目录为 Git 根）；当前工作区不是 Git 仓库。新增设计与计划链接错误已修复，新增文档仅有体积警告。
- `TestFileVersion.cmd`：基线失败，期望 `7.26.1118.0`，现有 32/64 位 exe 为 `5.5.0.9`；与本改动无关。
- GUI 手动冒烟未执行：连续输入补全、标签切换诊断隔离、强杀服务器后的无卡顿重启、inlayHints 开关、诊断列表跳转都只做了静态/编译与协议层验证。
