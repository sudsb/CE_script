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

