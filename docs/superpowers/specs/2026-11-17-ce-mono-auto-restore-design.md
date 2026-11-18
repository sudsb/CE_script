# CE 自动汇编导致 Mono 丢失：状态检测与自动恢复设计

日期：2026-11-17　状态：已实现（静态校验通过，运行时验证待具备 Mono 目标环境后执行）

## 1. 问题与目标

现象：CE 7.7 中「Mono → Activate mono features」已启用时，经本编辑器触发自动汇编操作（右键「自动汇编」模板、AA 启用/禁用、CE 语法检查），Mono 有较大概率被终止（`monopipe=nil`、符号功能失效、菜单勾选丢失）。

目标：桥接层增加 Mono 状态检测，确认「非用户主动关闭地丢失」时自动重新激活并记日志。

非目标：不改 CE 自带 `monoscript.lua`（上游文件）、不在非 Mono 目标强行注入、不替代用户显式关闭。

## 2. 根因（证据：CE 7.7 本机 `autorun/monoscript.lua`）

Mono 状态全在 CE 的 Lua 全局里，无 API 可直接查询「是否激活」（行号为 CE 7.7 `monoscript.lua`）：

- `libmono.monopipes[tid]`：每线程一条管道，`getMonoPipe()`（约 382 行）按 `getCurrentThreadID()` 取/建。
- `monopipe`：全局兼容变量，**只在主线程赋值**（约 545 行）；非主线程激活不设置它。
- `libmono.terminate()`（约 50 行）：清空所有管道、`monopipe=nil`、`ProcessID=nil`、`mono_AttachedProcess=nil`，注销回调 —— 即「丢失」的实质。
- `MDC_ShuttingDownAddress`：目标进程内 `MDC_ShuttingDown` 地址；HeartBeat（约 1460 行）每 500 ms 检测非 0 即 `terminate()`。
- `getMonoPipe()` 的 `OnTimeout`/`OnError`（约 509 行）：置 `libmono.fail=true` 并抛错。
- `mono_OpenProcessMT`（约 6163 行）：`ProcessID ~= getOpenedProcessID()` 时断链、`monopipe=nil`。
- `miMonoActivateClick`（约 5980 行）：唯一显式切换入口，用户关闭时置 `Checked=false`；菜单展开时刷新 `Checked=monopipe~=nil`（约 6059 行）。
- `USEMONO` 等注册为 AA 命令（约 7256 行）：AA 脚本可触发 `LaunchMonoDataCollector()`。

丢失路径推断（具体哪条需运行时日志确认）：

1. 桥接脚本一律经 `synchronize()` 在主线程执行（`np3_ce_bridge.lua` 的 `onMainThread`）。CE 内部 autoAssemble 在自己线程触碰 `getMonoPipe()`；超时/出错 → `libmono.fail=true` → 全局失败态。
2. `createAutoAssemblerForm('')` 隐藏工厂反复生成模板时，目标若 `isPaused()`，`_LaunchMonoDataCollector()`（约 994 行）直接返回 `nil`。
3. AA 注入改变目标模块布局，触发 `MDC_ShuttingDown` → HeartBeat → `terminate()`。
4. 目标进程重启/PID 变化，`mono_OpenProcessMT` 主动断链。

恢复入口唯一：`LaunchMonoDataCollector()`（约 1492 行，内部 `synchronize` 到主线程、重置 `abort/fail`、重建管道与回调、置 `ProcessID`）。

## 3. 关键约束

- 线程：`LaunchMonoDataCollector()` 必须在主线程；命令路径经 `onMainThread()`，定时器本身已在主线程（见取舍）。
- 用户意图：仅当 `Checked==true` 且曾观察到 attached 才恢复；不用 `getTableOption('UsesMono')`（不随显式关闭更新，约 7215 行）。
- 前置：`getOpenedProcessID()~=0`、`not isPaused()`、确为 Mono/.NET 目标（`miMonoTopMenuItem` 存在且 `Visible==true`；读不到 Visible 时保守判为非目标，避免误注入）。
- 幂等/限流：管道存在且 `Connected` 时不动；失败退避，避免反复弹框或重复注入。
- 不做 monkey-patch：不覆盖 `libmono.terminate`、`miMonoActivateClick`，只加旁路检测。
- 取舍：定时器和主线程回调直接调用恢复函数，不再包 `synchronize`——CE 从主线程调它是原地执行，嵌套无收益；`LaunchMonoDataCollector()` 内部自带的 `synchronize` 保持不动。

## 4. 方案（CE Lua 侧，改 `ce_bridge/np3_ce_bridge.lua`）

### 4.1 状态快照 `_monoSnapshot()`

主线程读取，返回 `{ wanted, attached, connected, failed, paused, pid }`：

- `wanted`：`menuChecked(MainForm.miMonoActivate)==true`，且桥接曾观察到 `attached==true`（gate，避免从未启用就误触发）。`Checked` 可靠：terminate 不清它，被动丢失后保持 true，只有用户显式关闭才置 false。菜单展开会刷新它——用户此时已看到 Mono 关闭，属可接受边界。
- `attached`：`libmono.monopipes[主线程tid]~=nil` 或 `monopipe~=nil`，且 `libmono.ProcessID==getOpenedProcessID()`。
- `connected`：管道存在且 `Connected==true`。
- `failed`：`libmono.fail` 或 `libmono.abort` 为真，或 `MDC_ShuttingDownAddress` 可读且非 0。
- `paused`：`isPaused()`；`pid`：`getOpenedProcessID()`。

「丢失」充分条件：`wanted and (not attached or not connected or failed)`。

### 4.2 恢复 `_monoRestore(snapshot)`

- `not wanted` / `pid==0` / `paused` / 已有恢复在途：跳过并记录 reason。
- `pcall(LaunchMonoDataCollector)`，失败判据同时覆盖返回 `0`/`nil`/`false` 与调用后 `attached` 仍为假。
- 成功后强制 `MainForm.miMonoActivate.Checked=true`（CE 激活函数不保证设置它），刷新 `libmono.fail/abort`。
- 日志：`[MONO] 已自动重新激活 / 重新激活失败 (原因: X)`，只经 `print()` 输出——脚本路径下 `print` 已重定向到 Notepad3 面板，看门狗/菜单路径下落到 CE Lua 窗口。**禁止**另开 `notifyPipe` 写帧：那会与 listener 的帧写入并发、破坏字节流帧协议。

### 4.3 触发时机（两条都要）

1. **操作后主动检测**：`generateAndEdit()`、`generateDefaultTemplate()`、`checkScript()`、`np3.applySession()`（AA 写表）及 `CMD_RUN` 完成后调用 `_monoEnsure()`（内部走 `onMainThread`）——本编辑器触发 AA 的汇聚点。
2. **低频看门狗**：`createTimer`（约 1500-2000 ms，主线程）同样调用 `_monoEnsure()`，覆盖 CE 自身 AA 编辑器等非桥接路径。随 `bridge.running=false` 销毁。

### 4.4 退避

`bridge.monoRecover={attempts,nextAt,lastReason,pid}`：成功清零；失败 `attempts++`、`nextAt=now+min(30,5*attempts)秒`；连续 3 次失败后停止，之后靠「重新观察到 healthy」（用户重开 Mono 后 CE 自行激活，下 tick 即 healthy）或手动 `np3.monoRestore()` 重置。显式关闭不清 `ever`，重开后仍可恢复。暂停/未附加不计入失败。**PID 变化时重置整个退避状态**，否则累计满 3 次后新进程永久沉默；快照检测 `pid` 变化即清零。

### 4.5 手动入口与可观测性

- `np3.monoState()` 返回快照；`np3.selftest()` 增 `mono=wanted/attached/connected/failed` 行；`np3.monoRestore()` 忽略退避强制恢复，供测试兜底。

## 5. 待确认 / 风险

1. **`LaunchMonoDataCollector()` 失败副作用（已实现取舍）**：失败返回 `0`/`nil`，可能弹模态框（约 996 行）阻塞主线程。失败判据已覆盖返回值与调用后 `attached`。**取舍**：看门狗保留注入（覆盖 CE 自带 AA 等非桥接路径丢失的唯一途径），靠退避限制至多 3 次后停止；模态框属 CE 上游行为无法改非模态。代价：极端情况最多 3 次弹框短暂阻塞桥接。
2. **恢复再触发丢失**：激活函数会销毁当前线程旧管道（约 1024 行），有短暂空窗；用 `nextAt` 冷却避开。
3. **目标暂停**：AA 断点命中 `isPaused()==true`，恢复须推迟；定时器推送即可，不阻塞读帧线程。
4. **版本差异**：本机 7.7 与上游 master 行号/对象不同；一律运行时 `type()`/`pcall` 探测，禁止硬编码对象存在性。

## 6. 验证计划

- 静态：`test/TestCeBridgeLua.ps1`（stock Lua 5.3）跑纯函数解析与桩行为断言。
- 运行时（需桌面 + CE 7.7 + Mono/Unity 目标）：① 附加目标并激活 Mono → `np3.monoState()` 应 `wanted=1 attached=1 connected=1`；② 多次生成 AA 模板 / CE 检查 / 启用禁用 AA 制造丢失 → 出现 `[MONO] 已自动重新激活`、状态恢复、Mono Dissect 可用；③ 用户手动关闭 → 等 10 秒确认不被重新激活；④ 附加非 Mono 进程 → 确认不注入、不弹框。
- 无 Mono 目标环境时，运行时验证记为「未执行」，不得声称已验证。
