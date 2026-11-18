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

