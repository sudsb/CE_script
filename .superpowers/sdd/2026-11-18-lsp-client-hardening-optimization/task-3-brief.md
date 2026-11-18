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

