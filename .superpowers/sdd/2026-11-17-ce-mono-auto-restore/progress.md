# SDD ledger — plan: docs/superpowers/plans/2026-11-17-ce-mono-auto-restore.md

Ruling: 工作区无 .git（git status 报 not a git repository），executing-plans 的 worktree/task-start/task-done/commit/review-package 脚本全部依赖 git，不可用 — 采用手工账本 + 每任务 TDD RED→GREEN + 最终独立审查者审查 — 代价：无 commit 边界，审查范围以改动文件列表界定。

Spec: docs/superpowers/specs/2026-11-17-ce-mono-auto-restore-design.md

Pre-flight 接口核对：
- Task1 产 `_monoSnapshot()` / `_monoRecoverDecision(s,rec,now)`，Task2 消费 — 签名一致（s 表字段由 Task1 定义，rec 字段 ever/attempts/nextAt/pid，now 秒数）。
- Task2 产 `_monoEnsure()` / `bridge.monoRecover` / `bridge.notifyPipe`，Task3 消费 — 名称一致。
- Task2 产 `bridge.monoEnsure`（runScript 间接调用点），Task3 消费 — 一致。
- 无冲突。

## Tasks

- [x] Task 1: 只读快照与纯决策函数 — tests: pwsh -NoProfile -File test\TestCeBridgeLua.ps1 → PASS (RED: FAIL mono helper not found; GREEN after impl)
- [x] Task 2: 恢复编排、退避落回与可观测性 — tests: pwsh -NoProfile -File test\TestCeBridgeLua.ps1 → PASS (RED: FAIL mono ensure helper not found: _monoLog; GREEN after impl + bridge state + notifyPipe lifecycle)
- [x] Task 3: 接入触发点、手动入口、完整回归与文档收尾 — tests: pwsh -NoProfile -File test\TestCeBridgeLua.ps1 → PASS; docs checker → only baseline 2 errors (no .git)

## Rulings

- Task 3: runScript 断言期望裸调用 `bridge.monoEnsure()`，实现用 `pcall(bridge.monoEnsure)` — 采用 pcall：检测逻辑抛错时不得中断用户脚本的结果回传 — 代价：测试断言需匹配 pcall 形式（已改）。

## Final review

独立审查者（code-explorer）报告：无 Critical；9 项 Important/Minor。重新分级与裁决：

- **Final: fixed notifyPipe 并发写（审查 Issue 1，re-grade 为 Critical）** — watcher 主线程经 `notifyPipe` 写帧与 listener 线程写 PONG 等帧并发，破坏字节流帧协议；原设计靠"listener 阻塞在 synchronize()"隔离，watchdog 独立于该链。裁决：移除 `_monoLog` 的 `sendFrame(notifyPipe,...)`，仅保留 `print()`（runScript 内已被重定向到面板，watchdog 路径落到 CE Lua 窗口），同步移除 bridge.notifyPipe 初始化/生命周期与文档/测试引用。代价：watchdog 触发的恢复日志不进 Notepad3 面板，只在 CE Lua 窗口可见。
- **Final: Ruling 看门狗模态框（审查 Issue 2，Important）** — 规格风险 1 建议"看门狗只探测不注入"，但看门狗的价值正是覆盖 CE 自带 AA 编辑器等非桥接路径丢失，只探测则失去意义。裁决：保留注入 + 退避（最多 3 次，之后停止）；CE 失败弹模态框属上游行为，本项目无法改为非模态。代价：极端情况下最多 3 次弹框会短暂阻塞桥接响应；在规格风险节记录该取舍。
- **Final: fixed not-wanted 清 ever（审查 Issue 4，re-grade Important）** — `not-wanted` 时 `setEver=false` 抹掉历史，用户重开后窄窗口内再丢失会落到 never-active 不恢复。裁决：wanted=false 只阻止恢复，不清 ever。代价：无实质代价（wanted 仍由 Checked 控制）。
- **Final: fixed monoEnsuring 卡死（审查 Issue 5，Important）** — 主体异常会让防重入标志永久 true。裁决：主体包 pcall，标志复位置于其后。代价：无。
- **Final: fixed target nil 判定（审查 Issue 7，Important）** — `menuVisible(...)~=false` 在属性读不到(nil)时视为 Mono 目标，可能误注入。裁决：改 `== true` 保守判定。代价：CE 版本读不到 Visible 时不自动恢复（符合"不误注入"意图）。
- **Final: Ruling 重复触发（审查 Issue 6，Minor）** — 保留 generateDefaultTemplate 自身的 `_monoEnsure`：checkScript 仅在 precheck 开启时调用，并非必达，移除会在关闭预检时漏检；双调用幂等且有防重入，代价可忽略。裁决不移除。代价：单次生成最多两次轻量检测。
- **Final: minor (deferred): 测试桩盲点（审查 Issue 3）** — 补 truthy-but-not-attached、synchronize 自排队、not-wanted 不清 ever、target nil 断言进测试（属修复验证的一部分）。
- **Final: minor (deferred): 测试硬编码 lua53-64.dll 路径（审查 Issue 9）** — 既有基础设施，非本次改动，推迟。
- 审查 Issue 8（重复输出）随 notifyPipe 移除自动解决。

最终验证：test\TestCeBridgeLua.ps1 → PASS；Build\scripts\Test-Docs.ps1 → 仅基线 2 error（无 .git）+ 计划页体量 WARN。所有修复经 TDD RED→GREEN。
Final review: 独立审查者（code-explorer，fresh context）执行；Critical 0，Important 4 项处理（3 修 1 裁决），Minor 记录/延后。
