# CE Mono 自动恢复 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 CE 桥接脚本中增加 Mono 状态检测与非用户关闭的丢失自动恢复，并通过纯函数行为断言验证判定与退避逻辑。

**Architecture:** 在 `ce_bridge/np3_ce_bridge.lua` 中新增三段：只读 CE 全局的 `_monoSnapshot()`、纯决策 `_monoRecoverDecision(s,rec,now)`、编排与 IO 的 `_monoEnsure()`。所有 CE 副作用集中到 `_monoEnsure()`，使前两者可用桩全局在 stock Lua 中直接断言。决策结果由 `_monoEnsure()` 落回 `bridge.monoRecover`；恢复经 `LaunchMonoDataCollector()` 并与 `bridge.notifyPipe` 输出面板联动。

**Tech Stack:** Cheat Engine 7.7 Lua 5.3（主线程 / synchronize）、PowerShell 测试夹具（`test/TestCeBridgeLua.ps1`，P/Invoke 加载 `lua53-64.dll`）。

**Spec:** `docs/superpowers/specs/2026-11-17-ce-mono-auto-restore-design.md`

## Global Constraints

- `LaunchMonoDataCollector()` 必须在 CE 主线程调用；非主线程只经 `synchronize` 排队。
- 用户意图只取 `MainForm.miMonoActivate.Checked`（经 `menuChecked`），**禁止**用 `getTableOption('UsesMono')`（显式关闭后不更新）。
- 恢复前置：`pid ~= 0`、`not isPaused()`、目标为 Mono/.NET（`miMonoTopMenuItem` 且 `Visible`）。
- 管道存在且 `Connected` 时不动作；意图为假不动作；从未观察到活跃不动作。
- 退避：失败 `attempts+1`、`nextAt = now + math.min(30, 5*attempts)`；`attempts >= 3` 停止自动恢复；`pid` 变化重置退避（不清 ever）；成功清零。
- 不 monkey-patch `libmono.terminate` / `miMonoActivateClick`；不硬编码对象存在性，统一 `type()` / `pcall` 探测。
- 恢复成功后强制 `MainForm.miMonoActivate.Checked = true`（CE 激活函数不保证设置）。
- 工作区非 git 仓库：不写 commit 步骤，验证通过即完成该任务。

## Review Focus

- **用户显式关闭 Mono 后不得被重新激活**（意图误判会让用户关不掉）→ Task 1 断言 `wanted=false` 返回 `skip/not-wanted`。
- **从未启用过 Mono 的目标不得被注入**（枚举不稳定，误注入会弹框）→ Task 1 断言 `rec.ever=false` 返回 `skip/never-active`；Task 2 断言 `target=false` 时定位为 skip。
- **进程暂停时不得尝试恢复**（CE 会弹「cannot use while frozen」，且阻塞桥接）→ Task 1 断言 `paused=true` 返回 `skip/paused`。
- **PID 变化后必须重置退避**（否则累计 3 次失败的新进程永久沉默）→ Task 1 断言 PID 变化返回 `resetPid=true` 且 `act=restore`。
- **恢复成功后菜单勾选必须同步**（否则下一 tick 误判意图）→ Task 2 端到端断言成功后 `Checked=true` 且 `rec.attempts=0`。

---

### Task 1: 只读快照与纯决策函数

**Files:**
- Modify: `ce_bridge/np3_ce_bridge.lua`（在 `reportError` 之后插入两个 local function；`menuChecked` 与 `menuVisible` 已在上方，可直接引用。`runScript` 在更上方定义，Task 3 通过 `bridge.monoEnsure` 间接调用，不受定义顺序影响）
- Test: `test/TestCeBridgeLua.ps1`（追加行为断言块）

**Interfaces:**
- Consumes: `menuChecked(item)`、`menuVisible(item)`（Task 1 所在文件已有，签名 `item -> boolean|nil`）。
- Produces:
  - `_monoSnapshot() -> table { wanted, attached, connected, failed, paused, pid, target }`（snapshot 自身只读 CE 全局；`ever` 由调用方从 `bridge.monoRecover.ever` 注入后传入 decision）
  - `_monoRecoverDecision(s, rec, now) -> table { act, reason, setEver, pid, resetPid }`，`act ∈ {'healthy','skip','reset-pid','restore'}`

- [ ] **Step 1: 在测试脚本追加失败断言**

在 `test/TestCeBridgeLua.ps1` 末尾（`Write-Host 'PASS'` 之前）新增：抽取 `_monoSnapshot`、`_monoRecoverDecision`、`menuChecked`、`menuVisible` 四个 `local function`（沿用现有 `(?ms)^local function name\(.*?^end\s*$` 抽取方式），拼入一个 Lua chunk，桩掉全部 CE 全局（`MainForm`, `libmono`, `monopipe`, `getOpenedProcessID`, `getCurrentThreadID`, `isPaused`, `readInteger`, `miMonoTopMenuItem`, `getProperty`, `os.time` 用真实库），再追加断言：

```
-- 快照：意图/连接/失败/目标/暂停
s = _monoSnapshot(); assert(s.wanted==true and s.attached==true and s.connected==true)
assert(s.failed==false and s.paused==false and s.pid==4242 and s.target==true)
-- 用户显式关闭（Checked=false）→ wanted=false
miMonoActivate.Checked = false
assert(_monoSnapshot().wanted==false)
-- MDC_ShuttingDown 非 0 → failed
miMonoActivate.Checked = true; readInteger = function(a) if a=='SD' then return 1 end return 0 end
assert(_monoSnapshot().failed==true)
-- 管道不存在 → attached=false
monopipe = nil; libmono.monopipes = {}; assert(_monoSnapshot().attached==false)
```

`_monoRecoverDecision` 断言（每次用字面表构造 `s`/`rec`，`now=1000`）：

```
local healthy = {wanted=true, attached=true, connected=true, failed=false, paused=false, pid=7, target=true}
d = _monoRecoverDecision(healthy, {ever=true, attempts=0, nextAt=0, pid=7}, 1000)
assert(d.act=='healthy' and d.setEver==true)

local lost = {wanted=true, attached=false, connected=false, failed=false, paused=false, pid=7, target=true}
assert(_monoRecoverDecision(lost, {ever=true, attempts=0, nextAt=0, pid=7}, 1000).act=='restore')
assert(_monoRecoverDecision(lost, {ever=false, attempts=0, nextAt=0, pid=7}, 1000).reason=='never-active')
local off = {}; for k,v in pairs(lost) do off[k]=v end; off.wanted=false
assert(_monoRecoverDecision(off, {ever=true, attempts=0, nextAt=0, pid=7}, 1000).reason=='not-wanted')
local p = {}; for k,v in pairs(lost) do p[k]=v end; p.paused=true
assert(_monoRecoverDecision(p, {ever=true, attempts=0, nextAt=0, pid=7}, 1000).reason=='paused')
local np = {}; for k,v in pairs(lost) do np[k]=v end; np.target=false
assert(_monoRecoverDecision(np, {ever=true, attempts=0, nextAt=0, pid=7}, 1000).reason=='not-target')
assert(_monoRecoverDecision(lost, {ever=true, attempts=0, nextAt=1500, pid=7}, 1000).reason=='cooldown')
assert(_monoRecoverDecision(lost, {ever=true, attempts=3, nextAt=0, pid=7}, 1000).reason=='exhausted')
local pidChange = _monoRecoverDecision(lost, {ever=true, attempts=3, nextAt=0, pid=9}, 1000)
assert(pidChange.act=='restore' and pidChange.resetPid==true)
local failState = {}; for k,v in pairs(lost) do failState[k]=v end; failState.failed=true
assert(_monoRecoverDecision(failState, {ever=true, attempts=0, nextAt=0, pid=7}, 1000).reason=='failed-state')
assert(_monoRecoverDecision({wanted=true, attached=true, connected=false, failed=false, paused=false, pid=7, target=true}, {ever=true, attempts=0, nextAt=0, pid=7}, 1000).reason=='disconnected')
```

- [ ] **Step 2: 运行测试确认失败**

Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
Expected: FAIL（`_monoSnapshot` / `_monoRecoverDecision` 抽取不到，报 `FAIL: ... not found`）

- [ ] **Step 3: 实现两个函数**

`_monoSnapshot()` 按 Global Constraints 读取：`wanted = menuChecked(MainForm and MainForm.miMonoActivate) == true`；`pid = getOpenedProcessID()`（失败回落 0）；`paused = isPaused() == true`；`target = miMonoTopMenuItem ~= nil and menuVisible(miMonoTopMenuItem) ~= false`；取 `libmono.monopipes[tid]` 或 `monopipe`（tid 来自 `getCurrentThreadID()`），`attached` 需管道非 nil 且 `libmono.ProcessID == pid`，`connected` 为管道 `.Connected == true`；`failed = libmono.fail or libmono.abort` 或 `readInteger(libmono.MDC_ShuttingDownAddress) ~= 0`（`readInteger` 不可读时回落 0，全部用 `pcall`）。所有 CE 全局用 `type(...)` 判存在。

`_monoRecoverDecision(s, rec, now)` 纯函数、无全局访问，按优先级返回：`healthy`（attached 且 connected，`setEver=true, pid=s.pid`）→ `skip/not-target` → `skip/no-process`（pid=0）→ `skip/paused` → `skip/not-wanted`（`setEver=false`）→ `skip/never-active`（`rec.ever=false`）→ `cooldown`（`now<rec.nextAt` 且无 resetPid）→ `exhausted`（`rec.attempts>=3` 且无 resetPid）→ 否则 `restore`。`resetPid = rec.pid ~= 0 and s.pid ~= rec.pid`，pid 变化和 exhausted/cooldown 同时成立时 `resetPid` 优先放行。`reason` 取 `failed-state` > `no-pipe` > `disconnected`。

- [ ] **Step 4: 运行测试确认通过**

Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
Expected: PASS（`PASS: CE bridge Lua self-check`）

- [ ] **Step 5: 完成**

工作区非 git 仓库，无需 commit。

---

### Task 2: 恢复编排、退避落回与可观测性

**Files:**
- Modify: `ce_bridge/np3_ce_bridge.lua`（紧接 Task 1 之后定义 `_monoLog`、`_monoEnsure`；新增 `bridge.notifyPipe` 指针；修改 `handleConnection` 首尾设置/清理该指针）
- Modify: `test/TestCeBridgeLua.ps1`（追加端到端断言块）

**Interfaces:**
- Consumes: `_monoSnapshot()`, `_monoRecoverDecision(s,rec,now)`（Task 1）。
- Produces: `_monoEnsure() -> nil`（副作用：更新 `bridge.monoRecover`，必要时调用 `LaunchMonoDataCollector`，经 `bridge.notifyPipe` 发 `KIND_OUTPUT`）；`bridge.monoRecover = { ever, attempts, nextAt, lastReason, pid }`；`bridge.notifyPipe`（当前连接，可能为 nil）。

- [ ] **Step 1: 追加端到端失败断言**

在测试脚本新增：抽取 `_monoSnapshot`、`_monoRecoverDecision`、`_monoLog`、`_monoEnsure`、`menuChecked`、`menuVisible`、`sendFrame` 到同一 chunk，桩 `LaunchMonoDataCollector`、`MainForm`、`libmono`、`monopipe`、`miMonoTopMenuItem`、`getOpenedProcessID`、`getCurrentThreadID`、`isPaused`、`readInteger`、`inMainThread`（返回 true）、`synchronize`（计数桩）、`KIND_OUTPUT` 常量、`print`（捕获日志）、`bridge`（含 `monoRecover` 初值、`notifyPipe=nil`、`echoToCe=false`）。断言：

```
-- 丢失 + ever → 调用激活一次，成功后勾选恢复、退避清零
libmono.monopipes = {}; monopipe = nil; libmono.ProcessID = 7
launchCalls = 0; LaunchMonoDataCollector = function() launchCalls=launchCalls+1; return true end
bridge.monoRecover = {ever=true, attempts=0, nextAt=0, pid=7}
_monoEnsure()
assert(launchCalls==1, 'restore not attempted')
assert(miMonoActivate.Checked==true, 'Checked not restored')
assert(bridge.monoRecover.attempts==0, 'attempts not cleared')
-- 成功桩需要让 snapshot 再次读到 attached：桩 Launch 时回填 monopipe/ProcessID
```

桩 `LaunchMonoDataCollector` 时需回填 `monopipe = {Connected=true}; libmono.ProcessID = 7`，使恢复后重读快照为 healthy。继续断言：

```
-- 用户关闭 → 不激活
miMonoActivate.Checked = false
_monoEnsure(); assert(launchCalls==1, 'closed mono was re-enabled')
-- 激活失败 → attempts 增加、冷却设值、日志含 [MONO]
miMonoActivate.Checked = true; monopipe=nil; libmono.monopipes={}
LaunchMonoDataCollector = function() return 0 end
bridge.monoRecover = {ever=true, attempts=0, nextAt=0, pid=7}
_monoEnsure()
assert(bridge.monoRecover.attempts==1, 'failed attempt not counted')
assert(bridge.monoRecover.nextAt>0, 'cooldown not set')
assert(logText:find('[MONO]', 1, true), 'no mono log line')
-- 无活动连接时不得向 notifyPipe 写帧（notifyPipe=nil 现状即可）
```

- [ ] **Step 2: 运行测试确认失败**

Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
Expected: FAIL（`_monoEnsure not found`）

- [ ] **Step 3: 实现 `_monoLog` 与 `_monoEnsure`**

`_monoLog(text)`：拼接 `'[MONO] ' .. tostring(text)`，`pcall(print, line)`；若 `bridge.notifyPipe` 非 nil，`pcall(sendFrame, bridge.notifyPipe, KIND_OUTPUT, line)`。

`_monoEnsure()` 步骤：① 若 `type(inMainThread)=='function'` 且不在主线程且有 `synchronize`，则 `synchronize(_monoEnsure)` 并 return（自排队，避免嵌套）；② `bridge.monoEnsuring` 防重入标志；③ `local s=_monoSnapshot(); s.ever=bridge.monoRecover.ever`；④ `local d=_monoRecoverDecision(s, bridge.monoRecover, os.time())`；⑤ 应用 `d.setEver`/`d.pid`/`d.resetPid`（resetPid 时清 `attempts/nextAt/lastReason` 并设 `pid`，保留 `ever`）；⑥ `act=='healthy'` 清 `attempts/nextAt/lastReason`；`act=='skip'` 记 `lastReason`；⑦ `act=='restore'` 时 `pcall(LaunchMonoDataCollector)`，重读快照，以 `ok and res and res~=0 and res~=false and s2.attached` 判成功：成功则 `setMenuChecked(miMonoActivate,true)`（失败回落直接赋值）、`ever=true`、`attempts=0`、`nextAt=0`，记日志 `restored (reason=..)`；失败则 `attempts+1`、`nextAt=now+min(30,5*attempts)`，记日志 `restore failed (reason=..)`；⑧ 清除防重入标志。

`bridge.notifyPipe`：`handleConnection(pipe)` 起始置 `bridge.notifyPipe = pipe`，函数末尾（`pipe.destroy` 之后或返回前）置 nil，仅当仍指向该 pipe 时清。

- [ ] **Step 4: 运行测试确认通过**

Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
Expected: PASS

- [ ] **Step 5: 完成**

工作区非 git 仓库，无需 commit。

---

### Task 3: 接入触发点、手动入口、完整回归与文档收尾

**Files:**
- Modify: `ce_bridge/np3_ce_bridge.lua`（6 处接入：`runScript` 末尾经 `bridge.monoEnsure()`；`generateAndEdit`、`generateDefaultTemplate`、`checkScript` 返回前；`np3.applySession` 的 `onMainThread` 回调内；server startup 区新增看门狗 timer；`np3.monoState`/`np3.monoRestore`；`np3.selftest` 增一行）
- Modify: `test/TestCeBridgeLua.ps1`（追加存在性静态检查）
- Modify: `docs/superpowers/specs/2026-11-17-ce-mono-auto-restore-design.md`（状态改为「已实现，待运行时验证」）

**Interfaces:**
- Consumes: `_monoEnsure()`（Task 2）。
- Produces: `np3.monoState() -> table`（快照 + `rec` 副本）；`np3.monoRestore() -> string`；`bridge.monoWatch`（看门狗 timer）。

- [ ] **Step 1: 追加静态检查断言**

在 `test/TestCeBridgeLua.ps1` 追加：

```
foreach ($needle in @('_monoSnapshot', '_monoRecoverDecision', '_monoEnsure',
                      'bridge.monoEnsure', 'np3.monoState', 'np3.monoRestore',
                      'bridge.monoRecover', 'bridge.notifyPipe', '[MONO]')) {
  if (-not $text.Contains($needle)) { Write-Host ('FAIL: mono auto-restore missing: ' + $needle); exit 1 }
}
# 命令路径守护：runScript 每帧结束后检测
$runFn = [regex]::Match($text, '(?ms)^local function runScript\(.*?^end\s*$')
if (-not $runFn.Success -or -not $runFn.Value.Contains('bridge.monoEnsure()')) {
  Write-Host 'FAIL: runScript does not run mono ensure'; exit 1 }
# 看门狗 timer 必须在已知 timer 区创建并带冷却
if ($text -notmatch 'monoWatch\.Interval\s*=\s*2000') {
  Write-Host 'FAIL: mono watchdog interval missing'; exit 1 }
```

- [ ] **Step 2: 运行测试确认失败**

Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
Expected: FAIL（新增静态检查未满足）

- [ ] **Step 3: 接入所有触发点**

在 `runScript()` 的 `print = originalPrint` 之后、`sendFrame(KIND_DONE, ...)` 之前插入 `if bridge.monoEnsure then pcall(bridge.monoEnsure) end`。在 `generateAndEdit`、`generateDefaultTemplate`、`checkScript` 的成功返回前，以及 `np3.applySession` 的 `onMainThread(function() result = applyScript(payload) end)` 之后，`pcall(_monoEnsure)`。在 server startup 两个现有 timer 之后新增 `createTimer(nil,false)`，`Interval=2000`、`OnTimer=function() if bridge.running then _monoEnsure() end end`、`Enabled=true`，存入 `bridge.monoWatch`。在 np3 API 区新增 `np3.monoState = function() local s=_monoSnapshot(); s.rec=bridge.monoRecover; return s end` 与 `np3.monoRestore = function() bridge.monoRecover.attempts=0; bridge.monoRecover.nextAt=0; _monoEnsure(); return 'requested' end`。在 `np3.selftest` 追加一行 `mono=wanted=.. attached=.. connected=.. failed=.. attempts=..`（取自 `_monoSnapshot()` 与 `bridge.monoRecover`）。在 mono 模块定义后加 `bridge.monoEnsure = _monoEnsure`。

- [ ] **Step 4: 运行完整测试与构建**

Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
Expected: PASS（含 Task 1、2 的行为断言与 Task 3 静态检查）

- [ ] **Step 5: 更新文档状态并复验**

把设计文档第 3 行改为「状态：已实现（静态校验通过，运行时验证待具备 Mono 目标环境后执行）」。Run: `pwsh -NoProfile -File Build\scripts\Test-Docs.ps1`。Expected: 仅剩基线 2 个 error（`domain/configuration-and-ini.md`、`domain/pathlib.md` 的 `metadata requires ... Git root`，因工作区无 `.git`，与本次改动无关），无新增 error/warn。

- [ ] **Step 6: 完成**

工作区非 git 仓库，无需 commit。
