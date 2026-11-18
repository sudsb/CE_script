# CE 调用链分析 与 保存自动入表：设计方案

日期：2026-11-17　状态：**需求 A 已实现；需求 B（调用链分析）已暂时禁用**——用户实测会卡死 CE，见第 10 节

## 1. 目标与已确认决策

两个独立需求，共用 CE 桥接脚本 `ce_bridge/np3_ce_bridge.lua`：

**需求 A — 保存自动入表（幂等）**：Notepad3 保存成功后，脚本自动进入 CE 地址列表；首次保存新建一条 AA 记录，之后保存更新同一条，不重复创建。

**需求 B — 调用链分析**：CE 侧新增一个可开关的功能，在断点命中时采集寄存器与调用栈，自动解析每层栈帧对应的函数（Mono 目标解析为方法全名），最终还原出函数调用链，并在 CE 专用列表窗体实时展示。只在用户开启时才采集，关闭即丢弃数据。

已与用户确认的决策：

| 决策点 | 选择 |
|---|---|
| 两张表的形态 | CE Lua 内存表，仅分析期间存在，关闭即丢弃 |
| 函数名（Mono） | 自动解析为 Mono 方法全名（`命名空间.类:方法`） |
| 采集触发 | 菜单开关手动启停（点菜单项开启下断点采集，再点关闭） |
| 保存行为 | 幂等：无记录则新建，有则更新同一条 |
| 结果出口 | CE 专用列表窗体，实时刷新寄存器表与栈帧/调用链 |

非目标：不改 CE 自带 `monoscript.lua`；不落盘、不跨会话保留；不做符号下载；不替代 CE 自带的调用栈窗口；不做单步/分支记录（Last Branch Recording）。

## 2. 可行性证据（已查证，非推测）

采集 API（`E:\Cheat Engine\Cheat Engine 7.7\celua.txt`）：

- 第 861-870 行：`debugger_onBreakpoint()` 回调触发时，CE 自动填充全局寄存器变量 `EAX..EDI/ESP/EIP`，64 位还有 `RAX..R15/RSP/RIP`。断点回调是采集入口，寄存器不需要额外 API 去取。
- 第 889-895 行：`debug_setBreakpoint(address, size, trigger, functiontocall)`、`debug_removeBreakpointByID(id)`、`debug_continueFromBreakpoint(method)`。可在钩子内自行 continue（`co_run`），做到对用户无感。
- 第 833 行：`getNameFromAddress(address)` 把地址解析为「注册符号 / 模块+偏移 / 十六进制」；第 209 行 `getRTTIClassName`、第 834 行 `inModule` 可辅助判断。

Mono 名称解析（`E:\Cheat Engine\Cheat Engine 7.7\autorun\monoscript.lua`）：

- 第 1332 行：CE 已注册 `registerAddressLookupCallback(mono_addressLookupCallback)`。
- 第 1886-1978 行：`mono_addressLookupCallback` 用 `mono_getJitInfo` + `mono_class_getName` + `mono_method_getName` 拼出 `namespace.class:method`（嵌套类用 `mono_class_getFullName`），并缓存到 `libmono.jitcache`。**因此 `getNameFromAddress(addr)` 对已 JIT 的 Mono 方法直接返回方法全名**，无需自己拼装。
- 第 3630-3663 行：`mono_getJitInfo(address)` → `{method, code_start, code_size}`，可作为 `getNameFromAddress` 返回纯十六进制时的兜底（自己拼 `mono_method_getFullName`，第 4318 行）。
- 第 131、7209 行：`mono_skipsafetycheck` 默认 `true`（DotNet 设置默认 `SkipSafetyCheck=1`）。第 1932 行的 `isPaused()` 拦截只在用户手动关掉该设置时才生效——即断点暂停时默认仍能解析 Mono 名。若用户关掉该设置，解析降级为模块+偏移/十六进制，功能仍可用但名称不友好，文档与 UI 需提示。

栈读取：`readQword/readPointer`（`本地文档.txt:661-688`）可读栈内存；断点暂停时目标已挂起，读栈稳定。线程 ID：`getCurrentThreadID()`（CE 全局，`monoscript.lua` 多处使用）。

窗体与线程（`celua.txt`）：第 1311 行 `createForm(visible)`、第 1762 行 `createListView(owner)`（ListView 支持 report 模式）；第 560-562 行 `synchronize/queue/checkSynchronize`。**断点回调运行在调试器线程，不能直接碰 UI**，必须用桥接脚本已有的 `onMainThread()`（`np3_ce_bridge.lua:309-316`，内部 `synchronize`）投递 UI 刷新。`checkSynchronize` 的 25ms 定时器（第 1823-1829 行）保证投递被执行。

保存链路现状：`FileSave` 成功后仅当 `CeSession_IsActive()` 调 `CeSession_SyncSavedScript()`（`Notepad3.c:13748`），走 `CMD_SYNC_SCRIPT`（=6，`CeBridge.h:23`）；Lua 侧 `syncSessionScript()`（`np3_ce_bridge.lua:1571`）按 `memrecId` 找**已有**记录，找不到不会新建。新建记录在 `applyScript()`（第 1527，`CMD_APPLY_SCRIPT`=3）。协议为 `if/elseif` 链（第 1749+），加命令成本很低。

## 3. 需求 A：保存自动入表（幂等）

### 3.1 方案

把"是否已有记录"的判定放到 Lua 侧 `syncSessionScript()`，C 侧 FileSave 触发点保持不变（仍发 `CMD_SYNC_SCRIPT`），改动集中在单一汇聚点。

```
syncSessionScript(payload):
  head, script = parse(payload)            -- sessionId|memrecId\n<script>
  al = getAddressList()
  mr = (memrecId>0) and al.getMemoryRecordByID(memrecId) or nil
  if mr == nil then
      -- 幂等新建：仅当确实找不到/无 id 时
      mr = al.createMemoryRecord()
      mr.Type = vtAutoAssembler
      mr.Description = session and session.description or 'Notepad3 AA Script'
      mr.Script = script
  else
      mr.Script = script                    -- 更新同一条
  end
  session.memrecId = mr.ID
  return 'OK:'..tostring(mr.ID)
```

### 3.2 不变量（来自既往教训，必须保持）

- 不取 CE 编辑锁（不改 `beginEdit/endEdit`，2026-11-16 已取消）。
- 每次按 `memrecId` 现场 `getMemoryRecordByID`，不缓存记录指针。
- 保持异步静默：不影响已成功的本地保存结果，不弹消息框。
- 新建分支与 `applyScript()` 行为一致（`vtAutoAssembler`、`Description`、`Script`），两条路径产出的记录无差别。

### 3.3 ID 回填（决定：C 侧写回 sidecar）

首次从反汇编生成的脚本 sidecar 里 `memrecId=0`（`writeMeta` 写死 0，见 `np3_ce_bridge.lua:784`），若只改 Lua 侧的 `session.memrecId`，sidecar 与 C 侧原子量不变，第二次保存仍发 `memrecId=0` → 又新建一条。**因此回填必须落到 `.np3ce` sidecar。**

决定采用 C 侧写回（职责与文件写入者一致，不引入新协议命令）：

- `syncSessionScript` 本就返回 `OK:<id>`（`np3_ce_bridge.lua:1592`）；现有 `_SyncThread` 只丢弃回复（`CeSession.c:451-458`）。改为在 `_SyncThread` 内用已有 `_CeSession_ParseReply()`（`:146`）解析出新 ID。
- 解析成功且 ID 变化时：`InterlockedExchange(&s_ceSessionMemrecId, id)`，并重写 sidecar 的 `memrecId=` 行。
- `CeSession.c` 目前只有 meta 读取路径（`:175-260`），没有写回 helper。新增 `_CeSession_RewriteMetaMemrecId(metaPath, id)`：读取现有 sidecar 文本，逐行替换 `memrecId=`，其余行原样保留，写回同一路径。实现走现有 Win32 文件惯用法即可，失败只记 `_CeSession_Log`，不改变已成功的保存结果。
- 该写回在 worker 线程做，不阻塞 UI；与原 `apply` 成功后更新 `s_ceSessionMemrecId`（`:338`）的语义一致。
- 不采用「Lua 直接改 sidecar」：sidecar 由 Notepad3 独占写入，Lua 侧改写会形成双写者、与重开时的读取竞争。

### 3.4 其他边界

- 仅对通过 CE 桥接打开/生成的**会话文档**生效（`CeSession_IsActive()` 为真，沿用现有 `Notepad3.c:13748` 门控）；普通本地文档保存行为不变。
- 旧记录被用户在 CE 里删除：`getMemoryRecordByID` 返回 nil → 新建，符合幂等语义。
- `vtAutoAssembler` 不可用：返回 `ERR:`，与现有 `applyScript` 的错误处理一致。

## 4. 需求 B：调用链分析

### 4.1 总体结构（全部在 CE Lua 侧，文件 `ce_bridge/np3_ce_bridge.lua`）

```
菜单项「调用链分析」(Checked 切换)
   ├─ 开启 → chain.start():  建两表 + 建窗体 + 下断点
   │           断点回调 chain.onHit():
   │             ① 读寄存器快照 → 表1
   │             ② 回溯调用栈   → 表2
   │             ③ onMainThread(刷新窗体)
   │             ④ debug_continueFromBreakpoint(co_run)  -- 不打断用户
   └─ 关闭 → chain.stop():   移除断点 + 清空两表 + 销毁窗体
```

新增函数前缀统一 `chain`，状态集中在 `bridge.chain = { enabled, bpIds, regs, stack, form, ... }`，强引用挂在 `bridge` 上防止 GC。

### 4.2 两张内存表（用户确认：内存表、关闭即丢弃）

**表 1 — 寄存器快照表 `bridge.chain.regs`**：一次命中一条快照。

| 字段 | 来源 |
|---|---|
| `seq` | 递增计数 |
| `tid` | `getCurrentThreadID()` |
| `rip` / `rsp` / `rbp` | 全局 `RIP/RSP/RBP`（32 位取 `EIP/ESP/EBP`） |
| `rax..r15` | 全局寄存器变量（按目标位数取舍） |
| `at` | `getNameFromAddress(rip)`——当前命中所在函数 |
| `tick` | `getTickCount()` |

**表 2 — 堆栈信息表 `bridge.chain.stack`**：一次命中保存从 `RSP` 起的一段原始栈扫描结果（不是只存识别出的帧），通过 `seq` 关联表 1。这张表同时服务右侧「调用链」与下方「堆栈信息」两个视图。

| 字段 | 来源 |
|---|---|
| `seq` | 外键 → 表 1 |
| `slotAddr` | 栈槽地址 |
| `offset` | 相对 `RSP` 的偏移 |
| `value` | 该槽的值（`readQword/readInteger`） |
| `symbol` | `getNameFromAddress(value)` → 原生符号/模块+偏移/Mono 方法全名（解析失败为空） |
| `isRet` | 是否被判定为返回地址（回溯算法标记） |
| `depth` | `isRet` 时的调用链层级（0 = 当前函数，逐层递增）；非返回地址为 nil |

两个视图都由这张表投影：

- **调用链（右）** = 按 `seq` 过滤、`isRet==true` 的项，按 `depth` 排序 → 节点显示 `symbol`（Mono 方法全名），附 `value`/模块偏移。
- **堆栈信息（下）** = 按 `seq` 过滤的**全部**槽，按 `offset` 排序 → 列 `slotAddr / offset / value / symbol`，`isRet` 行高亮，便于对照返回地址落在哪个栈槽。

上限：单次最多扫描固定字节数（如 64KB，记作栈快照窗口）、识别最多 64 层调用、最多保留最近 256 次命中（`seq` 环形淘汰），防止长时间运行内存膨胀——具体上限交由实现者定，但必须有界。

### 4.3 栈回溯算法（调用链的核心）

采用**帧指针链 + 栈扫描**双策略，按目标位数自动切换：

1. 优先帧指针链：x64 从 `RBP`、x86 从 `EBP` 出发。帧基址指向的栈布局为 `[base]` = 上一帧基址，`[base+ptr]` = 返回地址。用 `readQword/readInteger` 读取，逐层向上，直到读失败或越界。
2. 若帧指针不可用（优化编译常省略帧指针）：从 `RSP/ESP` 起向上线性扫描栈，步长 = 指针宽度，对每个值 `v` 用 `getNameFromAddress(v)` 试探；只接受「`inModule(v)` 为真」且解析结果不是纯十六进制的值作为返回地址，避免把普通数据当函数。
3. 扫描到的每个栈槽都解析 `symbol`；被判定为返回地址的槽标记 `isRet=true` 并赋予 `depth`（Mono 目标经第 2 节所述回调自动得到方法全名）。
4. 终止条件：帧数上限、读内存失败、地址远离当前栈范围。全程 `pcall` 包裹，任何异常不得冒泡到 CE 调试器（否则会打断用户调试）。

实现要点：栈扫描的误判率取决于启发式，属于可接受的近似（设计上标注为"尽力还原"而非精确 unwind）。若后续 CE 提供官方栈回溯 API，可替换本策略而不改表结构与 UI。

### 4.4 断点回调对用户的无感性

- 回调内先 `debug_continueFromBreakpoint(co_run)` 让目标尽快恢复，再做耗时解析（但寄存器/栈必须在 continue 前读完，因为上下文会变）——权衡见第 6 节。
- 不在回调里调用会弹模态框的 API；不调用 `LaunchMonoDataCollector`（Mono 丢失由既有看门狗负责）。
- 每次命中只在内存表追加，不写盘、不进桥接协议帧（避免与 listener 的帧写入并发破坏字节流，沿用 Mono 设计的教训）。

### 4.5 窗体（CE 侧，用户确认的三区布局）

- 一个 `createForm(false)` 窗体，标题「调用链分析 - Notepad3」，尺寸约 1100x700。
- **左侧 — 寄存器表**（ListView，report 模式）：寄存器快照表，列 `seq / tid / RIP / RSP / RBP / RAX..R15（按目标位数） / 命中函数`。列出每次命中的一条快照；选中一条驱动右侧与下方。
- **右侧 — 调用链表**：选中快照对应的函数调用链，按 `depth` 由内到外（0 → N）呈现，每个节点显示 `func`（Mono 为方法全名），附返回地址/模块偏移。用 ListView 列表或 TreeView 树形缩进展示层级；树形更直观，若 CE Lua 的 TreeView 节点 API 不便则用 ListView 的 `depth` 列 + 缩进前缀替代（实现者定）。
- **下方 — 堆栈信息**：选中快照对应的原始栈内存窗口，列 `栈地址 / 偏移(相对 RSP) / 值 / 解析符号`，逐行显示从 `RSP` 起的一段栈内容，便于对照调用链的返回地址来自哪个栈槽。可与右侧调用链中 `retAddr` 所在行联动定位。
- 三个区的数据都来自同一次 `seq`（选中左侧一行 → 右、下按 `seq` 过滤刷新）；未选中时默认显示最新一次命中。
- 所有控件访问必须走 `onMainThread()`（桥接已有封装）。表数据在调试器线程算好后，仅把"渲染"投递到主线程。
- 生命周期：随开关创建/销毁（`form.free()`），随 `bridge.running=false` 兜底清理；重复开启不重复建窗体。

### 4.6 与现有代码的集成点

- 菜单：复用 `installMenu()`（`np3_ce_bridge.lua:1410+`）挂到反汇编右键 `debuggerpopup`；新增一个 Checked 项，`OnClick` 用 `item.Checked = not item.Checked`（记忆实证：`doClick` 不改勾选态，必须自己写）。
- 调试器钩子：优先用 `debug_setBreakpoint` 的 `functiontocall` 参数挂回调，避免覆盖全局 `debugger_onBreakpoint`（CE 全局函数名唯一，覆盖会破坏其他脚本）。若该参数的上下文拿不到寄存器，则回退到定义 `debugger_onBreakpoint` 并保存/转发原函数——此回退是否必要见第 6 节。
- 断点地址来源：用户当前选中地址。
- 手动入口：`np3.chainStart()/chainStop()/chainDump()`，便于测试与无 UI 场景。

## 5. 测试策略

沿用现有 harness 模式（`test/TestCeBridgeLua.ps1` 用 P/Invoke 加载 `lua53-64.dll` 跑抽取出的纯函数断言）：

- 从 Lua 源文件抽取纯逻辑函数（栈扫描→表 2 归类、`chain.buildChain`（按 `isRet`/`depth` 投影调用链）、`chain.formatChain`、表上限淘汰逻辑）做单元测试，断言表结构、两个视图的投影/排序、淘汰、Mono 名格式化。
- `syncSessionScript` 增加幂等断言：memrecId=0 时新建一次且回填；memrecId 有效时只更新不新建；记录被删（查不到）时新建一次；连测两次保存不产生两条记录。
- `_CeSession_RewriteMetaMemrecId` 抽为纯函数做断言（可放入 `TestCeSession.ps1` 的 C++ harness）：给定 sidecar 文本 + 新 ID，只改 `memrecId=` 行、其余行与换行顺序不变；无效输入不写回。同时断言 `_SyncThread` 源码含解析回复与写回调用。
- 源码结构断言：菜单项、命令常量、三个 `np3.*` 入口存在。
- 运行时验证（需桌面 + CE 7.7 + 调试中的目标进程，本机具备 CE 7.7 但需具体进程）：开启开关 → 命中断点 → 窗体两表有数据、Mono 方法名为 `命名空间.类:方法`、目标不停顿；关闭 → 断点移除、表清空、窗体销毁。**此项无法在单人无目标进程时完成，须如实标注为未运行。**

需求 A 的 ID 回填在 C 侧现有 `CMD_SYNC_SCRIPT` 回复中完成，不新增协议命令；但需扩展 `test/TestCeSession.ps1` 断言写回逻辑。

## 6. 已解决 / 风险（builder 审查后）

已解决：

- ~~ID 回填路径~~：第 3.3 节已定为 C 侧在 `_SyncThread` 解析 `OK:<id>` 并写回 sidecar；Lua 单方面改自己的表无效。不新增协议命令。

实施时需确认/风险（不改变本设计的结构，由实现者决策或在实现中验证）：

1. **`functiontocall` 回调是否填充寄存器全局**：CE 文档只在 `debugger_onBreakpoint()` 下明说填充全局寄存器（第 868-870 行）。若 `functiontocall` 回调不填充，需回退到全局 `debugger_onBreakpoint` 钩子并转发原函数。实现前查 CE 源码（`LuaHandler.pas`/debugger 单元）或实测确认；两种路径都产出相同的表结构，不影响 UI/测试设计。
2. **栈扫描误判**：启发式可能把数据当返回地址；接受为近似方案，界面可标注"尽力还原"。
3. **Mono 安全设置关闭时**：`getNameFromAddress` 在暂停时返回 nil/纯十六进制，名称降级；功能仍可用。是否在开启分析时提示用户该设置，由实现者判断。
4. **断点回调性能**：解析函数名走 CE 符号处理器可能较慢，但目标已暂停，不影响；需确保回调最终一定 continue，绝不把用户卡在断点。
5. **线程**：所有 CE 对象读取（地址列表、窗体）在主线程；断点回调只操作 Lua table 与只读内存 API，UI 变更投递主线程。与既有 `onMainThread` 规则一致。

## 7. 交付顺序（供实施计划参考）

1. 需求 A（改动小、风险低、可完整测试）单独先行。
2. 需求 B 的纯逻辑（两表结构、回溯、格式化）与测试。
3. 需求 B 的菜单开关、断点回调、窗体。
4. 运行时验证（需真实目标进程）。

每个阶段结束跑 `test/TestCeBridgeLua.ps1`；需求 A 另跑 `test/TestCeSession.ps1`；完成后跑 Release x64 构建。文档变更后跑 `pwsh -NoProfile -File Build\scripts\Test-Docs.ps1`。

## 8. 实施中的修正（与本文上文的差异）

实施按 `docs/superpowers/plans/2026-11-17-ce-call-chain-analysis.md` 执行，落地时修正了三处：

1. **ID 回填位置**：第 3.3 节已定（Lua 单方面改表无效）。
2. **名称解析预算**：`chainResolveSlots` 先解析被标记为返回地址的槽，再解析其余槽。原设计未指定顺序，若按栈序解析，深处的调用帧会因预算耗尽而拿不到名字。
3. **列表选中项**：刷新寄存器列表会清空选中行（`ItemIndex` 归 -1），需在重建后写回 `ItemIndex`，否则用户选中的快照每次命中都丢失高亮。

另外 `onMainThread` 改为「先声明后赋值」（`local onMainThread` + `function onMainThread`）：它在文件中定义于调用链模块之后，Lua 的 `local function` 只对其下方可见，不改则调用链采集会取到全局 `nil`。

运行时修正（2026-11-17，用户实测报「list 为空」）：

4. **ListView 必须是报表视图且子项只能追加**：CE 的 ListView 默认是图标视图，此时**没有 SubItems**；必须设 `lv.ViewStyle = 'vsReport'`（CE 自带的 `dotnetinfo.lua`/`java.lua`/`patchscan.lua` 全部如此）。新建条目的 `SubItems` 是**空列表**（`dotnetinfo.lua:2112` 用 `SubItems.Count<3` 守卫、`结构工具.lua:83` 读 `SubItems[0..Count-1]` 可证），所以列值必须用 `item.SubItems.add(v)` 追加，按下标 `SubItems[0] = v` 会越界并中断整次刷新。列标题用 `col.Caption`（celua.txt 写的是 `Text`，但 CE 自己的脚本一律用 `Caption`）。第 4.5 节的三区表格本身不变。

## 9. 验证状态

- 静态校验：`test/TestCeBridgeLua.ps1`、`test/TestCeSession.ps1`、Release x64 完整构建、docs 校验器，均已通过。
- 运行时验证（断点命中、Mono 方法全名解析、窗体三区刷新、断点移除）**未执行**：需要桌面会话、CE 7.7 与一个处于调试状态的目标进程。本机具备 CE 7.7（`E:\Cheat Engine\Cheat Engine 7.7`），但无具备该条件的测试目标。
- 部署注意：`ce_bridge/np3_ce_bridge.lua` 以资源 `IDR_CE_BRIDGE_LUA` 嵌入，需重建 Notepad3 并重启 CE 才会生效。

## 10. 调用链分析已暂时禁用（2026-11-17 用户实测）

用户实测：开启调用链分析会导致 **CE 程序卡死**。按要求先屏蔽，需求 A（保存自动入表）保持启用。

屏蔽方式（源码保留，便于日后恢复）：
- `ce_bridge/np3_ce_bridge.lua` 顶部 `local CHAIN_ENABLED = false`。
- `chainStart()` 首行 `if not CHAIN_ENABLED then return false, 'NP3: 调用链分析已暂时禁用（会导致 CE 卡死，待排查）' end`。
- `installMenu()` 中菜单项创建包在 `if CHAIN_ENABLED then ... end` 里——入口不可见，无法从 UI 触发。
- 脚本加载时**无条件**清理上一版本遗留的断点（`if bridge.chain and bridge.chain.bpId then debug_removeBreakpointByID ... end`），避免旧断点继续暂停目标。
- `test/TestCeBridgeLua.ps1` 新增断言：禁用时 `chainStart` 必须拒绝启动、不得调用 `debug_setBreakpoint`、不得改动会话状态。

**卡死的最可能原因（待验证，不是已确认结论）**：断点回调运行在 CE 调试器线程，回调里做了两类危险操作——① 经 `onMainThread()` → `synchronize()` 把刷新投递到 CE 主线程并**等待其完成**；② 在解析阶段调用 `getNameFromAddress` / `inModule`，Mono 目标会走 `mono_getJitInfo` 的管道往返。调试器线程等待主线程、主线程又可能在处理断点而等待调试器，构成死锁；Mono 管道往返会放大该风险。

后续若恢复本功能，必须先解决线程模型（例如完全不在断点回调里 `synchronize`，改为只写表、由主线程定时器自行拉取并刷新；解析也移到主线程），并在带真实目标进程的环境复测后再把 `CHAIN_ENABLED` 改回 `true`。
