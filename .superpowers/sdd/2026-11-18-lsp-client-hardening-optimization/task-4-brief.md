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

