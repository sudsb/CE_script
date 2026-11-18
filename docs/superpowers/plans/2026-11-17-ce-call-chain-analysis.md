# CE 调用链分析 与 保存自动入表 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 保存 CE 脚本时幂等地写入 CE 地址列表；并新增一个可开关的「调用链分析」功能，在断点命中时采集寄存器与堆栈，还原函数调用链并在 CE 侧三区窗体实时展示。

**Architecture:** 需求 A 走既有 `CMD_SYNC_SCRIPT` 协议：Lua 侧在记录缺失时幂等新建，C 侧解析回复把新记录 ID 写回 `.np3ce` sidecar。需求 B 全部在 CE Lua 侧（`ce_bridge/np3_ce_bridge.lua`）：两张内存表（寄存器快照表 + 堆栈表）由纯逻辑函数维护，断点回调采集原始值后立即 `co_run` 放行目标，符号解析与窗体刷新投递到 CE 主线程。

**Tech Stack:** CE 7.7 Lua（stock Lua 5.3）、Notepad3 C（MSVC）、PowerShell 测试 harness。

**Spec:** `docs/superpowers/specs/2026-11-17-ce-call-chain-analysis-design.md`

## Global Constraints

- 本工作区**没有 `.git` 目录**。不得执行 `git init` / `git commit`；每个任务以「checkpoint」步骤收尾（跑通对应测试并记录结果）。
- CE 侧功能一律实现在 `ce_bridge/np3_ce_bridge.lua`（C 侧无调试器 API）。`ce_edit/` 是原型，不改。
- 不新增 C 侧全局变量；复用 `s_ceSession` / `s_ceSessionMemrecId` 等既有静态量。
- `src/CeSession.c` 的纯函数不得依赖 Win32/Scintilla（`test/TestCeSession.ps1` 会把它们抽出来单独编译）。
- Lua 侧纯函数必须写成顶层 `local function NAME(...)`，函数体以行首 `end` 结束——`test/TestCeBridgeLua.ps1` 用正则 `(?ms)^local function NAME\(.*?^end\s*$` 抽取，缩进不一致会导致抽取失败。
- 不得取 CE 编辑锁（`beginEdit`/`endEdit` 已在 2026-11-16 移除）；记录一律按 `memrecId` 现场 `getMemoryRecordByID` 解析，不缓存记录指针。
- 桥接管道写帧必须串行，禁止新增并发写帧路径（会破坏字节流协议）。
- 修改 `ce_bridge/np3_ce_bridge.lua` 后必须重建 Notepad3（该文件以资源 `IDR_CE_BRIDGE_LUA` 嵌入），并重启 CE 才会加载新脚本。
- 测试命令：
  - `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
  - `pwsh -NoProfile -File test\TestCeSession.ps1`
  - `pwsh -NoProfile -File Build\scripts\Test-Docs.ps1`
  - 构建：`& $msbuild src\Notepad3.vcxproj /t:ClCompile /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal`

## Review Focus

以下输入/状态规格暗示但没有任何任务的测试覆盖，是最可能真正咬人的地方。每条在所属任务中补一条断言。

1. **用户保存时已在 CE 里删掉了那条记录** → 必须新建一条，而不是报错也不是静默失败（否则用户的编辑丢失）。
2. **`vtAutoAssembler` 不可用**（非 AA 上下文） → 必须返回 `ERR:` 而不是抛 Lua 异常打断 CE。
3. **sidecar 回填写坏了其它键**（`id=` / `description=` / `format=` 被覆盖或丢失） → 下次打开会话会丢失元数据。
4. **栈扫描读到不可读地址**（`readQword` 返回 nil） → 绝不能把异常抛进 CE 调试器，否则用户的游戏被永久卡在断点。
5. **命中次数超过保留上限** → 两张表必须是有界的，长时间运行不能让 CE 内存无限增长。
6. **关闭「调用链分析」后断点残留** → 用户的进程会被一个不再有人处理的断点永久暂停。

---

### Task 1: 需求 A —— Lua 侧幂等新建地址列表记录

**Files:**
- Modify: `ce_bridge/np3_ce_bridge.lua:1571-1593`（`syncSessionScript`）
- Modify: `test/TestCeBridgeLua.ps1:451-513`（`syncSessionScript` 抽取 + 行为断言）

**Interfaces:**
- Consumes: 既有 `bridge.sessions[sid]`（字段 `memrecId`、`description`）、`getAddressList()`、`vtAutoAssembler`、`al.getMemoryRecordByID(id)`、`al.createMemoryRecord()`。
- Produces: `syncSessionScript(payload: string) -> string`，返回 `'OK:<id>'`（成功，含新建）或 `'ERR:<reason>'`。新建时必须把 `session.memrecId` 更新为新记录 ID。

- [ ] **Step 1: 改写既有行为断言，使其描述新语义**

  `test/TestCeBridgeLua.ps1` 中现有断言把「新建记录」当错误，必须先改成新语义，否则测试无法表达需求。替换 `calls.created == 0` 那段（第 502 行）与「deleted record」那段（第 503-508 行）为：

  ```powershell
  assert(calls.created == 0, 'sync must not create a record when one exists')
  -- The user may delete the record in CE while it is open in Notepad3: the sync
  -- must re-create it instead of losing the edit.
  calls.deleted = true
  local gone = syncSessionScript('sid-1|4242\n[ENABLE]\nalloc(newmem,8)\n[DISABLE]\n')
  assert(gone == 'OK:4242', 'deleted record must be re-created: ' .. tostring(gone))
  assert(calls.created == 1, 'deleted record must be re-created exactly once')
  assert(calls.scriptSets == 2, 're-created record must receive the saved script')
  ```

  并在该 chunk 内追加「memrecId=0 时新建一次、连存两次不产生第二条」的断言：

  ```powershell
  local fresh = syncSessionScript('sid-2|0\n[ENABLE]\nalloc(newmem,8)\n[DISABLE]\n')
  assert(fresh == 'OK:4242', 'memrecId=0 must create a record: ' .. tostring(fresh))
  assert(calls.created == 2, 'memrecId=0 must create exactly one record')
  assert(bridge.sessions['sid-2'].memrecId == 4242, 'new id must be remembered in the session')
  ```

  同时在 `$syncChunk` 的 stub 中补上 `bridge.sessions['sid-2'] = { memrecId = 0 }`，并让 `vtAutoAssembler` 成为一个全局数字（`vtAutoAssembler = 1`）。

- [ ] **Step 2: 运行测试确认它失败**

  Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
  Expected: FAIL，消息为 `deleted record must be re-created: ERR:...`（当前实现对缺失记录返回 ERR）。

- [ ] **Step 3: 实现幂等新建**

  重写 `ce_bridge/np3_ce_bridge.lua` 的 `syncSessionScript`（替换第 1571-1593 行整个函数）：

  ```lua
  -- After a successful save in Notepad3, push the current buffer into the CE
  -- address list.  Idempotent: an existing record is updated in place, a
  -- missing one (never created yet, or deleted by the user in CE) is created.
  -- The record is resolved by its ID on every call, never cached: the user may
  -- delete the record in CE while it is open in Notepad3.
  local function syncSessionScript(payload)
    local head, script = string.match(payload or '', '^([^\n]*)\n(.*)$')
    if not head then return 'ERR:bad sync payload' end
    local sid, memid = string.match(head, '^([^|]*)|(%d*)$')
    if not sid or sid == '' then return 'ERR:bad session header' end
    local session = bridge.sessions[sid]
    if not session then return 'ERR:unknown session' end
    if type(vtAutoAssembler) ~= 'number' then return 'ERR:vtAutoAssembler unavailable' end

    local al = getAddressList()
    if not al then return 'ERR:no address list' end
    local idNum = tonumber(memid or '') or 0
    local mr = nil
    if idNum > 0 then
      local ok, found = pcall(function() return al.getMemoryRecordByID(idNum) end)
      if ok then mr = found end
    end
    if not mr then
      local ok, created = pcall(function() return al.createMemoryRecord() end)
      if not ok or not created then return 'ERR:createMemoryRecord failed' end
      mr = created
      local setOk = pcall(function()
        mr.Type = vtAutoAssembler
        mr.Description = (session and session.description) or 'Notepad3 AA Script'
      end)
      if not setOk then return 'ERR:configure memory record failed' end
    end
    local ok, err = pcall(function() mr.Script = script end)
    if not ok then return 'ERR:memrec.Script: ' .. tostring(err) end
    local rid = 0
    pcall(function() rid = mr.ID or 0 end)
    session.memrecId = rid
    return 'OK:' .. tostring(rid)
  end
  ```

  注意：`mr.Script = script` 必须在 `mr` 确认存在之后；`session.memrecId = rid` 必须无条件执行（含新建路径），否则 C 侧拿不到新 ID。

- [ ] **Step 4: 运行测试确认通过**

  Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
  Expected: PASS（脚本末尾输出 `PASS`）。若既有断言 `sync must not create a new memory record` 仍存在，说明 Step 1 没替换干净，回去替换。

- [ ] **Step 5: checkpoint**

  记录 `TestCeBridgeLua.ps1` 的 PASS 输出。不要执行 git 命令。

---

### Task 2: 需求 A —— C 侧把新记录 ID 写回 sidecar

**Files:**
- Modify: `src/CeSession.c`（新增 `_CeSession_ReplaceMetaMemrecId` 纯函数 + `_CeSession_RewriteMetaMemrecId` Win32 包装；改写 `_SyncThread`，当前在第 444-463 行）
- Modify: `test/TestCeSession.ps1`（抽取新纯函数 + 结构断言）
- Modify: `test/TestCeSession.cpp`（新纯函数的断言）

**Interfaces:**
- Consumes: `s_ceSession.metaPath`（`WCHAR[MAX_PATH]`，静态）、`_CeSession_ParseReply(reply, len, int* outMemrecId, char* err, size_t errCap)`（已存在于 `src/CeSession.c:146`）、`s_ceSessionMemrecId`（`volatile LONG`）。
- Produces:
  - `static bool _CeSession_ReplaceMetaMemrecId(const char* text, size_t len, int id, char* out, size_t cap)` — 纯函数：把 `text` 中的 `memrecId=` 行替换为 `memrecId=<id>`；不存在该行时在末尾追加 `memrecId=<id>\n`；其余行与顺序原样保留。容量不足返回 false。
  - `static bool _CeSession_RewriteMetaMemrecId(LPCWSTR metaPathW, int id)` — 读文件、调用纯函数、写回；失败返回 false。

- [ ] **Step 1: 写失败的 C++ 断言**

  在 `test/TestCeSession.cpp` 的 `main()` 中，第 118 行之后（紧跟 sidecar 路径用例）插入：

  ```cpp
  // 5b. Sidecar memrecId rewrite: only that key changes.
  {
      char out[256] = { 0 };
      const char text[] =
          "version=1\n"
          "id=s-42\n"
          "memrecId=0\n"
          "description=Code Injection\n"
          "format=1\n";
      CHECK(_CeSession_ReplaceMetaMemrecId(text, sizeof(text) - 1, 77, out, sizeof(out)));
      CHECK(std::strcmp(out,
          "version=1\n"
          "id=s-42\n"
          "memrecId=77\n"
          "description=Code Injection\n"
          "format=1\n") == 0);
      // No memrecId key yet: append it, keep everything else.
      const char none[] = "version=1\nid=s-9\n";
      CHECK(_CeSession_ReplaceMetaMemrecId(none, sizeof(none) - 1, 5, out, sizeof(out)));
      CHECK(std::strcmp(out, "version=1\nid=s-9\nmemrecId=5\n") == 0);
      // A too small buffer must fail instead of truncating.
      char tiny[8] = { 0 };
      CHECK(!_CeSession_ReplaceMetaMemrecId(text, sizeof(text) - 1, 77, tiny, sizeof(tiny)));
      // An existing non-zero id is replaced, not appended.
      const char have[] = "memrecId=3\nid=x\n";
      CHECK(_CeSession_ReplaceMetaMemrecId(have, sizeof(have) - 1, 9, out, sizeof(out)));
      CHECK(std::strcmp(out, "memrecId=9\nid=x\n") == 0);
  }
  ```

- [ ] **Step 2: 抽取新函数并确认测试失败**

  在 `test/TestCeSession.ps1` 的 `$parts += ...` 列表（第 294-303 行）追加一行：

  ```powershell
  $parts += Get-Function $session 'static bool _CeSession_ReplaceMetaMemrecId('
  ```

  Run: `pwsh -NoProfile -File test\TestCeSession.ps1`
  Expected: FAIL —— `Get-Function` 找不到 `static bool _CeSession_ReplaceMetaMemrecId(`，脚本抛 `function not found`。

- [ ] **Step 3: 实现纯函数**

  在 `src/CeSession.c` 中，紧跟 `_CeSession_MetaPathFromScript`（第 175-188 行）之后插入：

  ```c
  // Rewrite only the memrecId key of a session sidecar, keeping every other
  // line (and their order) byte-identical.  A sidecar without the key gets
  // one appended.  Pure: no Win32 calls, so the focused harness can use it.
  static bool _CeSession_ReplaceMetaMemrecId(const char* text, size_t len, int id,
                                               char* out, size_t cap)
  {
      if (!out || cap == 0) {
          return false;
      }
      out[0] = '\0';
      if (!text && len > 0) {
          return false;
      }
      char const key[] = "memrecId=";
      size_t const keyLen = (sizeof(key) - 1);
      size_t pos = 0;
      size_t written = 0;
      bool replaced = false;
      while (pos < len) {
          size_t eol = pos;
          while (eol < len && text[eol] != '\n') {
              eol++;
          }
          size_t lineLen = eol - pos;
          size_t next = (eol < len) ? (eol + 1) : len;
          if (!replaced && lineLen >= keyLen &&
              memcmp(text + pos, key, keyLen) == 0) {
              int const n = snprintf(out + written, cap - written, "%s%d\n", key, id);
              if (n < 0 || (size_t)n >= cap - written) {
                  return false;
              }
              written += (size_t)n;
              replaced = true;
          }
          else if (lineLen > 0) {
              if (written + lineLen + 2 > cap) {
                  return false;
              }
              memcpy(out + written, text + pos, lineLen);
              written += lineLen;
              out[written++] = '\n';
          }
          pos = next;
      }
      if (!replaced) {
          int const n = snprintf(out + written, cap - written, "%s%d\n", key, id);
          if (n < 0 || (size_t)n >= cap - written) {
              return false;
          }
          written += (size_t)n;
      }
      out[written] = '\0';
      return true;
  }
  ```

- [ ] **Step 4: 运行测试确认通过**

  Run: `pwsh -NoProfile -File test\TestCeSession.ps1`
  Expected: PASS，输出 `PASS: CE session helpers`（`TestCeSession.cpp` 打印 `PASS: CE session helpers`）。

- [ ] **Step 5: 实现 Win32 写回包装，并在 `_SyncThread` 里接上**

  在 `src/CeSession.c` 中 `_CeSession_ReplaceMetaMemrecId` 之后插入：

  ```c
  // Read the sidecar, replace its memrecId key and write it back.  The
  // sidecar is written only by the editor, so this stays the single writer.
  static bool _CeSession_RewriteMetaMemrecId(LPCWSTR metaPathW, int id)
  {
      if (!metaPathW || metaPathW[0] == L'\0' || id <= 0) {
          return false;
      }
      char text[8192] = { 0 };
      HANDLE const h = CreateFileW(metaPathW, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      if (h == INVALID_HANDLE_VALUE) {
          return false;
      }
      DWORD read = 0;
      BOOL const ok = ReadFile(h, text, (DWORD)(sizeof(text) - 1), &read, NULL);
      CloseHandle(h);
      if (!ok) {
          return false;
      }
      text[read] = '\0';
      char updated[8192] = { 0 };
      if (!_CeSession_ReplaceMetaMemrecId(text, (size_t)read, id, updated, sizeof(updated))) {
          return false;
      }
      HANDLE const w = CreateFileW(metaPathW, GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      if (w == INVALID_HANDLE_VALUE) {
          return false;
      }
      DWORD written = 0;
      BOOL const wrote = WriteFile(w, updated, (DWORD)strlen(updated), &written, NULL);
      CloseHandle(w);
      return wrote && written == (DWORD)strlen(updated);
  }
  ```

  然后改写 `_SyncThread`（当前第 444-463 行），把「丢弃回复」改为「解析并回填」：

  ```c
  static unsigned __stdcall _SyncThread(void* arg)
  {
      CE_SESSION_JOB* const job = (CE_SESSION_JOB*)arg;
      if (!job) {
          _endthreadex(0);
          return 0;
      }
      char* reply = NULL;
      DWORD replyLen = 0;
      CeBridge_SendRequest(CE_BRIDGE_CMD_SYNC_SCRIPT, job->payload, job->len,
                           &reply, &replyLen);
      // The reply carries the record id the bridge actually used.  A session
      // that had no record yet gets one on the first save, and the new id has
      // to reach the sidecar: the next save would otherwise ask for record 0
      // again and create a second record.
      int memrecId = 0;
      if (reply && _CeSession_ParseReply(reply, replyLen, &memrecId, NULL, 0)) {
          int const current = (int)InterlockedCompareExchange(&s_ceSessionMemrecId, 0, 0);
          if (memrecId > 0 && memrecId != current) {
              InterlockedExchange(&s_ceSessionMemrecId, (LONG)memrecId);
              if (!_CeSession_RewriteMetaMemrecId(s_ceSession.metaPath, memrecId)) {
                  _CeSession_Log("meta_rewrite_failed");
              }
          }
      }
      if (reply) {
          FreeMem(reply);
      }
      FreeMem(job->payload);
      FreeMem(job);
      _endthreadex(0);
      return 0;
  }
  ```

- [ ] **Step 6: 加结构断言并运行测试**

  在 `test/TestCeSession.ps1` 中 `_SyncThread` 的既有断言块（第 103-109 行附近）之后追加：

  ```powershell
  if (-not $sessionText.Contains('_CeSession_RewriteMetaMemrecId')) {
      Write-Host 'FAIL: the sidecar memrecId rewrite helper is missing'
      exit 1
  }
  if (-not $syncThreadFn.Value.Contains('_CeSession_ParseReply')) {
      Write-Host 'FAIL: the sync worker does not parse the record id from the reply'
      exit 1
  }
  if (-not $syncThreadFn.Value.Contains('_CeSession_RewriteMetaMemrecId')) {
      Write-Host 'FAIL: the sync worker does not write the record id back to the sidecar'
      exit 1
  }
  ```

  Run: `pwsh -NoProfile -File test\TestCeSession.ps1`
  Expected: PASS。

- [ ] **Step 7: 编译检查**

  Run: `& $msbuild src\Notepad3.vcxproj /t:ClCompile /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal`
  Expected: 无错误（`/WX` 级别的警告也算失败，需要消掉）。

- [ ] **Step 8: checkpoint**

  记录 `TestCeSession.ps1` PASS 与编译结果。不要执行 git 命令。

---

### Task 3: 调用链 —— 两张表与两个视图的纯逻辑（Lua）

**Files:**
- Modify: `ce_bridge/np3_ce_bridge.lua`（在 `np3.symbols`（第 209-211 行）之后插入 `chain` 模块的纯逻辑部分）
- Modify: `test/TestCeBridgeLua.ps1`（新增 `chain` 纯函数断言块）

**Interfaces:**
- Consumes: 无（纯函数，不调用任何 CE API）。
- Produces: 五个顶层 `local function`，全部只操作传入的 `state` 表：
  - `chainAppendRegs(state, snap) -> snap`：`snap.seq` 赋 `state.seq + 1` 并递增 `state.seq`，追加到 `state.regs`，调用 `chainEvict(state, 256)`，返回 snap。
  - `chainAppendStack(state, seq, slots) -> number`：给每个 slot 打上 `slot.seq = seq` 后追加到 `state.stack`，返回追加条数。
  - `chainEvict(state, maxHits) -> nil`：`#state.regs > maxHits` 时丢弃最旧的寄存器快照，并丢弃其 seq 对应的所有堆栈槽。
  - `chainBuildChain(state, seq) -> array`：返回该次命中的调用链节点数组，每项 `{ depth=, func=, retAddr=, slotAddr=, offset= }`。`depth = 0` 是当前函数（取自寄存器快照的 `at` 字段），其后是 `state.stack` 中 `seq` 匹配且 `isRet` 为真的槽，按 `depth` 升序。
  - `chainBuildStackView(state, seq) -> array`：返回该次命中的全部堆栈槽（`seq` 匹配），按 `offset` 升序，每项就是槽记录本身（含 `slotAddr/offset/value/symbol/isRet/depth`）。

  `state` 结构：`{ seq = 0, regs = {}, stack = {} }`。

- [ ] **Step 1: 写失败的 Lua 断言**

  在 `test/TestCeBridgeLua.ps1` 的 `$syncChunk` 断言块之后追加：

  ```powershell
  # --- Call-chain analysis: the two in-memory tables and the two views.
  $chainFns = @()
  foreach ($name in @('chainAppendRegs', 'chainAppendStack', 'chainEvict',
                       'chainBuildChain', 'chainBuildStackView')) {
      $m = [regex]::Match($text, ('(?ms)^local function ' + $name + '\(.*?^end\s*$'))
      if (-not $m.Success) { Write-Host ('FAIL: chain function not found: ' + $name); exit 1 }
      $chainFns += $m.Value
  }
  $chainChunk = ($chainFns -join "`n") + "`n" + @'
  local state = { seq = 0, regs = {}, stack = {} }
  local s1 = chainAppendRegs(state, { tid = 7, rip = 0x401000, rsp = 0x1000, rbp = 0x1100, at = 'game.exe+10' })
  assert(s1.seq == 1, 'first hit must get seq 1')
  assert(state.seq == 1, 'state.seq must advance')
  chainAppendStack(state, 1, {
    { slotAddr = 0x1000, offset = 0,  value = 0x1100, symbol = '', isRet = false, depth = nil },
    { slotAddr = 0x1008, offset = 8,  value = 0x402000, symbol = 'game.exe+2000', isRet = true, depth = 1 },
    { slotAddr = 0x1010, offset = 16, value = 0x403000, symbol = 'game.exe+3000', isRet = true, depth = 2 },
  })
  local chain = chainBuildChain(state, 1)
  assert(#chain == 3, 'chain must have the current function plus two frames: ' .. #chain)
  assert(chain[1].depth == 0 and chain[1].func == 'game.exe+10', 'depth 0 must be the hit function')
  assert(chain[2].depth == 1 and chain[2].func == 'game.exe+2000', 'frame 1 wrong')
  assert(chain[3].depth == 2 and chain[3].func == 'game.exe+3000', 'frame 2 wrong')
  assert(chain[2].retAddr == 0x402000, 'frame return address missing')
  local view = chainBuildStackView(state, 1)
  assert(#view == 3, 'stack view must show every slot: ' .. #view)
  assert(view[1].offset == 0 and view[3].offset == 16, 'stack view must be ordered by offset')
  -- A second hit must not disturb the first one's chain.
  local s2 = chainAppendRegs(state, { tid = 7, rip = 0x404000, rsp = 0x2000, rbp = 0x2100, at = 'other.exe+1' })
  assert(s2.seq == 2, 'second hit must get seq 2')
  assert(#chainBuildChain(state, 1) == 3, 'first chain must stay intact')
  assert(#chainBuildChain(state, 2) == 1, 'second chain has only the hit function')
  -- Eviction must drop the oldest snapshot together with its stack slots.
  for i = 1, 300 do
      local s = chainAppendRegs(state, { tid = 7, rip = 0x400000 + i, at = 'f' .. i })
      chainAppendStack(state, s.seq, { { slotAddr = 0x1000 + i, offset = 0, value = i } })
  end
  assert(#state.regs <= 256, 'register table must stay bounded: ' .. #state.regs)
  assert(chainBuildChain(state, 1)[1] == nil, 'evicted hit must be gone')
  assert(#state.stack <= 256 * 64, 'stack table must stay bounded: ' .. #state.stack)
  '@
  $chainRc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($chainChunk))
  if ($chainRc -ne 'OK') { Write-Host ('FAIL: call-chain table assertions: ' + $chainRc); exit 1 }
  ```

- [ ] **Step 2: 运行测试确认它失败**

  Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
  Expected: FAIL，消息 `FAIL: chain function not found: chainAppendRegs`。

- [ ] **Step 3: 实现纯逻辑函数**

  在 `ce_bridge/np3_ce_bridge.lua` 中 `np3.symbols` 函数之后（第 211 行之后）插入以下代码。注意每个函数必须以行首 `end` 结尾，且是顶层 `local function`：

  ```lua
  -- ---------------------------------------------------------------------------
  -- Call-chain analysis: the two in-memory tables and their two views.
  -- Pure logic only: no CE API is called here, so the focused harness can
  -- exercise it with plain Lua tables.
  -- ---------------------------------------------------------------------------

  -- Drop the oldest hits once the retention limit is exceeded.  A hit's stack
  -- slots are dropped together with its register snapshot, otherwise the
  -- stack table would grow without bound.
  local function chainEvict(state, maxHits)
    local limit = tonumber(maxHits) or 256
    while #state.regs > limit do
      local old = table.remove(state.regs, 1)
      local oldSeq = old and old.seq
      if oldSeq ~= nil then
        local kept = {}
        for _, slot in ipairs(state.stack) do
          if slot.seq ~= oldSeq then kept[#kept + 1] = slot end
        end
        state.stack = kept
      end
    end
  end

  -- Append one register snapshot; assigns and returns its sequence number.
  local function chainAppendRegs(state, snap)
    state.seq = (state.seq or 0) + 1
    snap.seq = state.seq
    state.regs[#state.regs + 1] = snap
    chainEvict(state, 256)
    return snap
  end

  -- Append the raw stack slots captured for one hit.
  local function chainAppendStack(state, seq, slots)
    local n = 0
    for _, slot in ipairs(slots or {}) do
      slot.seq = seq
      state.stack[#state.stack + 1] = slot
      n = n + 1
    end
    return n
  end

  -- The call chain of one hit: the hit function first, then the frames that
  -- were recognised as return addresses, innermost first.
  local function chainBuildChain(state, seq)
    local result = {}
    for _, snap in ipairs(state.regs) do
      if snap.seq == seq then
        result[#result + 1] = { depth = 0, func = snap.at, retAddr = snap.rip, slotAddr = nil, offset = nil }
        break
      end
    end
    local frames = {}
    for _, slot in ipairs(state.stack) do
      if slot.seq == seq and slot.isRet then
        frames[#frames + 1] = { depth = slot.depth or 0, func = slot.symbol, retAddr = slot.value,
                                 slotAddr = slot.slotAddr, offset = slot.offset }
      end
    end
    table.sort(frames, function(a, b) return (a.depth or 0) < (b.depth or 0) end)
    for _, f in ipairs(frames) do result[#result + 1] = f end
    return result
  end

  -- The raw stack window of one hit, ordered by offset.
  local function chainBuildStackView(state, seq)
    local result = {}
    for _, slot in ipairs(state.stack) do
      if slot.seq == seq then result[#result + 1] = slot end
    end
    table.sort(result, function(a, b) return (a.offset or 0) < (b.offset or 0) end)
    return result
  end
  ```

- [ ] **Step 4: 运行测试确认通过**

  Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
  Expected: PASS。

- [ ] **Step 5: checkpoint**

  记录 PASS 输出。不要执行 git 命令。

---

### Task 4: 调用链 —— 栈采集、帧回溯与断点钩子（Lua）

**Files:**
- Modify: `ce_bridge/np3_ce_bridge.lua`（`chain` 模块续：在 Task 3 插入的代码之后追加采集部分）
- Modify: `test/TestCeBridgeLua.ps1`（新增采集纯逻辑断言）

**Interfaces:**
- Consumes: CE API `readQword(addr)` / `readInteger(addr)` / `getNameFromAddress(addr)` / `inModule(addr)` / `getCurrentThreadID()` / `getTickCount()` / `debug_getContext(extraregs)` / `debug_setBreakpoint(addr, fn)` / `debug_removeBreakpointByID(id)` / `debug_continueFromBreakpoint(co_run)`；既有 `onMainThread(fn)`（本文件第 309-316 行）。
- Produces:
  - `chainScanStack(rsp, is64, maxBytes) -> slots` — 从 `rsp` 起按指针宽度逐槽读取原始值，返回槽数组（只含 `slotAddr/offset/value`，`symbol` 为空串）。读失败即停止。
  - `chainMarkReturns(slots, is64, rbp, maxFrames) -> slots` — 纯函数：沿帧指针链标记 `isRet`/`depth`（`depth` 从 1 起，0 是当前函数）。链断裂即停。
  - `chainMarkByScan(slots, maxFrames, isCodeAddr) -> slots` — 纯函数：`isCodeAddr(slot)` 为真的槽按出现顺序标记 `isRet`/`depth`（从 1 起）。
  - `chainResolveSlots(slots, maxResolve) -> slots` — 为最多 `maxResolve` 个槽调用 `getNameFromAddress` 填 `symbol`；优先已标记 `isRet` 的槽。
  - `chainOnHit()` — 断点回调：采集 → 放行 → 主线程解析并刷新。
  - `chainStart(address) -> ok, err` / `chainStop() -> ok` / `np3.chainStart()` / `np3.chainStop()` / `np3.chainDump()`。

  常量：`CHAIN_STACK_BYTES = 32768`、`CHAIN_MAX_FRAMES = 64`、`CHAIN_MAX_RESOLVE = 64`。

- [ ] **Step 1: 写失败的 Lua 断言（纯函数部分）**

  在 `test/TestCeBridgeLua.ps1` 的 chain 断言块之后追加：

  ```powershell
  # --- Call-chain analysis: stack scan and frame unwinding.
  $scanFns = @()
  foreach ($name in @('chainScanStack', 'chainMarkReturns', 'chainMarkByScan')) {
      $m = [regex]::Match($text, ('(?ms)^local function ' + $name + '\(.*?^end\s*$'))
      if (-not $m.Success) { Write-Host ('FAIL: chain scan function not found: ' + $name); exit 1 }
      $scanFns += $m.Value
  }
  $scanChunk = ($scanFns -join "`n") + "`n" + @'
  -- Fake target memory: three qwords at 0x1000.
  local mem = { [0x1000] = 0x1100, [0x1008] = 0x402000, [0x1010] = 0x403000 }
  readQword = function(a) return mem[a] end
  local slots = chainScanStack(0x1000, true, 64)
  assert(#slots == 3, 'scan must read three slots: ' .. #slots)
  assert(slots[1].slotAddr == 0x1000 and slots[1].offset == 0, 'first slot wrong')
  assert(slots[2].offset == 8 and slots[2].value == 0x402000, 'second slot wrong')
  assert(slots[3].offset == 16, 'third slot wrong')
  -- An unreadable address must stop the scan, not raise.
  local slots2 = chainScanStack(0x9000, true, 64)
  assert(#slots2 == 0, 'unreadable stack must yield no slots: ' .. #slots2)
  -- Frame-pointer unwinding: rbp=0x1100 -> [0x1100]=0x1200, [0x1108]=0x401000
  local fp = {
    { slotAddr = 0x1000, offset = 0,  value = 0x1100 },
    { slotAddr = 0x1008, offset = 8,  value = 0x402000 },
    { slotAddr = 0x1010, offset = 16, value = 0x403000 },
    { slotAddr = 0x1100, offset = 256, value = 0x1200 },
    { slotAddr = 0x1108, offset = 264, value = 0x401000 },
  }
  chainMarkReturns(fp, true, 0x1100, 8)
  local marked = 0
  for _, s in ipairs(fp) do if s.isRet then marked = marked + 1 end end
  assert(marked == 1, 'frame pointer chain must mark one return address: ' .. marked)
  assert(fp[5].isRet == true and fp[5].depth == 1, 'return address not marked at depth 1')
  -- A broken frame pointer chain must not raise and must mark nothing.
  local broken = { { slotAddr = 0x1000, offset = 0, value = 0x1100 } }
  chainMarkReturns(broken, true, 0x999999, 8)
  assert(broken[1].isRet == nil, 'a missing frame must not be marked')
  -- Scan fallback: only slots the predicate accepts are marked.
  local guess = { { slotAddr = 1, offset = 0, value = 0x10 }, { slotAddr = 2, offset = 8, value = 0x20 } }
  chainMarkByScan(guess, 4, function(s) return s.value == 0x20 end)
  assert(guess[1].isRet == nil, 'predicate rejected slot must not be marked')
  assert(guess[2].isRet == true and guess[2].depth == 1, 'accepted slot must be marked')
  '@
  $scanRc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($scanChunk))
  if ($scanRc -ne 'OK') { Write-Host ('FAIL: call-chain scan assertions: ' + $scanRc); exit 1 }
  ```

- [ ] **Step 2: 运行测试确认它失败**

  Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
  Expected: FAIL，消息 `FAIL: chain scan function not found: chainScanStack`。

- [ ] **Step 3: 实现扫描与回溯纯函数**

  在 `ce_bridge/np3_ce_bridge.lua` 的 `chainBuildStackView` 之后追加：

  ```lua
  -- Read the raw stack of the stopped target.  Only the values are captured
  -- here: symbol resolution happens later, on CE's main thread, because the
  -- Mono lookup is a pipe round trip that must not run while the target is
  -- paused.
  local function chainScanStack(rsp, is64, maxBytes)
    local slots = {}
    if type(rsp) ~= 'number' or rsp == 0 then return slots end
    local width = is64 and 8 or 4
    local limit = tonumber(maxBytes) or 32768
    local count = math.floor(limit / width)
    for i = 0, count - 1 do
      local addr = rsp + (i * width)
      local value = nil
      local ok, v = pcall(function()
        if is64 then return readQword(addr) end
        return readInteger(addr)
      end)
      if ok then value = v end
      if type(value) ~= 'number' then break end
      slots[#slots + 1] = { slotAddr = addr, offset = i * width, value = value, symbol = '' }
    end
    return slots
  end

  -- Walk the frame-pointer chain and mark the return addresses.  Pure: it
  -- only looks at the slots that were captured.
  local function chainMarkReturns(slots, is64, rbp, maxFrames)
    local width = is64 and 8 or 4
    local limit = tonumber(maxFrames) or 64
    if type(rbp) ~= 'number' or rbp == 0 then return slots end
    local byAddr = {}
    for _, s in ipairs(slots) do byAddr[s.slotAddr] = s end
    local cur = rbp
    local depth = 1
    while depth <= limit do
      local base = byAddr[cur]
      if not base then break end
      local ret = byAddr[cur + width]
      if not ret then break end
      ret.isRet = true
      ret.depth = depth
      depth = depth + 1
      local nextBase = base.value
      if type(nextBase) ~= 'number' or nextBase == 0 or nextBase <= cur then break end
      cur = nextBase
    end
    return slots
  end

  -- Fallback for targets compiled without a frame pointer: accept the slots
  -- the predicate recognises as code addresses.
  local function chainMarkByScan(slots, maxFrames, isCodeAddr)
    local limit = tonumber(maxFrames) or 64
    local depth = 1
    for _, s in ipairs(slots) do
      if depth > limit then break end
      if not s.isRet and isCodeAddr and isCodeAddr(s) then
        s.isRet = true
        s.depth = depth
        depth = depth + 1
      end
    end
    return slots
  end
  ```

- [ ] **Step 4: 运行测试确认通过**

  Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
  Expected: PASS。

- [ ] **Step 5: 实现解析、断点钩子与生命周期**

  在同一文件中继续追加（放在 `chainMarkByScan` 之后）。这些函数依赖 CE API，只能靠结构断言覆盖，不做行为单测：

  ```lua
  -- Resolve the captured addresses into names.  Bounded: a Mono lookup is a
  -- pipe round trip, so only a limited number of slots is resolved per hit.
  local function chainResolveSlots(slots, maxResolve)
    local limit = tonumber(maxResolve) or 64
    local resolved = 0
    for _, s in ipairs(slots) do
      if resolved >= limit then break end
      if s.symbol == nil or s.symbol == '' then
        local ok, name = pcall(function() return getNameFromAddress(s.value) end)
        if ok and type(name) == 'string' then
          s.symbol = name
          resolved = resolved + 1
        end
      end
    end
    return slots
  end

  local function chainIsCodeAddress(slot)
    if not slot or type(slot.value) ~= 'number' then return false end
    local ok, inside = pcall(function() return inModule(slot.value) end)
    if ok and inside then return true end
    -- A resolved name that is not a bare hex string came from a symbol
    -- handler (a Mono method name, for instance).
    local name = slot.symbol
    if type(name) == 'string' and name ~= '' then
      return not name:match('^%x+$')
    end
    return false
  end

  -- Breakpoint callback.  The target is stopped here, so the registers and the
  -- stack have to be read before it is continued.  is64/rbp are declared
  -- outside the pcall so the main-thread callback can still use them.
  local function chainOnHit()
    local state = bridge.chain
    local seq = 0
    local is64 = false
    local rbp = 0
    local ok, err = pcall(function()
      pcall(function() debug_getContext(false) end)
      is64 = (type(targetIs64Bit) == 'function' and targetIs64Bit()) or false
      local rip = is64 and RIP or EIP
      local rsp = is64 and RSP or ESP
      rbp = (is64 and RBP) or EBP
      if type(rip) ~= 'number' or rip == 0 then return end
      local snap = chainAppendRegs(state, {
        tid = getCurrentThreadID(),
        rip = rip, rsp = rsp, rbp = rbp,
        at = (getNameFromAddress(rip) or ''),
        tick = getTickCount(),
      })
      seq = snap.seq
      local slots = chainScanStack(rsp, is64, CHAIN_STACK_BYTES)
      chainAppendStack(state, seq, slots)
    end)
    if not ok then
      logEvent('chain error: ' .. tostring(err))
    end
    -- Always let the target run again, even when the capture failed.
    pcall(function() debug_continueFromBreakpoint(co_run) end)
    if seq > 0 then
      onMainThread(function()
        pcall(function()
          local slots = {}
          for _, s in ipairs(bridge.chain.stack) do
            if s.seq == seq then slots[#slots + 1] = s end
          end
          chainResolveSlots(slots, CHAIN_MAX_RESOLVE)
          chainMarkReturns(slots, is64, rbp, CHAIN_MAX_FRAMES)
          chainMarkByScan(slots, CHAIN_MAX_FRAMES, chainIsCodeAddress)
        end)
        pcall(chainRefreshForm)
      end)
    end
  end
  ```

  然后实现生命周期与手动入口（同样放在 `chainOnHit` 之后）：

  ```lua
  local function chainStop()
    local state = bridge.chain
    state.enabled = false
    if state.bpId then
      pcall(function() debug_removeBreakpointByID(state.bpId) end)
      state.bpId = nil
    end
    state.regs = {}
    state.stack = {}
    state.seq = 0
    if state.form then
      pcall(function() state.form.destroy() end)
      state.form = nil
    end
    return true
  end

  local function chainStart(address)
    local state = bridge.chain
    if state.enabled then return true end
    if type(address) ~= 'number' or address == 0 then
      return false, 'NP3: no address selected'
    end
    state.enabled = true
    state.regs = {}
    state.stack = {}
    state.seq = 0
    local ok, err = pcall(function()
      local _, id = debug_setBreakpoint(address, chainOnHit)
      state.bpId = id
    end)
    if not ok then
      state.enabled = false
      return false, 'NP3: ' .. tostring(err)
    end
    pcall(chainEnsureForm)
    return true
  end

  np3.chainStart = function() return chainStart(chainSelectedAddress()) end
  np3.chainStop = chainStop
  np3.chainDump = function()
    local lines = {}
    for _, node in ipairs(chainBuildChain(bridge.chain, bridge.chain.seq)) do
      lines[#lines + 1] = string.format('  %s%s', string.rep('  ', node.depth or 0), tostring(node.func))
    end
    return table.concat(lines, '\n')
  end
  ```

  并定义常量（放在 `chain` 模块顶部）：

  ```lua
  local CHAIN_STACK_BYTES = 32768
  local CHAIN_MAX_FRAMES = 64
  local CHAIN_MAX_RESOLVE = 64
  ```

  以及读取当前反汇编选中地址的辅助函数：

  ```lua
  local function chainSelectedAddress()
    local ok, addr = pcall(function()
      local mv = getMemoryViewForm()
      local dv = mv and mv.DisassemblerView
      return dv and dv.SelectedAddress or nil
    end)
    if ok and type(addr) == 'number' then return addr end
    return nil
  end
  ```

  **Task 4 结束时的状态**：`chainEnsureForm` 与 `chainRefreshForm` 尚未定义。`chainOnHit` 里对它们的引用写在 `pcall(chainRefreshForm)` 中——Lua 在函数**调用时**才解析全局名，所以这在 Task 4 结束时是合法的，且整文件语法检查（`TestCeBridgeLua.ps1` 的第一步）会通过。Task 5 负责真正定义它们。

- [ ] **Step 6: 加结构断言并运行测试**

  在 `test/TestCeBridgeLua.ps1` 的 chain 断言之后追加：

  ```powershell
  foreach ($needle in @('chainOnHit', 'chainStart', 'chainStop', 'chainResolveSlots',
                         'chainSelectedAddress', 'np3.chainStart', 'np3.chainStop',
                         'np3.chainDump', 'debug_continueFromBreakpoint', 'CHAIN_MAX_FRAMES')) {
      if (-not $text.Contains($needle)) {
          Write-Host ('FAIL: call-chain wiring missing: ' + $needle); exit 1
      }
  }
  # The breakpoint must always be removed again, or the user's process stays
  # paused on a breakpoint nobody handles any more.
  if (-not $text.Contains('debug_removeBreakpointByID(state.bpId)')) {
      Write-Host 'FAIL: stopping the analysis does not remove the breakpoint'
      exit 1
  }
  # The capture must never let an exception escape into CE's debugger.
  if (-not $text.Contains("logEvent('chain error: '")) {
      Write-Host 'FAIL: breakpoint errors are not trapped'
      exit 1
  }
  ```

  Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
  Expected: PASS。

- [ ] **Step 7: checkpoint**

  记录 PASS 输出。不要执行 git 命令。

---

### Task 5: 调用链 —— 菜单开关与三区窗体（Lua）

**Files:**
- Modify: `ce_bridge/np3_ce_bridge.lua`（`installMenu`（第 1410-1484 行）加菜单项；新增 `chainEnsureForm` / `chainRefreshForm` / `chainFormState`）
- Modify: `test/TestCeBridgeLua.ps1`（结构断言）

**Interfaces:**
- Consumes: CE API `createForm(visible)`、`createListView(owner)`、`form.Caption/Width/Height/Left/Top`、`lv.Columns.add()`（返回带 `.Text` 的 HeaderSection）、`lv.Items.add()`（返回带 `.SubItems` 的 ListItem）、`lv.clear()`、`lv.beginUpdate()/endUpdate()`、`lv.ItemIndex`、`createTimer`、`createMenuItem`、`item.Checked`。
- Produces:
  - `chainEnsureForm() -> form` — 创建/复用窗体，含三个 ListView：寄存器（左）、调用链（右）、堆栈信息（下）。
  - `chainRefreshForm() -> nil` — 按 `bridge.chain.selSeq` 刷新三个视图；未选中时用最新一次命中。
  - 菜单项 `Name = 'miNP3CallChain'`、Caption `调用链分析`，Checked 表示分析是否开启；实例挂在 `bridge.chainItem`。

- [ ] **Step 1: 写结构断言**

  在 `test/TestCeBridgeLua.ps1` 的 chain 断言之后追加：

  ```powershell
  foreach ($needle in @('miNP3CallChain', 'chainEnsureForm', 'chainRefreshForm',
                         'createListView', 'createForm', '调用链分析')) {
      if (-not $text.Contains($needle)) {
          Write-Host ('FAIL: call-chain UI missing: ' + $needle); exit 1
      }
  }
  # The menu item must carry its own checked state: CE's doClick() never
  # updates it.
  if (-not $text.Contains('chainItem.Checked = not chainItem.Checked')) {
      Write-Host 'FAIL: the call-chain menu item does not toggle its checked state'
      exit 1
  }
  # The three panes.
  if (-not $text.Contains('state.lvRegs') -or -not $text.Contains('state.lvChain') -or
      -not $text.Contains('state.lvStack')) {
      Write-Host 'FAIL: the three call-chain panes are missing'
      exit 1
  }
  # Stopping the analysis must clear the menu's checked state as well.
  if (-not $text.Contains('bridge.chainItem.Checked = false')) {
      Write-Host 'FAIL: stopping the analysis does not clear the menu check mark'
      exit 1
  }
  ```

- [ ] **Step 2: 运行测试确认它失败**

  Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
  Expected: FAIL，消息 `FAIL: call-chain UI missing: miNP3CallChain`。

- [ ] **Step 3: 实现窗体**

  在 `ce_bridge/np3_ce_bridge.lua` 中（放在 `chainOnHit` 之前，因为 `chainOnHit` 会调用 `chainRefreshForm`）追加：

  ```lua
  -- The call-chain window: registers on the left, the call chain on the
  -- right, the raw stack below.  All three panes show the same hit.
  local function chainEnsureForm()
    local state = bridge.chain
    if state.form then return state.form end
    local form = createForm(false)
    form.Caption = '调用链分析 - Notepad3'
    form.Width = 1100
    form.Height = 700
    form.Left = 40
    form.Top = 40
    form.DoNotSaveInTable = true

    local function makePane(parent, caption, left, top, width, height, columns)
      local lv = createListView(parent)
      lv.Parent = parent
      lv.Caption = caption
      lv.Left = left
      lv.Top = top
      lv.Width = width
      lv.Height = height
      lv.RowSelect = true
      for _, text in ipairs(columns) do
        local col = lv.Columns.add()
        col.Text = text
        col.Width = 120
      end
      return lv
    end

    state.lvRegs = makePane(form, '寄存器', 0, 0, 520, 330,
      { 'seq', 'tid', 'RIP', 'RSP', 'RBP', '函数' })
    state.lvChain = makePane(form, '调用链', 525, 0, 570, 330,
      { 'depth', '函数', '返回地址' })
    state.lvStack = makePane(form, '堆栈信息', 0, 335, 1095, 340,
      { '栈地址', '偏移', '值', '解析符号' })
    state.form = form
    form.show()

    -- CE's ListView exposes no selection event, so the selection is polled.
    if not state.timer then
      local timer = createTimer(nil, false)
      timer.Interval = 250
      timer.OnTimer = function()
        if not bridge.chain.form then return end
        local lv = bridge.chain.lvRegs
        if not lv then return end
        local idx = lv.ItemIndex
        if idx ~= bridge.chain.selSeq and idx >= 0 then
          bridge.chain.selSeq = idx
          chainRefreshForm()
        end
      end
      timer.Enabled = true
      state.timer = timer
    end
    return form
  end

  local function chainRefreshForm()
    local state = bridge.chain
    if not state.form or not state.lvRegs then return end
    local rows = {}
    for _, snap in ipairs(state.regs) do rows[#rows + 1] = snap end
    local sel = nil
    if state.selSeq and state.selSeq >= 0 and state.selSeq < #rows then
      sel = rows[state.selSeq + 1]
    else
      sel = rows[#rows]
    end
    if not sel then return end

    state.lvRegs.beginUpdate()
    state.lvRegs.Items.clear()
    for _, snap in ipairs(rows) do
      local item = state.lvRegs.Items.add()
      item.Caption = tostring(snap.seq)
      item.SubItems[0] = tostring(snap.tid or '')
      item.SubItems[1] = string.format('0x%X', snap.rip or 0)
      item.SubItems[2] = string.format('0x%X', snap.rsp or 0)
      item.SubItems[3] = string.format('0x%X', snap.rbp or 0)
      item.SubItems[4] = tostring(snap.at or '')
    end
    state.lvRegs.endUpdate()

    state.lvChain.beginUpdate()
    state.lvChain.Items.clear()
    for _, node in ipairs(chainBuildChain(state, sel.seq)) do
      local item = state.lvChain.Items.add()
      item.Caption = tostring(node.depth or 0)
      item.SubItems[0] = tostring(node.func or '')
      item.SubItems[1] = string.format('0x%X', node.retAddr or 0)
    end
    state.lvChain.endUpdate()

    state.lvStack.beginUpdate()
    state.lvStack.Items.clear()
    for _, slot in ipairs(chainBuildStackView(state, sel.seq)) do
      local item = state.lvStack.Items.add()
      item.Caption = string.format('0x%X', slot.slotAddr or 0)
      item.SubItems[0] = string.format('0x%X', slot.offset or 0)
      item.SubItems[1] = string.format('0x%X', slot.value or 0)
      item.SubItems[2] = tostring(slot.symbol or '')
    end
    state.lvStack.endUpdate()
  end
  ```

  把 Task 4 里的占位 `chainSelectedAddress` 保留（它是真实实现，不是占位），但删除 Task 4 步骤 5 末尾提到的、Task 5 之前不存在的占位 `chainEnsureForm` / `chainRefreshForm` 定义——Task 4 只在 `chainOnHit` 里以 `pcall(chainRefreshForm)` 引用它们，此时它们尚未定义是合法的（Lua 在函数体运行时才解析全局名），但**必须**保证 Task 4 结束时文件仍能通过语法检查（`TestCeBridgeLua.ps1` 第一步就是整文件语法检查）。

- [ ] **Step 4: 加菜单项**

  `installMenu`（`ce_bridge/np3_ce_bridge.lua:1410` 起）里，`pm` 是反汇编右键弹出菜单（`mv.debuggerpopup`，或 `mv.DisassemblerView.getPopupMenu()`），顶层项通过 `pm.Items` 管理：`createMenuItem(pm)` 建项，`items.insert(index + 1, root)` / `items.add(root)` 加入；子菜单项用 `root.add(defRoot)`。「调用链分析」是一个独立的开关，**不是**「自动汇编」的子项，所以它作为 `pm.Items` 的一个顶层项加入。

  在 `installMenu` 中 `items.add(root)` 那一段之后（仍在同一个 `pcall` 闭包内，`pm` 与 `items` 都在作用域里）追加：

  ```lua
    -- Standalone toggle: analyse the call chain at the selected address.
    local chainItem = createMenuItem(pm)
    chainItem.Name = 'miNP3CallChain'
    chainItem.Caption = '调用链分析'
    chainItem.Checked = bridge.chain.enabled == true
    chainItem.OnClick = function()
      -- CE's doClick() never updates the checked state, so do it here.
      chainItem.Checked = not chainItem.Checked
      if chainItem.Checked then
        local started, err = chainStart(chainSelectedAddress())
        if not started then
          chainItem.Checked = false
          reportError(err or 'NP3: cannot start the call-chain analysis')
        end
      else
        chainStop()
      end
    end
    items.add(chainItem)
    bridge.chainItem = chainItem
  ```

  `chainStop()` 也要把菜单项的勾选态清掉——在 `chainStop` 末尾加一行：

  ```lua
    if bridge.chainItem then
      pcall(function() bridge.chainItem.Checked = false end)
    end
  ```

  `bridge.chain` 状态表必须在文件顶部（`bridge.monoRecover` 附近）初始化为：

  ```lua
  bridge.chain = { enabled = false, bpId = nil, seq = 0, regs = {}, stack = {}, form = nil, item = nil, timer = nil }
  ```

  注意 `bridge.chain` 的初始化必须在 `installMenu` 运行之前完成（`installMenu` 会读 `bridge.chain.enabled`）。

- [ ] **Step 5: 运行测试确认通过**

  Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1`
  Expected: PASS。

- [ ] **Step 6: 编译检查**

  Run: `& $msbuild src\Notepad3.vcxproj /t:ClCompile /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal`
  Expected: 无错误。

- [ ] **Step 7: checkpoint**

  记录 PASS 输出与编译结果。不要执行 git 命令。

---

### Task 6: 文档状态更新与全量验证

**Files:**
- Modify: `docs/superpowers/specs/2026-11-17-ce-call-chain-analysis-design.md`（第 3 行状态行）
- Modify: `docs/README.md`（第 52 行索引项的状态描述）

**Interfaces:**
- Consumes: 无。
- Produces: 无（纯文档）。

- [ ] **Step 1: 更新设计文档状态**

  把设计文档第 3 行改为：

  ```
  日期：2026-11-17　状态：已实现（静态校验通过；运行时验证需桌面 + CE 7.7 + 调试中的目标进程）
  ```

  并在文档末尾追加一节，记录运行时验证**未执行**及原因（与仓库既有做法一致，不得声称已验证）：

  ```markdown
  ## 8. 验证状态

  - 静态校验：`test/TestCeBridgeLua.ps1`、`test/TestCeSession.ps1`、Release x64 编译、docs 校验器，均已通过。
  - 运行时验证（断点命中、Mono 方法名解析、窗体三区刷新、断点移除）**未执行**：需要桌面会话、CE 7.7 与一个处于调试状态的目标进程。
  ```

- [ ] **Step 2: 更新索引描述**

  `docs/README.md` 中该行改为：

  ```markdown
  | CE call-chain analysis + idempotent save-to-address-list design (implemented, runtime validation pending) | [CE call-chain analysis design](superpowers/specs/2026-11-17-ce-call-chain-analysis-design.md) |
  ```

- [ ] **Step 3: 跑全部测试**

  Run: `pwsh -NoProfile -File test\TestCeBridgeLua.ps1` → Expected: PASS
  Run: `pwsh -NoProfile -File test\TestCeSession.ps1` → Expected: PASS
  Run: `pwsh -NoProfile -File Build\scripts\Test-Docs.ps1` → Expected: 除既有的 2 个「requires the docs parent to be a Git root」错误外无新错误（该工作区无 `.git`，属已知基线）

- [ ] **Step 4: 完整构建**

  Run:
  ```powershell
  $msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
  & $msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
  ```
  Expected: 构建成功，无错误。

- [ ] **Step 5: 记录未验证项并交付**

  明确告诉用户：运行时验证（真实断点命中、Mono 方法名、三区窗体、断点移除）**未执行**，缺桌面 + CE 7.7 + 调试目标进程环境；需部署新桥接脚本（重建后重启 CE）后由用户实测。

---

## 自查记录

- **规格覆盖**：需求 A → Task 1 + Task 2；需求 B 的两张表 → Task 3；栈采集与回溯 → Task 4；菜单与三区窗体 → Task 5；测试与验证 → 各任务内步骤 + Task 6。规格第 4.5 节的三区布局与 Task 5 的三个 ListView 一一对应。
- **类型一致性**：`chainAppendRegs/chainAppendStack/chainEvict/chainBuildChain/chainBuildStackView` 在 Task 3 定义，Task 4/5 复用；`chainScanStack/chainMarkReturns/chainMarkByScan/chainResolveSlots` 在 Task 4 定义；`chainEnsureForm/chainRefreshForm/chainSelectedAddress` 在 Task 4/5 定义；C 侧 `_CeSession_ReplaceMetaMemrecId` / `_CeSession_RewriteMetaMemrecId` 在 Task 2 定义并被 `_SyncThread` 调用。
- **Review Focus 覆盖**：#1/#2 → Task 1 Step 1/3；#3 → Task 2 Step 1；#4 → Task 4 Step 1（`chainScanStack` 遇不可读地址返回空表）与 Step 5（`pcall` 包裹）；#5 → Task 3 Step 1（保留上限断言）；#6 → Task 4 Step 6（`debug_removeBreakpointByID` 断言）。
