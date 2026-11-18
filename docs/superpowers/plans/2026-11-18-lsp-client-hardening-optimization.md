# LSP Client Hardening and Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 CE LSP 客户端在拆除、请求生命周期、同步吞吐、诊断展示和配置方面达到设计文档 `2026-11-18-lsp-client-hardening-optimization-design.md` 描述的行为，且不破坏用户已确认的 AA/LSP 策略。

**Architecture:** 客户端继续持有进程/管道/请求队列，所有 Scintilla 操作留在 UI 线程；worker 只负责阻塞 IO 和拆除等待，结果经 `WM_CELSP_MESSAGE/STATUS` 回投。服务端继续是单文档 stdio JSON-RPC 进程，新增增量 `didChange` 应用、按范围诊断与 semanticTokens 缓存。测试分纯 helper 抽取与真实服务端标准 IO 两层，不引入 Win32/HWND/Scintilla stub。

**Tech Stack:** Win32 C/C++，MSBuild/Visual C++，Scintilla `SciCall.h`，自研 `CeLspJson/Framing/Pos`，PowerShell + C++ 回归 harness。

**Spec:** `docs/superpowers/specs/2026-11-18-lsp-client-hardening-optimization-design.md`

## Global Constraints

- 不强制杀工作线程；线程超时后由线程退出时自行释放管道句柄。进程宽限期后可 `TerminateProcess`。
- 诊断只接受当前会话 generation、当前 URI、版本 `>=` 当前版本；其他丢弃并清屏。
- 启动失败退避：`CELSP_RETRY_COOLDOWN_MS=2000u`、`CELSP_RETRY_MAX_BACKOFF_MS=30000u`、`CELSP_MAX_RESTARTS=3`。
- `CELSP_STOP_TIMEOUT=300`、`CELSP_THREAD_TIMEOUT=500`、`CELSP_MAX_PENDING=32`。
- 新增普通请求超时 `CELSP_REQUEST_TIMEOUT_MS=8000`；completion/hover/signature 超时 `CELSP_FAST_REQUEST_TIMEOUT_MS=3000`；completion 防抖 `CELSP_COMPLETION_DEBOUNCE_MS=80`。
- 增量范围累积上限 `CELSP_MAX_CHANGE_RANGES=256`，超出必须降级全量同步。
- 不新增项目级全局结构体；只用 `Globals/Settings/Settings2/Flags/Paths` 和文件内静态状态。
- 所有 Scintilla 调用走 `SciCall.h`；批量编辑包在 undo 宏中。
- 不提交 git，除非用户后续明确要求。

## Review Focus

- 编辑后 80ms 内连续击键：必须只发最后一次 completion，且旧结果不能覆盖新列表。测试：Task 6 的 `TestCompletionDebounce*` 与 Task 2 的 `TestPendingTimeout*`。
- 服务端挂起不响应：请求必须在 3/8 秒后从 pending 移除并发出 `$/cancelRequest`，UI 不死等。测试：Task 2 的 `TestPendingTimeout*`。
- 服务崩溃/重启期间旧 generation 消息回投：必须丢弃，不能清掉新会话装饰。测试：Task 1 的 `TestMessageGenerationGuard` 与 Task 8 的手动冒烟。
- 增量 `didChange` 与全量 `didChange` 的最终服务端文本必须一致；删除/多行插入/UTF-8 中文均不能漂移。测试：Task 4 的 `TestCeLspEndToEnd.ps1` 与 Task 5 的范围诊断断言。
- `CeLspServer.exe` 写 stderr 日志：不能破坏 stdout 的 `Content-Length` 帧解析。测试：Task 3 的 `TestStderrDoesNotEnterStdout` 手动/脚本化服务器帧检查。

---

### Task 1: 纯策略 helper 与测试基线

**Files:**
- Modify: `src/CeLspClient.c:38-60`（保留现有纯 helper，新增纯 helper）
- Modify: `test/TestCeLsp.ps1:24-30`（抽取新增 helper）
- Modify: `test/TestCeLsp.cpp`（新增断言）

**Interfaces:**
- Consumes: 无。
- Produces:
  - `static bool _Celsp_MessageIsCurrent(UINT msgGeneration, UINT currentGeneration)`；
  - `static bool _Celsp_RequestExpired(ULONGLONG now, ULONGLONG deadline)`；
  - `static int _Celsp_PickPendingDrop(int count, const int* kinds, const ULONGLONG* deadlines, int newKind)`；
  - `static bool _Celsp_MergeChangeRange(int* start, int* end, int newStart, int newEnd, int docLen)`；
  - `static bool _Celsp_DiagSeverityForSquiggle(int severity)`；
  - `static bool _Celsp_DiagIsCurrent(...)` 保持现有签名不变。

- [ ] **Step 1: 在 `test/TestCeLsp.cpp` 增加失败断言**

`TestSessionPolicy()` 中增加：
- `_Celsp_MessageIsCurrent(7,7)==true`、`(7,8)==false`；
- `_Celsp_RequestExpired(1000,999)==true`、`(1000,1000)==false`、`(1000,1001)==false`；
- `_Celsp_PickPendingDrop(3, kinds={1,2,2}, deadlines={500,100,200}, newKind=2)==1`；
- `_Celsp_MergeChangeRange(&s,&e,2,5,0)` 为 true 且后续 `(6,10)` 合并为 `(2,10)`；`(20,22)` 返回 false 并保持 `(2,10)`，表示客户端应另起范围段。

- [ ] **Step 2: 运行测试确认失败**

Run: `pwsh -NoProfile -File test\TestCeLsp.ps1`
Expected: FAIL，错误为新 helper 未定义或断言失败。

- [ ] **Step 3: 在 `CeLspClient.c` 实现四个纯 helper**

保持无 Win32/Scintilla 依赖；`_Celsp_PickPendingDrop` 优先返回同类最早 deadline，无同类时返回 deadline 最小下标；`_Celsp_MergeChangeRange` 只在相邻或重叠时合并，半开区间 `[start,end)`，不跨不相邻区间。

- [ ] **Step 4: 让 harness 抽取新 helper**

在 `test/TestCeLsp.ps1` 的 `$sig` 列表追加五个新签名（含 `_Celsp_DiagSeverityForSquiggle`），保持现有文本抽取方式，不 include `CeLspClient.c` 全文。

- [ ] **Step 5: 运行测试确认通过**

Run: `pwsh -NoProfile -File test\TestCeLsp.ps1`
Expected: PASS，输出无 `FAIL`。

### Task 2: 请求超时、latest-wins 与 id 回绕

**Files:**
- Modify: `src/CeLspClient.h:64-71`（`CELSP_PENDING` 增加 `deadline`、`generation`）
- Modify: `src/CeLspClient.c:28-36`（超时常量）、`534-574`（`_AddPending/_SendRequest`）、`1175-1198`（`CancelPending`）、`1226-1281`（`OnMessage`）
- Modify: `src/resource.h:88-89`（新增 `IDT_TIMER_CELSP_TIMEOUT=245`）
- Modify: `src/Notepad3.c:2813-2826`（`WM_TIMER` 分支）
- Modify: `src/CeLspClient.h:85-92`（新增 `CeLspClient_OnRequestTimeoutTimer()`）

**Interfaces:**
- Consumes: Task 1 的 `_Celsp_RequestExpired`、`_Celsp_PickPendingDrop`、`_Celsp_MessageIsCurrent`。
- Produces: `void CeLspClient_OnRequestTimeoutTimer(void)`；`CELSP_PENDING` 新字段；`IDT_TIMER_CELSP_TIMEOUT`。

- [ ] **Step 1: 写失败测试**

在 `test/TestCeLsp.cpp` 的 `TestSessionPolicy()` 中增加断言：同 kind 的新请求会使旧请求成为 drop 候选；deadline 已过的 pending 必须被判定过期。`TestCeLsp.ps1` 已抽取 Task 1 helper，因此只需断言 helper 行为，不直接构造 `CELSP_PENDING`。

- [ ] **Step 2: 运行 `pwsh -NoProfile -File test\TestCeLsp.ps1`，确认当前行为未覆盖该路径**

Expected: helper 测试通过，但产品代码尚未使用 helper；此步只确认测试基础设施正常。

- [ ] **Step 3: 扩展 pending 结构并在 `_SendRequest()` 写入 deadline/generation**

`deadline = GetTickCount64() + (fast kind ? CELSP_FAST_REQUEST_TIMEOUT_MS : CELSP_REQUEST_TIMEOUT_MS)`；fast kind 为 completion/signature/hover。`generation` 取当前 `s_generation`。

- [ ] **Step 4: 改造 `_AddPending()`**

用 `_Celsp_PickPendingDrop()` 选择要移除的 pending：同 kind 已存在时优先移除该 kind 最早 deadline；否则用 helper 选全局最早 deadline。移除后追加新请求。

- [ ] **Step 5: 实现 `CeLspClient_OnRequestTimeoutTimer()`**

每 1000ms 定时器触发：复制过期项的 `id/kind/generation` 后从 pending 移除；对每个过期 id 发 `$/cancelRequest`；不清空 UI 装饰。定时器只在 `s_pendingCount>0` 时由 `CeLspClient_Start()`/`_SendRequest()` 启动，pending 归零时 `KillTimer()`。

- [ ] **Step 6: 在 `Notepad3.c` 的 `WM_TIMER` 分支接入**

与 `IDT_TIMER_CELSP_SYNC/HILITE` 并列，调用 `CeLspClient_OnRequestTimeoutTimer()` 并 `return TRUE`。

- [ ] **Step 7: 处理 id 回绕**

`_SendRequest()` 中若 `s_nextId` 超过 `INT_MAX-1`，置回 1 并清空 pending（不清空发送队列，避免丢在途帧）；记录日志。

- [ ] **Step 8: 验证**

Run: `pwsh -NoProfile -File test\TestCeLsp.ps1`
Expected: PASS。随后手动触发服务端无响应场景，确认 3~8 秒后日志出现取消且新请求可继续。

### Task 3: 异步 teardown 与事件句柄所有权

**Files:**
- Modify: `src/CeLspClient.h:22-24`（新增 `CELSP_STATUS_TEARDOWN_DONE=4`）
- Modify: `src/resource.h:88-89`（新增 `IDT_TIMER_CELSP_TEARDOWN=246`）
- Modify: `src/CeLspClient.c:660-808`（句柄创建/复用）、`810-912`（`_Teardown` 拆分）、`1351-1370`（状态处理）
- Modify: `src/Notepad3.c:2813-2826`（`WM_CELSP_STATUS` 与 timer 兜底）

**Interfaces:**
- Consumes: Task 1 的 `_Celsp_MessageIsCurrent`。
- Produces: `CELSP_STATUS_TEARDOWN_DONE`；`IDT_TIMER_CELSP_TEARDOWN`；内部 `_TeardownAsync(bool final)` 与 `_FinalizeTeardown(UINT generation)`。

- [ ] **Step 1: 写失败测试**

`test/TestCeLsp.cpp` 增加 `_Celsp_MessageIsCurrent` 对 teardown-done generation 的断言：`(oldGen,newGen)==false`。运行 `pwsh -NoProfile -File test\TestCeLsp.ps1` 确认 helper 断言存在。

- [ ] **Step 2: 把事件句柄改为会话内复用**

`s_hEvent`/`s_hStopEvent` 仍由 `CeLspClient_Start()` 首次创建；`_Teardown(false)` 只 `ResetEvent()`，不 `CloseHandle()`；只有 `CeLspClient_Stop()` final 路径在线程确认退出后关闭。`_Enqueue()` 仍在锁内复制事件句柄到本地变量后再 `SetEvent()`。

- [ ] **Step 3: 拆分 `_Teardown()`**

`_Teardown(bool final)` 保留状态切换、generation 递增、入队 shutdown/exit、置事件；非 final 时把原等待/关闭/清队列代码搬到 `_FinalizeTeardown(UINT generation)`，由 `_TeardownWorker()` 调用，完成后 `PostMessageW(WM_CELSP_STATUS, CELSP_STATUS_TEARDOWN_DONE, generation)`。

- [ ] **Step 4: final 路径保持同步**

`CeLspClient_Stop()` 调用同步 `_Teardown(true)`，仍不强制杀线程；这是进程退出路径。

- [ ] **Step 5: 接入 UI 消息与定时器兜底**

`CeLspClient_OnStatus(CELSP_STATUS_TEARDOWN_DONE, ...)` 只在 generation 匹配时 `KillTimer(IDT_TIMER_CELSP_TEARDOWN)` 并做 UI 清理；`Notepad3.c` 的 timer 分支在超时后调用 `_FinalizeTeardown()` 的 UI 安全部分，不等待线程。

- [ ] **Step 6: 验证**

Run: 编译 `src\Notepad3.vcxproj /t:ClCompile /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal`
Expected: 无新增 C/C++ 错误。手动强制结束 `CeLspServer.exe`，确认编辑窗口无 1 秒级卡顿且日志显示重启。

### Task 4: stderr 独立管道

**Files:**
- Modify: `src/CeLspClient.c:700-740`（创建第三管道）、`328-353`（新增 `_StderrThread`）、`810-912`（拆除线程列表）

**Interfaces:**
- Consumes: Task 3 的事件句柄复用规则。
- Produces: `_StderrThread`、`s_hStderrR/s_hStderrW`、`s_hThreadStderr`。

- [ ] **Step 1: 写失败测试**

`test/TestCeLspEndToEnd.ps1` 中增加脚本化服务器：stdout 写合法 `publishDiagnostics` 帧，stderr 写 `non-protocol log line`；断言 stdout 帧仍按 `Content-Length` 解码、stderr 文本被单独捕获且不进入 stdout 字节流。本层不假称验证客户端日志 UI，客户端日志只做手动冒烟。先在脚本中写入期望输出，此时脚本不存在或不完整。

- [ ] **Step 2: 创建独立 stderr 管道**

`CreatePipe(&hErrR,&hErrW,...)`，`si.hStdError=hErrW`；父进程关闭 `hErrW`；`hErrR` 交给 stderr 线程。

- [ ] **Step 3: 实现 `_StderrThread`**

用 `ReadFile` 阻塞读，遇到数据整块 `PostMessageW(WM_CELSP_STATUS, CELSP_STATUS_ERROR, heapMessage)`；`s_hStopEvent` 置位且管道读完退出；退出时关闭 `hErrR`。

- [ ] **Step 4: 把 stderr 线程加入 teardown worker 的等待集合**

不得强制杀线程；若超时，沿用现有“句柄由线程退出时释放”策略。

- [ ] **Step 5: 验证**

Run: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1`
Expected: PASS，stdout 帧 JSON 可解析，stderr 文本出现在日志断言中。

### Task 5: 增量文档同步与服务端应用

**Files:**
- Modify: `src/CeLspClient.h:88-93`（新增 `CeLspClient_OnDocumentRangeChanged()`）
- Modify: `src/CeLspClient.c:461-532`（`_FlushChanges`）、`924-958`（生命周期）、`1-120`（范围数组静态状态）
- Modify: `src/Notepad3.c:10591-10628`（SCN_MODIFIED 钩子）
- Modify: `ce_lsp_server/CeLspServer.c:3421-3434`（`_HandleDidChange`）

**Interfaces:**
- Consumes: Task 1 的 `_Celsp_MergeChangeRange`、Task 4 的会话状态。
- Produces: `void CeLspClient_OnDocumentRangeChanged(DocPos position, DocPos length, int linesAdded, const char* text)`；服务端 `static bool _ApplyRangeChange(const char* text, int startLine, int startChar, int endLine, int endChar)`。

- [ ] **Step 1: 写失败测试**

新建 `test/TestCeLspEndToEnd.ps1`：向服务端发送完全相同的文档依次用全量 `didChange` 和增量 `didChange`（含中文、多行插入、删除），再发 `textDocument/hover` 或 `documentSymbol` 断言服务端表现一致。若服务端二进制不存在，脚本用源码编译的 harness 替代并标注 SKIP 真实服务器子项。

- [ ] **Step 2: 运行脚本确认失败**

Run: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1`
Expected: FAIL，增量 didChange 未被服务端识别。

- [ ] **Step 3: 客户端累积变更范围**

`CeLspClient_OnDocumentRangeChanged()` 只在 `CeLspFeatures_IsEnabled()` 且 UTF-8 会话时累积；`SC_MOD_INSERTTEXT` 用 `[position,position+length)`，`SC_MOD_DELETETEXT` 用 `[position,position+length)`，text 为空；`linesAdded!=0` 不改变字节范围。范围超过 256 段或发生 didOpen/文档切换时设 `s_fullSync=true`。

- [ ] **Step 4: `_FlushChanges()` 生成 contentChanges**

`s_fullSync==true` 时保持现有全量文本；否则把累积范围转为 LSP `Position`（通过现有 `CeLspPos_OffsetToPos` 或等价接口），生成 `range/text` 对象；发送后清空范围数组和 `s_fullSync`。

- [ ] **Step 5: 服务端应用增量 change**

`_HandleDidChange()` 检测 `contentChanges[i]` 是否有 `range`：无 range 保持 `_SetText()`；有 range 时按 LSP position 转 UTF-8 字节偏移，替换 `g_docText` 区间，更新 `g_docLen`。只使用最后一个 change 的语义改为依次应用所有 changes。

- [ ] **Step 6: 不一致时降级**

服务端若检测到 range 越界/负长度，返回全量重同步请求不可行时，客户端在下一次 flush 发送全量文档；日志记录一次 `incremental sync desync, falling back to full sync`。

- [ ] **Step 7: 验证**

Run: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1`
Expected: PASS，增量与全量最终文档一致。

### Task 6: 服务端诊断范围刷新、semanticTokens 缓存、补全防抖

**Files:**
- Modify: `ce_lsp_server/CeLspServer.c:534-760,3415-3495`（诊断与缓存）
- Modify: `src/CeLspClient.c:1007-1032`（补全防抖）、`946-958`（刷新节流）
- Modify: `src/CeLspFeatures.c:435-441,520-528`（取消防抖）、`1927-1941`（inlay/codeLens 请求开关）
- Modify: `src/resource.h:88-89`（新增 `IDT_TIMER_CELSP_COMPLETION=247`）
- Modify: `src/Notepad3.c:2813-2826`（timer 接入）

**Interfaces:**
- Consumes: Task 5 的变更范围、Task 2 的 pending 超时。
- Produces: `void CeLspClient_OnCompletionTimer(void)`；服务端 `static void _PublishDiagnosticsForLines(int firstLine, int lastLine)`；服务端 semanticTokens 版本缓存。

- [ ] **Step 1: 写失败测试**

在 `test/TestCeLspEndToEnd.ps1` 增加：
- 同版本连续两次 `semanticTokens/full`，第二次响应来自缓存（输出相同且服务端不重算标记可间接通过固定输入断言）；
- didChange 只改第 10 行时，未变更第 3 行诊断仍存在；
- 连续两次 completion 请求间隔 20ms 的客户端防抖不在 E2E 脚本断言，改由 Task 1 的纯策略测试和手动冒烟覆盖。

- [ ] **Step 2: 运行脚本确认失败**

Run: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1`
Expected: FAIL 或 SKIP 项存在，直到实现完成。

- [ ] **Step 3: 实现服务端诊断缓存**

把 `g_diagCount/g_diag[]` 扩展为按行缓存；`_PublishDiagnosticsForLines()` 只替换受影响行；didOpen/didSave/全量 change 或跨行状态变化时走原 `_PublishDiagnostics()` 全量扫描。

- [ ] **Step 4: 实现 semanticTokens 版本缓存**

服务端保存 `{version,tokens}`，请求版本相同则直接返回缓存；客户端 `RefreshDecorations()` 记录上次成功版本，相同版本不重复请求。

- [ ] **Step 5: 实现 completion 防抖**

`CeLspClient_RequestCompletion()` 保存参数到文件静态结构并启动 `IDT_TIMER_CELSP_COMPLETION`；`CeLspClient_OnCompletionTimer()` 调用原发送逻辑；`CeLspFeatures_OnAutoCCancelled()` 和选择完成时 `KillTimer`。

- [ ] **Step 6: 让 inlayHint/codeLens 开关生效**

`RefreshDecorations()` 在 `Settings2.CeLspInlayHints`/`CeLspCodeLens` 为真时发起请求；默认 false 不变。渲染继续走 `_RenderAnnotations()`。

- [ ] **Step 7: 容量保护**

`_ParseCapabilities()` 中若 semantic token legend >32，清 `CELSP_CAP_SEMTOKENS` 并记日志；其他固定数组超限记录一次日志并截断，不得越界。

- [ ] **Step 8: 验证**

Run: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1`
Expected: PASS。Run: `pwsh -NoProfile -File test\TestCeLsp.ps1` Expected: PASS。

### Task 7: 配置默认值、INI 与服务器参数/长路径

**Files:**
- Modify: `src/TypeDefs.h:926-934`（`CeLspServerPath` 改 `HPATHL`，新增 `CeLspServerArgs[256]`）
- Modify: `src/CeSettings.c:351-360`（补齐默认值）
- Modify: `src/Config/Config.cpp:1236-1247,2201-2208`（读写新字段）
- Modify: `Build/Notepad3.ini:80-84`（补全 8 个键和参数说明）
- Modify: `src/CeLspClient.c:683-698`（动态命令行）

**Interfaces:**
- Consumes: 无。
- Produces: `Settings2.CeLspServerArgs`；`Settings2.CeLspServerPath` 为 `HPATHL`。

- [ ] **Step 1: 写失败测试**

`test/TestCeLspList.ps1` 或新增纯文本断言：读取 `CeSettings.c`、`Config.cpp`、`Build/Notepad3.ini`，断言 4 个缺失 bool 默认值出现、8 个 INI 键出现、`CeLspServerArgs` 被读写。文本断言可由 PowerShell `Select-String` 完成。

- [ ] **Step 2: 运行确认失败**

Run: `pwsh -NoProfile -File test\TestCeLspList.ps1`
Expected: FAIL，缺失键报告。

- [ ] **Step 3: 改字段与默认值**

`CeLspServerPath` 改 `HPATHL` 后，所有初始化/释放点按现有 `HPATHL` 生命周期处理；`CeSettings_ResetDefaults()` 补 4 个 bool 默认值与参数空串。

- [ ] **Step 4: INI 读写与示例**

沿用 `IniSectionGetBool/SetBool`；新增 `CeLspServerArgs` 用字符串读写；`Build/Notepad3.ini` 列出全部键，注释默认值。

- [ ] **Step 5: 动态命令行**

`CeLspClient_Start()` 用 `Path_Get(Settings2.CeLspServerPath)` 得到路径，按 `pathLen + argsLen + 8` 动态分配 `WCHAR*`；无自定义路径时仍拼 `<模块目录>\CeLspServer.exe`；`CreateProcessW` 命令行加 args（若有）。

- [ ] **Step 6: 验证**

Run: `pwsh -NoProfile -File test\TestCeLspList.ps1`
Expected: PASS。Run 编译 x64，确认无 struct 初始化遗漏。

### Task 8: 诊断列表与跳转

**Files:**
- Modify: `src/CeLspFeatures.h:42-57`（新增 `CeLspFeatures_ShowDiagnostics()`）
- Modify: `src/CeLspFeatures.c:936-971`（诊断缓存）、`1003-1425`（复用符号窗口模式）
- Modify: `language/common_res.h:899`（新增 `IDM_CE_LSP_DIAGNOSTICS=41069`）
- Modify: `language/np3_en_us/menu_en_us.rc:463-479`
- Modify: `language/np3_zh_cn/menu_zh_cn.rc:463-479`
- Modify: `src/Notepad3.c:7471-7580`（命令分发）、`2813-2826`（如需要）

**Interfaces:**
- Consumes: Task 6 的诊断缓存，Task 7 的设置。
- Produces: `void CeLspFeatures_ShowDiagnostics(void)`；`IDM_CE_LSP_DIAGNOSTICS`。

- [ ] **Step 1: 写失败测试**

在 `test/TestCeLsp.cpp` 增加纯 helper 断言：`_Celsp_DiagIsCurrent()` 对 severity 无关的 URI/version/generation 组合按现有策略返回 true/false；另加 `_Celsp_DiagSeverityForSquiggle(int)` 断言 1/2 为 true、3/4 为 false。客户端诊断缓存的 UI 表现不计入自动化断言。

- [ ] **Step 2: 运行确认失败**

Run: `pwsh -NoProfile -File test\TestCeLsp.ps1`
Expected: FAIL，诊断 severity 过滤 helper 不存在。

- [ ] **Step 3: 缓存诊断**

`CeLspFeatures.c` 增加文件静态 `LSP_DIAG s_diags[512]` 与 `s_diagUri/s_diagVersion`；`OnDiagnostics()` 先按当前 URI/版本过滤，解析全部 severity 到缓存，再按 1/2 渲染 marker/indicator。

- [ ] **Step 4: 实现列表窗口**

复用 `_SymWndProc` 的创建/双击跳转/关闭模式；显示 `severity/source/message/line:char`；双击调用 `SciCall_SetSel` 到缓存 range 起点并聚焦编辑窗口。无诊断时显示占位文本。

- [ ] **Step 5: 接命令与菜单**

`common_res.h` 新增 ID；中英文 CE 菜单各加一项；`MsgCommand()` 的 CE 分支调用 `CeLspFeatures_ShowDiagnostics()`。

- [ ] **Step 6: 验证**

Run 编译 x64 与 Win32，确认资源 ID 不冲突。手动打开含 warning/info 的脚本，确认列表显示 3 条、双击跳转正确、编辑器只画 2 条 squiggle。

### Task 9: 全量验证与文档收尾

**Files:**
- Modify: `docs/superpowers/specs/2026-11-18-lsp-client-hardening-optimization-design.md`（状态改为已实现/部分待运行时验证）
- Modify: `docs/README.md`（如新增了 harness，加入索引行）

**Interfaces:**
- Consumes: Task 1-8 全部输出。
- Produces: 验证记录。

- [ ] **Step 1: 运行纯测试**

Run: `pwsh -NoProfile -File test\TestCeLsp.ps1`
Expected: PASS。

Run: `pwsh -NoProfile -File test\TestCeLspList.ps1`
Expected: PASS。

Run: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1`
Expected: PASS；若真实 `CeLspServer.exe` 不存在，记录真实服务器子项为 SKIP，不把 SKIP 写成 PASS。

- [ ] **Step 2: 运行其他针对性测试**

Run: `pwsh -NoProfile -File Build\scripts\Test-Docs.ps1`
Expected: PASS。

Run: `cmd /d /c "call test\TestFileVersion.cmd"`
Expected: PASS；若失败，先确认是否基线失败。

- [ ] **Step 3: 运行构建**

Discover MSBuild:
`& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1`

Run: `& $msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal`
Expected: 0，无新错误。

Run: `& $msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal`
Expected: 0，无新错误。

- [ ] **Step 4: 更新设计状态与文档索引**

把设计文档状态改为“已实现（静态/自动化通过项列明；GUI 项按实际执行结果标注）”；若新增 harness 未进入索引，补一行。

- [ ] **Step 5: 不提交**

Run: `git --no-pager status --short`
Expected: 仅显示本计划涉及文件；不执行 `git commit`。
