-- encoding: UTF-8
-- Notepad3 <-> Cheat Engine bridge (Cheat Engine 7.x / Lua 5.3).
--
-- Deploy to <Cheat Engine>\autorun\np3_ce_bridge.lua and (re)start Cheat Engine.
-- Notepad3 deploys this file automatically; it can also be copied by hand.
--
-- Wire protocol on the named pipe "NP3_CE_BRIDGE" (byte stream, little endian):
--   request  : [cmd:1][len:uint32][payload:len]
--   response : [kind:1][len:uint32][payload:len] ... terminated by DONE
--   cmd      : 0 = PING, 1 = RUN (payload = Lua source), 2 = CLOSE,
--              3 = APPLY_SCRIPT (sessionId|memrecId\n<script UTF-8>),
--              4 = CLOSE_SESSION (sessionId),
--              5 = CHECK_SCRIPT (<script UTF-8> -> CE syntax findings)
--              6 = SYNC_SCRIPT (sessionId|memrecId\n<script UTF-8>): persist a
--                  saved editor buffer into the existing address-list record,
--                  which is looked up by its record ID on every call.
--   kind     : 0 = OUTPUT (print), 1 = RESULT, 2 = ERROR, 3 = DONE, 4 = INFO
--   DONE payload is "0" on success, non-zero on failure.
-- All payloads are UTF-8 text.
--
-- RUN executes inside CE's own Lua state, so the script has full access to the
-- CE API (readInteger/writeBytes/autoAssemble/debug_setBreakpoint/
-- createDisassembler/...), the currently opened process and its debugger.
--
-- CE-side editor integration:
--   * "Auto Assemble" (自动汇编) submenu on the disassembler right-click menu,
--     mirroring CE's own Template menu items (no template text is replicated).
--     The mirrored option rows really switch the JMP mode: CE's Lua doClick()
--     only fires OnClick and never updates the item's checked state, so the
--     state is applied by hand.  The rows follow CE's own visibility rules
--     (the 5/14-byte JMP needs a 64-bit x86 target, the 1-byte exception JMP
--     an x86 target) and a failing action is reported in CE's own UI.
--   * A hidden TfrmAutoInject is used as a template factory: the CE-native
--     OnClick handlers generate the exact built-in templates, including the
--     5/14-byte and 1-byte-exception JMP options and CE 7.7's runtime hook.
--   * Generated scripts are written to a session file and opened in Notepad3.
--     Their sidecar carries format=1, so the editor runs its CE formatter once
--     right after the load instead of waiting for the first save.  A script
--     opened from an existing record never gets that request: its text is the
--     user's own and must not be reformatted behind their back.
--   * AddressList.OnAutoAssemblerEdit is taken over, so double-clicking an AA
--     record opens Notepad3 instead of CE's built-in editor.
--   * The CE edit lock (mr.beginEdit) is never taken: it would block CE's own
--     "更改脚本" and the record deletion in CE (see installEditorHook).
--   * CE's own script editor is not blocked: only the bridge's own template
--     factory is ever hidden, so CE's own "Auto Assemble" / "更改脚本" menus
--     still open CE's editor.  The hook also falls back to CE's editor when
--     Notepad3 is unavailable or the session cannot be created.
--   * Routes into Notepad3: the right-click "自动汇编" submenu and the
--     double-click on an existing AA record.
--   * A "默认模板" submenu driven by the user-editable
--     autorun\np3_user_templates.tpl (nop / ret 1 / jmp / cmp by default).
--     AOB is the default generation method; a persisted setting can switch
--     to fixed addresses.  Selected instructions are always kept as comments.
--   * The hidden AA template factory is created only on first menu popup, so
--     CE startup does not flash a window.
--   * CMD_CHECK_SCRIPT runs CE's own autoAssembleCheck on the current script
--     so the editor can show CE's real verdict instead of local heuristics.
--   * Writing a script to the current address list is always explicit:
--     CMD_APPLY_SCRIPT.  saveTable() is never called.

local PIPE_NAME = 'NP3_CE_BRIDGE'
local MAX_PAYLOAD = 8 * 1024 * 1024
local RUN_TIMEOUT_SECONDS = 30

local CMD_PING, CMD_RUN, CMD_CLOSE = 0, 1, 2
local CMD_APPLY_SCRIPT, CMD_CLOSE_SESSION = 3, 4
local CMD_CHECK_SCRIPT, CMD_SYNC_SCRIPT = 5, 6
local KIND_OUTPUT, KIND_RESULT, KIND_ERROR, KIND_DONE, KIND_INFO = 0, 1, 2, 3, 4

-- Keep strong references: CE's GC must not collect the server/timer/thread.
if NP3_CE_BRIDGE and NP3_CE_BRIDGE.running then
  print('NP3 CE bridge: already running')
  return
end

NP3_CE_BRIDGE = NP3_CE_BRIDGE or {}
local bridge = NP3_CE_BRIDGE
bridge.running = false
-- When false (default), script output is shown only in Notepad3's output pane
-- and CE's own Lua window is never popped up.  Set to true to also mirror
-- output into CE's Lua Engine window (useful while debugging the bridge).
bridge.echoToCe = false
bridge.sessions = {}
bridge.menuPairs = {}
bridge.sessionSeq = 0
-- Checked option: with this on, CE's own "Cheat Table Framework Code" item is
-- invoked before the template so CE emits the full [ENABLE]/[DISABLE] script
-- (including dealloc/db restore).  Default on = table-ready scripts.
bridge.ctFramework = true
bridge.ctFrameworkItem = nil
-- CE 7.7's "Runtime hook script generation" row is an option in this menu.
bridge.runtimeHook = false
-- Run CE's own autoAssembleCheck on a generated default template and surface
-- findings as leading comments.  Disable with np3.setTemplatePrecheck(false).
bridge.precheckTemplates = true
bridge.lastPrecheck = 0
bridge.menuInstalled = false
bridge.editorHookInstalled = false
-- Mono auto-restore state.  |ever| is set once a live Mono pipe is observed.
bridge.monoRecover = { ever=false, attempts=0, nextAt=0, lastReason='', pid=0 }
bridge.monoEnsuring = false

-- Call-chain analysis state.  Two in-memory tables: |regs| holds one register
-- snapshot per breakpoint hit, |stack| holds the raw stack slots that were
-- captured for those hits.  Both are dropped when the analysis is switched
-- off: nothing is written to disk.
-- A previous load of this script may have left a breakpoint behind.  Remove it
-- before the state is reset, otherwise the user's process would keep pausing
-- on a breakpoint nobody handles any more.
if bridge.chain and bridge.chain.bpId then
  pcall(function() debug_removeBreakpointByID(bridge.chain.bpId) end)
end
bridge.chain = {
  enabled = false, bpId = nil, seq = 0,
  regs = {}, stack = {},
  form = nil, item = nil, timer = nil, selSeq = nil,
}

-- ---------------------------------------------------------------------------
-- Convenience API exposed to scripts running through the bridge.
-- Everything here is optional: plain CE API calls work as well.
-- ---------------------------------------------------------------------------

local function resolveAddress(address)
  if type(address) == 'string' then
    requireApi('getAddress')
    local resolved = getAddress(address)
    if not resolved or resolved == 0 then
      error('NP3: cannot resolve symbol: ' .. address)
    end
    return resolved
  end
  return address
end

np3 = np3 or {}
np3.version = '1.1'

-- CE versions expose different Lua APIs (for example, older builds have no
-- debugger hook API).  Probe by name instead of branching on a version number:
-- the probe feeds diagnostics, and the wrappers below turn a missing API into
-- a clear error instead of "attempt to call a nil value".
local CAPABILITY_APIS = {
  'getAddress', 'getOpenedProcessID', 'readBytes', 'writeBytes',
  'createDisassembler', 'getNameFromAddress', 'autoAssemble',
  'debug_setBreakpoint', 'debug_removeBreakpointByID',
  'debug_removeBreakpoint', 'autoAssembleCheck',
  'createPipe', 'createThread', 'createTimer', 'checkSynchronize',
  'synchronize', 'getCheatEngineDir', 'getTempFolder',
  'getAddressList', 'getMemoryViewForm', 'createAutoAssemblerForm',
  'shellExecute', 'LaunchMonoDataCollector', 'targetIs64Bit',
}
local function requireApi(name)
  if type(_G[name]) == 'function' then return true end
  error('NP3: ' .. name .. ' is unavailable in this CE version')
end
np3.capabilities = function()
  local caps = {}
  for _, name in ipairs(CAPABILITY_APIS) do
    caps[name] = (type(_G[name]) == 'function')
  end
  return caps
end

-- Breakpoint callbacks fire after the RUN request has finished. Their print()
-- output still shows up in CE's Lua Engine, and is also buffered here so a
-- later "print(np3.dumpEvents())" from Notepad3 can pull it into the pane.
np3.events = {}
local function logEvent(text)
  np3.events[#np3.events + 1] = text
  if #np3.events > 500 then table.remove(np3.events, 1) end
end
np3.dumpEvents = function()
  local text = table.concat(np3.events, '\n')
  np3.events = {}
  return text
end

np3.process = function()
  requireApi('getOpenedProcessID')
  local info = { pid = getOpenedProcessID() }
  if type(getTargetedProcessInfo) == 'function' then
    local ok, target = pcall(getTargetedProcessInfo)
    if ok then info.target = target end
  end
  return info
end

np3.read = function(address, size)
  requireApi('readBytes')
  return readBytes(resolveAddress(address), size or 1, true)
end

np3.write = function(address, bytes)
  requireApi('writeBytes')
  return writeBytes(resolveAddress(address), bytes)
end

-- "Decompile" the target's code: return a readable disassembly listing.
np3.disassemble = function(address, count)
  requireApi('createDisassembler')
  local addr = resolveAddress(address)
  local disassembler = createDisassembler()
  local lines = {}
  for _ = 1, (count or 8) do
    local opcode = disassembler.disassemble(addr)
    local data = disassembler.getLastDisassembleData() or {}
    local bytes = {}
    for _, b in ipairs(data.bytes or {}) do
      bytes[#bytes + 1] = string.format('%02X', b)
    end
    lines[#lines + 1] = string.format('%X  %-24s %s %s',
                                      addr, table.concat(bytes, ' '),
                                      opcode or '', data.parameters or '')
    local width = #(data.bytes or {})
    if width <= 0 then break end
    addr = addr + width
  end
  return table.concat(lines, '\n')
end

-- Hook a function by placing an execute breakpoint. The callback runs in CE
-- and its print() output is streamed back to the Notepad3 output pane.
-- Returns CE's setbreakpoint values (the second one is the breakpoint ID).
np3.hook = function(address, callback)
  requireApi('debug_setBreakpoint')
  return debug_setBreakpoint(resolveAddress(address), function(...)
    local previousPrint = print
    print = function(...)
      local parts = {}
      for i = 1, select('#', ...) do parts[i] = tostring(select(i, ...)) end
      logEvent(table.concat(parts, '\t'))
      -- Only mirror to CE's Lua window when explicitly enabled.
      if bridge.echoToCe then pcall(previousPrint, ...) end
    end
    local ok, err = pcall(callback, ...)
    print = previousPrint
    if not ok then logEvent('hook error: ' .. tostring(err)) end
  end)
end

np3.unhook = function(id)
  if type(debug_removeBreakpointByID) == 'function' then
    return debug_removeBreakpointByID(id)
  end
  if type(debug_removeBreakpoint) == 'function' then
    return debug_removeBreakpoint(id)
  end
  return false
end

np3.aa = function(script)
  requireApi('autoAssemble')
  return autoAssemble(script)
end

np3.symbols = function(address)
  requireApi('getNameFromAddress')
  return getNameFromAddress(resolveAddress(address))
end

-- ---------------------------------------------------------------------------
-- Call-chain analysis: the two in-memory tables and their two views.
-- Pure logic only: no CE API is called here, so the focused harness can
-- exercise it with plain Lua tables.
-- ---------------------------------------------------------------------------

-- DISABLED until the CE freeze is diagnosed.  A breakpoint hit ran work on
-- CE's debugger thread (name resolution through the symbol handler / Mono
-- pipe plus a synchronize() into the main thread) and could hang CE.  Both
-- the menu entry and chainStart() are gated by this flag; the code is kept so
-- the problem can be reproduced and fixed later.
local CHAIN_ENABLED = false

-- How much of the stack is captured per hit, how many frames are unwound and
-- how many addresses are resolved per hit.  All of them are bounded: the
-- analysis must not grow without limit while the user keeps debugging.
local CHAIN_STACK_BYTES = 32768
local CHAIN_MAX_FRAMES = 64
local CHAIN_MAX_RESOLVE = 64

-- The window is defined further down (it needs CE's form API); it is declared
-- here so the capture code can call it.
local chainEnsureForm
local chainRefreshForm

-- onMainThread() is defined further down as well.  A `local function` is only
-- visible to the code below it, so it is declared here to let the call-chain
-- capture reach it.
local onMainThread

-- Drop the oldest hits once the retention limit is exceeded.  A hit's stack
-- slots are dropped together with its register snapshot, otherwise the stack
-- table would grow without bound.
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
      result[#result + 1] = { depth = 0, func = snap.at, retAddr = snap.rip,
                              slotAddr = nil, offset = nil }
      break
    end
  end
  local frames = {}
  for _, slot in ipairs(state.stack) do
    if slot.seq == seq and slot.isRet then
      frames[#frames + 1] = { depth = slot.depth or 0, func = slot.symbol,
                              retAddr = slot.value, slotAddr = slot.slotAddr,
                              offset = slot.offset }
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

-- Walk the frame-pointer chain and mark the return addresses.  Pure: it only
-- looks at the slots that were captured.
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

-- Fallback for targets compiled without a frame pointer: accept the slots the
-- predicate recognises as code addresses.
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

-- Resolve the captured addresses into names.  Bounded: a Mono lookup is a
-- pipe round trip, so only a limited number of slots is resolved per hit.
-- The frames recognised as return addresses are resolved first, because the
-- call chain is built from them; the remaining slots only fill the stack
-- view.
local function chainResolveSlots(slots, maxResolve)
  local limit = tonumber(maxResolve) or 64
  local resolved = 0
  for _, wantRet in ipairs({ true, false }) do
    for _, s in ipairs(slots) do
      if resolved >= limit then break end
      if (s.isRet == true) == wantRet then
        if s.symbol == nil or s.symbol == '' then
          local ok, name = pcall(function() return getNameFromAddress(s.value) end)
          if ok and type(name) == 'string' then
            s.symbol = name
            resolved = resolved + 1
          end
        end
      end
    end
  end
  return slots
end

-- Is this stack value a code address?  A value inside a module always is; a
-- value that resolved to a name that is not a bare hex string came from a
-- symbol handler (a Mono method name, for instance).
local function chainIsCodeAddress(slot)
  if not slot or type(slot.value) ~= 'number' then return false end
  local ok, inside = pcall(function() return inModule(slot.value) end)
  if ok and inside then return true end
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
        -- The frame-pointer walk needs no names, so it runs first: it marks
        -- the frames that the call chain is built from, and those are the
        -- slots the name resolution has to spend its budget on.
        chainMarkReturns(slots, is64, rbp, CHAIN_MAX_FRAMES)
        chainResolveSlots(slots, CHAIN_MAX_RESOLVE)
        chainMarkByScan(slots, CHAIN_MAX_FRAMES, chainIsCodeAddress)
      end)
      pcall(function() chainRefreshForm() end)
    end)
  end
end

-- Stop the analysis: remove the breakpoint, drop both tables and close the
-- window.  The breakpoint must go away, otherwise the user's process stays
-- paused on a breakpoint nobody handles any more.
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
  state.selSeq = nil
  if state.form then
    pcall(function() state.form.destroy() end)
    state.form = nil
  end
  if state.timer then
    pcall(function() state.timer.destroy() end)
    state.timer = nil
  end
  if bridge.chainItem then
    pcall(function() bridge.chainItem.Checked = false end)
  end
  return true
end

-- Start the analysis at the given address.
local function chainStart(address)
  if not CHAIN_ENABLED then
    return false, 'NP3: 调用链分析已暂时禁用（会导致 CE 卡死，待排查）'
  end
  local state = bridge.chain
  if state.enabled then return true end
  if type(address) ~= 'number' or address == 0 then
    return false, 'NP3: no address selected'
  end
  state.enabled = true
  state.regs = {}
  state.stack = {}
  state.seq = 0
  state.selSeq = nil
  local ok, err = pcall(function()
    local _, id = debug_setBreakpoint(address, chainOnHit)
    state.bpId = id
  end)
  if not ok then
    state.enabled = false
    return false, 'NP3: ' .. tostring(err)
  end
  -- A window that fails to open must be reported: a silent failure would
  -- leave the user with a running analysis and no way to see the result.
  local shown, showErr = pcall(function() chainEnsureForm() end)
  if not shown then
    reportError('NP3: call-chain window: ' .. tostring(showErr))
  end
  return true
end

-- The address the user has selected in CE's disassembler.
local function chainSelectedAddress()
  local ok, addr = pcall(function()
    local mv = getMemoryViewForm()
    local dv = mv and mv.DisassemblerView
    return dv and dv.SelectedAddress or nil
  end)
  if ok and type(addr) == 'number' then return addr end
  return nil
end

-- The call-chain window: registers on the left, the call chain on the right,
-- the raw stack below.  All three panes show the same hit.
function chainEnsureForm()
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
    -- vsReport is what makes the columns and the sub-items work.  Without it
    -- the ListView stays an icon view and has no sub-items at all, so every
    -- row would fail to fill.
    lv.ViewStyle = 'vsReport'
    lv.RowSelect = true
    lv.ReadOnly = true
    for _, text in ipairs(columns) do
      local col = lv.Columns.add()
      col.Caption = text
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
      local st = bridge.chain
      if not st.form or not st.lvRegs then return end
      local idx = st.lvRegs.ItemIndex
      if idx ~= st.selSeq and idx >= 0 then
        st.selSeq = idx
        chainRefreshForm()
      end
    end
    timer.Enabled = true
    state.timer = timer
  end
  return form
end

function chainRefreshForm()
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
    -- A fresh item's SubItems is an empty list: the values have to be
    -- appended, assigning them by index would be out of range.
    item.SubItems.add(tostring(snap.tid or ''))
    item.SubItems.add(string.format('0x%X', snap.rip or 0))
    item.SubItems.add(string.format('0x%X', snap.rsp or 0))
    item.SubItems.add(string.format('0x%X', snap.rbp or 0))
    item.SubItems.add(tostring(snap.at or ''))
  end
  state.lvRegs.endUpdate()
  -- Rebuilding the list drops the selection, so put it back: otherwise the
  -- row the user picked would lose its highlight on every new hit.
  if state.selSeq and state.selSeq >= 0 and state.selSeq < #rows then
    pcall(function() state.lvRegs.ItemIndex = state.selSeq end)
  end

  state.lvChain.beginUpdate()
  state.lvChain.Items.clear()
  for _, node in ipairs(chainBuildChain(state, sel.seq)) do
    local item = state.lvChain.Items.add()
    item.Caption = tostring(node.depth or 0)
    item.SubItems.add(tostring(node.func or ''))
    item.SubItems.add(string.format('0x%X', node.retAddr or 0))
  end
  state.lvChain.endUpdate()

  state.lvStack.beginUpdate()
  state.lvStack.Items.clear()
  for _, slot in ipairs(chainBuildStackView(state, sel.seq)) do
    local item = state.lvStack.Items.add()
    item.Caption = string.format('0x%X', slot.slotAddr or 0)
    item.SubItems.add(string.format('0x%X', slot.offset or 0))
    item.SubItems.add(string.format('0x%X', slot.value or 0))
    item.SubItems.add(tostring(slot.symbol or ''))
  end
  state.lvStack.endUpdate()
end

np3.chainStart = function()
  return chainStart(chainSelectedAddress())
end

np3.chainStop = chainStop

np3.chainDump = function()
  local lines = {}
  for _, node in ipairs(chainBuildChain(bridge.chain, bridge.chain.seq)) do
    lines[#lines + 1] = string.format('%s%s', string.rep('  ', node.depth or 0),
                                      tostring(node.func))
  end
  return table.concat(lines, '\n')
end

-- ---------------------------------------------------------------------------
-- Framing
-- ---------------------------------------------------------------------------

-- CE binds LuaPipe methods as closures over the object: call them with '.',
-- never with ':'.  Passing self makes writeByte()/writeDword() no-ops (they
-- require exactly one argument) and writeString() write a single NUL byte.
-- Also avoid lock()/unlock(): CE's TGuiSafeCriticalSection is thread-owner
-- based and raises "Criticalsection leave without enter" as soon as frames
-- are emitted from a synchronize() callback on the main thread.  There is
-- only one writer per connection (the listener blocks inside synchronize()),
-- so no pipe lock is required.  Check the byte counts to catch silent drops.
local function sendFrame(pipe, kind, text)
  if not pipe then return false end
  text = tostring(text or '')
  return pcall(function()
    if pipe.writeByte(kind) ~= 1 then error('NP3: pipe write failed') end
    if pipe.writeDword(#text) ~= 4 then error('NP3: pipe write failed') end
    if #text > 0 and pipe.writeString(text) ~= #text then
      error('NP3: pipe write failed')
    end
  end)
end

local function readFrame(pipe)
  local cmd = pipe.readByte()
  if cmd == nil then return nil end
  local len = pipe.readDword()
  if len == nil or len < 0 or len > MAX_PAYLOAD then return nil end
  local payload = ''
  if len > 0 then
    payload = pipe.readString(len)
    if payload == nil or #payload ~= len then return nil end
  end
  return cmd, payload
end

-- ---------------------------------------------------------------------------
-- Script execution (always on CE's main thread)
-- ---------------------------------------------------------------------------

local function runScript(pipe, code)
  local originalPrint = print

  print = function(...)
    local parts = {}
    for i = 1, select('#', ...) do
      parts[i] = tostring(select(i, ...))
    end
    sendFrame(pipe, KIND_OUTPUT, table.concat(parts, '\t'))
    -- By default output goes only to the Notepad3 output pane.  Set
    -- NP3_CE_BRIDGE.echoToCe = true to also mirror it into CE's Lua window.
    if bridge.echoToCe then
      pcall(originalPrint, ...)
    end
  end

  local chunk, loadError = load(code, '=Notepad3', 't')
  local ok, result
  if not chunk then
    ok, result = false, loadError
  else
    local hasDebug = (type(debug) == 'table' and debug.sethook and debug.gethook)
    local oldHook, oldMask, oldCount
    if hasDebug then
      -- CE may use a debug hook itself (e.g. for "stop script"); restore it.
      oldHook, oldMask, oldCount = debug.gethook()
      local deadline = os.clock() + RUN_TIMEOUT_SECONDS
      debug.sethook(function()
        if os.clock() > deadline then
          error('NP3: script execution timed out')
        end
      end, '', 100000)
    end
    ok, result = pcall(chunk)
    if hasDebug then
      if oldHook then
        debug.sethook(oldHook, oldMask or '', oldCount or 0)
      else
        debug.sethook()
      end
    end
  end

  print = originalPrint

  if ok then
    sendFrame(pipe, KIND_RESULT, result == nil and '' or tostring(result))
  else
    sendFrame(pipe, KIND_ERROR, tostring(result))
  end
  sendFrame(pipe, KIND_DONE, ok and '0' or '1')
  -- A script may have run CE's autoAssemble / touched mono on this thread.
  if bridge.monoEnsure then pcall(bridge.monoEnsure) end
end

function onMainThread(fn)
  if type(synchronize) == 'function' and type(inMainThread) == 'function'
     and not inMainThread() then
    synchronize(fn)
  else
    fn()
  end
end

local function executeOnMainThread(pipe, code)
  onMainThread(function() runScript(pipe, code) end)
end

-- ---------------------------------------------------------------------------
-- CE editor integration
-- ---------------------------------------------------------------------------

local function pathExists(p)
  if type(fileExists) == 'function' then
    local ok, found = pcall(fileExists, p)
    if ok then return found and true or false end
  end
  local f = io.open(p, 'rb')
  if f then f:close() return true end
  return false
end

local function menuName(item)
  local ok, name = pcall(function() return item.Name end)
  return (ok and name) and tostring(name) or ''
end

local function itemCaption(item)
  local ok, cap = pcall(function() return item.Caption end)
  return (ok and cap) and tostring(cap) or ''
end

-- TMenuItem.Checked is published, and CE's own autorun scripts read/write it
-- directly (e.g. MainForm.miMonoActivate.Checked).  celua.txt does not list it
-- under MenuItem, so keep both the direct field and the RTTI accessor.
local function menuChecked(item)
  if not item then return nil end
  local ok, value = pcall(function() return item.Checked end)
  if ok and type(value) == 'boolean' then return value end
  ok, value = pcall(function() return getProperty(item, 'Checked') end)
  if ok and type(value) == 'boolean' then return value end
  return nil
end

-- Same trick for RadioItem and Visible: Lazarus declares both as published,
-- celua.txt just does not list them under MenuItem.
local function menuRadioItem(item)
  if not item then return false end
  local ok, value = pcall(function() return item.RadioItem end)
  if ok and type(value) == 'boolean' then return value end
  ok, value = pcall(function() return getProperty(item, 'RadioItem') end)
  if ok and type(value) == 'boolean' then return value end
  return false
end

local function menuVisible(item)
  if not item then return nil end
  local ok, value = pcall(function() return item.Visible end)
  if ok and type(value) == 'boolean' then return value end
  ok, value = pcall(function() return getProperty(item, 'Visible') end)
  if ok and type(value) == 'boolean' then return value end
  return nil
end

-- CE's Lua doClick() only invokes the item's OnClick handler (see
-- menuItem_doClick in CE's LuaMenu.pas): the check/radio bookkeeping of
-- Lazarus' TMenuItem.Click never runs.  Option rows of the mirrored menu
-- therefore have to update the source item's state by hand, otherwise the
-- factory keeps its initial values and the generated script ignores the
-- chosen JMP mode.
local function setMenuChecked(item, value)
  if not item then return false end
  local ok = pcall(function() item.Checked = value end)
  if not ok then return false end
  local now = menuChecked(item)
  if now == nil then return true end
  return now == value
end

-- A menu action that fails must not be silent: the bridge's own Lua window is
-- not shown by default, so the user would only notice that "nothing
-- happened".
local function reportError(msg)
  msg = tostring(msg or '')
  if msg == '' then return end
  if bridge.echoToCe then print('NP3: ' .. msg) end
  pcall(function()
    if type(showMessage) == 'function' then showMessage('NP3: ' .. msg) end
  end)
end

-- ---------------------------------------------------------------------------
-- Mono auto-restore: read-only state snapshot and pure recovery decision.
-- All CE global access lives in the snapshot; the decision is a plain function
-- of (snapshot, recover-state, now) so it can be tested in stock Lua with stub
-- globals.  _monoEnsure (added later) owns every CE side effect.
-- ---------------------------------------------------------------------------
local function _monoSnapshot()
  local s = { wanted=false, attached=false, connected=false, failed=false,
              paused=false, pid=0, target=false }
  local okPid, pid = pcall(function() return getOpenedProcessID() end)
  if okPid and type(pid) == 'number' then s.pid = pid end
  local okPause, pause = pcall(function() return isPaused() end)
  if okPause then s.paused = (pause == true) end

  local monoItem = nil
  if type(MainForm) ~= 'nil' then
    local okItem, item = pcall(function() return MainForm.miMonoActivate end)
    if okItem then monoItem = item end
  end
  s.wanted = (menuChecked(monoItem) == true)

  local topItem = nil
  if type(miMonoTopMenuItem) ~= 'nil' then
    local okTop, top = pcall(function() return miMonoTopMenuItem end)
    if okTop then topItem = top end
  end
  -- Conservative: only treat it as a Mono target when Visible is explicitly
  -- true.  An unreadable Visible (nil on some CE versions) must not trigger a
  -- restore/attach on a non-Mono process.
  s.target = (topItem ~= nil and menuVisible(topItem) == true)

  local pipe = nil
  if type(libmono) == 'table' then
    local okTid, tid = pcall(function() return getCurrentThreadID() end)
    if okTid and type(tid) == 'number' and type(libmono.monopipes) == 'table' then
      pipe = libmono.monopipes[tid]
    end
    if pipe == nil and type(monopipe) == 'table' then pipe = monopipe end
    local okMpid, mpid = pcall(function() return libmono.ProcessID end)
    if okMpid and mpid == s.pid then s.attached = (pipe ~= nil) end
    if libmono.fail == true or libmono.abort == true then s.failed = true end
    if libmono.MDC_ShuttingDownAddress ~= nil and type(readInteger) == 'function' then
      local okSd, sd = pcall(readInteger, libmono.MDC_ShuttingDownAddress)
      if okSd and type(sd) == 'number' and sd ~= 0 then s.failed = true end
    end
  elseif type(monopipe) == 'table' then
    pipe = monopipe
    s.attached = true
  end
  if pipe ~= nil then
    local okConn, conn = pcall(function() return pipe.Connected end)
    if okConn then s.connected = (conn == true) end
  end
  return s
end

local function _monoRecoverDecision(s, rec, now)
  local d = { act='skip', reason='', setEver=nil, pid=nil, resetPid=false }
  if s.attached and s.connected then
    d.act='healthy'; d.setEver=true; d.pid=s.pid; d.reason='connected'
    return d
  end
  d.resetPid = (rec.pid ~= 0 and s.pid ~= rec.pid)
  if not s.target then d.reason='not-target'; return d end
  if s.pid == 0 then d.reason='no-process'; return d end
  if s.paused then d.reason='paused'; return d end
  -- An explicit close blocks restore but must not erase the "was active"
  -- history: reopening Mono later should remain recoverable.
  if not s.wanted then d.reason='not-wanted'; return d end
  if not rec.ever then d.reason='never-active'; return d end
  if now < rec.nextAt and not d.resetPid then d.reason='cooldown'; return d end
  if rec.attempts >= 3 and not d.resetPid then d.reason='exhausted'; return d end
  d.act='restore'
  if s.failed then d.reason='failed-state'
  elseif not s.attached then d.reason='no-pipe'
  elseif not s.connected then d.reason='disconnected'
  else d.reason='lost' end
  return d
end

-- Log through print() only.  In runScript print() is already redirected to the
-- Notepad3 output pane; from the watchdog/menu paths it goes to CE's Lua
-- window.  Writing to a shared notifyPipe from these threads would race with
-- the listener's frame writes and corrupt the pipe's byte stream.
local function _monoLog(text)
  pcall(function() print('[MONO] ' .. tostring(text or '')) end)
end

-- Re-activate a lost Mono data collector.  All CE side effects live here.
-- A failed LaunchMonoDataCollector() may pop a modal dialog on CE's main
-- thread; the backoff below keeps it from being called on every tick.
local function _monoEnsure()
  if type(inMainThread) == 'function' and type(synchronize) == 'function' then
    local okMain, main = pcall(function() return inMainThread() end)
    if okMain and not main then
      pcall(synchronize, _monoEnsure)
      return
    end
  end
  if bridge.monoEnsuring then return end
  local rec = bridge.monoRecover
  if not rec then return end
  bridge.monoEnsuring = true
  -- finally semantics: a thrown error must never leave the guard stuck true,
  -- otherwise auto-restore would silently stop working for the whole session.
  local bodyOk, bodyErr = pcall(function()

  local s = _monoSnapshot()
  local d = _monoRecoverDecision(s, rec, os.time())

  if d.setEver ~= nil then rec.ever = d.setEver end
  if d.pid ~= nil then rec.pid = d.pid end
  if d.resetPid then
    rec.attempts = 0
    rec.nextAt = 0
    rec.lastReason = ''
    rec.pid = s.pid
  end

  if d.act == 'healthy' then
    rec.attempts = 0
    rec.nextAt = 0
    rec.lastReason = ''
  elseif d.act == 'skip' then
    rec.lastReason = d.reason
  elseif d.act == 'restore' then
    local okLaunch, res = false, nil
    if type(LaunchMonoDataCollector) == 'function' then
      okLaunch, res = pcall(LaunchMonoDataCollector)
    end
    local s2 = _monoSnapshot()
    local restored = okLaunch and res and res ~= 0 and res ~= false and s2.attached
    if restored then
      local item = nil
      if type(MainForm) ~= 'nil' then
        local okItem, v = pcall(function() return MainForm.miMonoActivate end)
        if okItem then item = v end
      end
      if item == nil or not setMenuChecked(item, true) then
        pcall(function() if item then item.Checked = true end end)
      end
      rec.ever = true
      rec.attempts = 0
      rec.nextAt = 0
      rec.lastReason = ''
      _monoLog('Mono 已自动重新激活 (原因: ' .. tostring(d.reason) .. ')')
    else
      rec.attempts = (rec.attempts or 0) + 1
      rec.nextAt = os.time() + math.min(30, 5 * rec.attempts)
      rec.lastReason = d.reason
      _monoLog('Mono 重新激活失败 (原因: ' .. tostring(d.reason) ..
               ', 返回: ' .. tostring(res) .. ')')
    end
  end

  end)
  bridge.monoEnsuring = false
  if not bodyOk then
    _monoLog('_monoEnsure 内部错误: ' .. tostring(bodyErr))
  end
end

-- Freshly created menu items do not render CE's native radio dot/check mark,
-- so every option shows its state as a leading "✓ " in the caption instead.
-- The native Checked value of the source item is mirrored, never invented.
--
-- CE decides which JMP rows exist when its own Template menu opens
-- (TfrmAutoInject.emplate1Click): the 5/14-byte JMP rows need a 64-bit x86
-- target, the 1-byte exception JMP only an x86 target.  The hidden factory's
-- Template menu never opens, so CE never applies that rule and the mirror
-- would offer options that produce a broken script (jmp1 does not exist on
-- x64).  The same target facts CE uses are available to Lua.
local function targetSupportsFarJmp()
  local ok64, is64 = pcall(function() return targetIs64Bit() end)
  if not (ok64 and is64 == true) then return false end
  local okx86, isx86 = pcall(function() return targetIsX86() end)
  return (okx86 and isx86 == true)
end

local function targetSupportsJmp1()
  local okx86, isx86 = pcall(function() return targetIsX86() end)
  return (okx86 and isx86 == true)
end

local function syncOptionState()
  local groupVisible = nil   -- visibility of the option group above
  for _, pair in ipairs(bridge.menuPairs) do
    local visible = menuVisible(pair.src)
    if pair.name == 'mi5ByteJMP' or pair.name == 'mi14ByteJMP' then
      visible = targetSupportsFarJmp()
    elseif pair.name == 'mi1ByteExceptionJMP' then
      visible = targetSupportsJmp1()
    elseif pair.separator then
      -- CE's separator rows belong to the option group above them.
      if groupVisible ~= nil then visible = groupVisible end
    end
    if pair.isOption and not pair.separator then groupVisible = visible end
    if visible ~= nil then
      pcall(function() pair.child.Visible = visible end)
    end
    if pair.isOption then
      local checked
      if pair.custom == 'ctFramework' then
        checked = (bridge.ctFramework == true)
      elseif pair.custom == 'runtimeHook' then
        checked = (bridge.runtimeHook == true)
      else
        checked = menuChecked(pair.src)
        if checked == nil then checked = menuChecked(pair.child) or false end
      end
      local cap = itemCaption(pair.src)
      pcall(function()
        pair.child.Caption = (checked and '✓ ' or '　 ') .. cap
      end)
    end
  end
end

-- Apply the user's choice to the factory's option row.  Radio rows form a
-- group, so the siblings have to be cleared as well.  CE's own doClick()
-- cannot be used for that (it only fires OnClick, see setMenuChecked).
local function applyOptionState(pair)
  local src = pair.src
  if pair.radio then
    setMenuChecked(src, true)
    for _, other in ipairs(bridge.menuPairs) do
      if other.radio and other.src ~= src then
        setMenuChecked(other.src, false)
      end
    end
  else
    local checked = menuChecked(src)
    setMenuChecked(src, not (checked == true))
  end
end

-- The state of the factory's 1-byte exception JMP row, if CE has one.
local function jmp1OptionEnabled()
  for _, pair in ipairs(bridge.menuPairs) do
    if pair.name == 'mi1ByteExceptionJMP' then
      return menuChecked(pair.src) == true
    end
  end
  return false
end

-- CE's "Cheat Table Framework Code" row inserts [ENABLE]/[DISABLE] into its
-- own editor.  In this bridge it is a checked option: the state controls
-- whether generated templates keep the cheat-table framework.
local function customOptionKey(name, cap)
  if name == 'CheatTablecompliantcodee1' then return 'ctFramework' end
  -- Localized captions vary (CT表框架代码 / CT 表框架代码) and may carry a
  -- tab-separated shortcut, so match on the distinctive words.
  if cap:find('框架代码', 1, true) or cap:find('表框架', 1, true) or
     cap:find('Cheat Table Framework Code', 1, true) then
    return 'ctFramework'
  end
  local lower = cap:lower()
  if name == 'miRuntimeHooking' or
     (lower:find('hook', 1, true) and
      (cap:find('运行时', 1, true) or lower:find('runtime', 1, true))) then
    return 'runtimeHook'
  end
  return nil
end

-- With the option off, keep only the code between [ENABLE] and [DISABLE]:
-- no framework section headers, no CE description comments, no dealloc/db
-- disable section.  Scripts without an [ENABLE] header are returned as-is.
local function stripCtFramework(text)
  if not text or text == '' then return text end
  local lines = {}
  local seenEnable = false
  for line in (text .. '\n'):gmatch('([^\r\n]*)\r?\n') do
    local upper = line:upper()
    if upper:match('^%s*%[DISABLE%]') then
      break
    elseif upper:match('^%s*%[ENABLE%]') then
      seenEnable = true
      lines = {}   -- discard any header before the framework
    elseif seenEnable and line:match('^%s*//') and
           (line:find('从此处到', 1, true) or line:find('用于启用', 1, true) or
            line:find('code will enable', 1, true)) then
      -- CE's framework description comment
    else
      lines[#lines + 1] = line
    end
  end
  if not seenEnable then return text end
  while #lines > 0 and lines[#lines]:match('^%s*$') do table.remove(lines) end
  while #lines > 0 and lines[1]:match('^%s*$') do table.remove(lines, 1) end
  return table.concat(lines, '\r\n')
end

-- Notepad3 writes its own module path into autorun\np3_ce_bridge.path when it
-- deploys this script.  Fall back to CustomEditorPath (legacy usenotepad.lua)
-- and to the portable copy next to CE.
local function editorPath()
  local dir = getCheatEngineDir() or ''
  local f = io.open(dir .. 'autorun\\np3_ce_bridge.path', 'rb')
  if f then
    local p = (f:read('*a') or ''):gsub('%s+$', '')
    f:close()
    if p ~= '' and pathExists(p) then return p end
  end
  if CustomEditorPath and pathExists(CustomEditorPath) then
    return CustomEditorPath
  end
  local fallback = dir .. 'autorun\\notepad\\Notepad3.exe'
  if pathExists(fallback) then return fallback end
  return nil
end

-- The AA form used as a template factory is created lazily (only when the user
-- first opens "自动汇编"), so CE startup never shows a window that flashes and
-- disappears.  Only OUR factory instance is hidden: CE's own script editor
-- must stay usable, so a TfrmAutoInject created by CE (the user opening the
-- built-in editor) is never touched.  The guard flag is raised only while this
-- factory is being created, which keeps the no-flash behaviour without blocking
-- CE.  createAutoAssemblerForm() calls Show itself AFTER the add-notification
-- fires, so setting Visible=false there alone is too late: the form is parked
-- off-screen before Show and hidden again from OnShow as a second line of
-- defence.
local function hideFactoryForm(form, offscreen)
  pcall(function() form.Visible = false end)
  if offscreen then
    -- createAutoAssemblerForm() calls Show after the add-notification, and
    -- Show sets Visible back to true.  These survive that Show: a fully
    -- transparent, zero-sized window cannot paint a visible frame even if
    -- the form recentres itself or Visible is flipped back.
    pcall(function() form.AlphaBlend = true end)
    pcall(function() form.AlphaBlendValue = 0 end)
    pcall(function() form.Width = 1 end)
    pcall(function() form.Height = 1 end)
    pcall(function()
      -- poDesigned, otherwise Show may recentre the form and undo Left/Top.
      pcall(function() form.Position = 0 end)
      form.Left = -32000
      form.Top = -32000
    end)
  end
  pcall(function() form.hide() end)
end

local function ensureFactory()
  if bridge.factory then return bridge.factory end
  if type(createAutoAssemblerForm) ~= 'function' then return nil end
  pcall(function()
    if type(registerFormAddNotification) == 'function' then
      registerFormAddNotification(function(candidate)
        pcall(function()
          if bridge.creatingFactory and candidate and
             tostring(candidate.ClassName or '') == 'TfrmAutoInject' then
            hideFactoryForm(candidate, true)
            pcall(function()
              candidate.OnShow = function(sender)
                hideFactoryForm(sender, true)
              end
            end)
          end
        end)
      end)
    end
  end)
  bridge.creatingFactory = true
  local ok, form = pcall(createAutoAssemblerForm, '')
  bridge.creatingFactory = false
  if not ok or not form then return nil end
  hideFactoryForm(form, true)
  bridge.factory = form
  return form
end

-- CE 7.x names the Template menu "emplate1"; CE 7.7 may add items such as
-- "Runtime hook script generation", so enumerate at runtime instead of
-- hard-coding the menu structure found in the upstream source.
local function getTemplateMenu(form)
  if not form then return nil end
  local ok, tpl = pcall(function() return form.emplate1 end)
  if ok and tpl and tpl.Count and tpl.Count > 0 then return tpl end
  ok, tpl = pcall(function()
    local menu = form.Menu
    if not menu or not menu.Items then return nil end
    local items = menu.Items
    for i = 0, items.Count - 1 do
      local it = items.getItem(i)
      local cap = it and itemCaption(it) or ''
      if cap == 'Template' or cap == '模板' then return it end
    end
    return nil
  end)
  if ok then return tpl end
  return nil
end

local function writeMeta(path, sid, memrecId, caption, address, description, autoFormat)
  local f, err = io.open(path, 'wb')
  if not f then return false, err end
  f:write('version=1\n')
  f:write('id=' .. sid .. '\n')
  -- Persist the real memory-record id so Notepad3 can target the same record
  -- when syncing saved edits (hardcoding 0 made every sync create a new record).
  f:write('memrecId=' .. tostring(tonumber(memrecId) or 0) .. '\n')
  f:write('description=' .. tostring(description or '') .. '\n')
  f:write('address=' .. tostring(address or '') .. '\n')
  f:write('templateCaption=' .. tostring(caption or '') .. '\n')
  -- format=1 asks the editor to run its CE formatter once after the load.
  -- Only freshly generated template code sets it; a record's own script stays
  -- byte-identical to what CE stores.
  f:write('format=' .. (autoFormat and '1' or '0') .. '\n')
  f:close()
  return true
end

local function selectedAddressString()
  local ok, text = pcall(function()
    local mv = getMemoryViewForm()
    if not mv or not mv.DisassemblerView then return '' end
    return string.format('%X', mv.DisassemblerView.SelectedAddress or 0)
  end)
  return (ok and text) or ''
end

-- Template click -> session .cea + sidecar .np3ce -> launch/reuse Notepad3.
-- No memory record is created here; the first explicit APPLY_SCRIPT does that.
-- autoFormat asks the editor to format the freshly generated code once.
local function newSession(text, caption, memrecId, description, autoFormat)
  local exe = editorPath()
  if not exe then return nil, 'NP3: Notepad3.exe not found (deploy the CE bridge first)' end
  local base = getTempFolder() or getCheatEngineDir()
  if not base then return nil, 'NP3: no temp folder' end
  if not base:match('\\$') then base = base .. '\\' end
  local dir = base .. 'np3_ce_sessions\\'
  if lfs and lfs.mkdir then pcall(lfs.mkdir, dir) end

  bridge.sessionSeq = (bridge.sessionSeq or 0) + 1
  local sid = string.format('np3-%d-%d', os.time(), bridge.sessionSeq)
  local scriptPath = dir .. sid .. '.cea'
  local metaPath = scriptPath .. '.np3ce'

  -- Write the raw UTF-8 bytes ourselves: CE's TStringList.saveToFile() may
  -- use a local code page, which would corrupt Chinese template comments.
  local normalized = text:gsub('\r\n', '\n'):gsub('\n', '\r\n')
  local scriptFile, ferr = io.open(scriptPath, 'wb')
  if not scriptFile then
    return nil, 'NP3: cannot write session script: ' .. tostring(ferr)
  end
  -- A BOM lets Notepad3 auto-detect UTF-8 without encoding switches; the
  -- line endings are already normalized to CRLF above.
  scriptFile:write('\239\187\191')
  scriptFile:write(normalized)
  scriptFile:close()

  local address = selectedAddressString()
  local desc = description or (tostring(caption or 'CE Template') .. ' @ ' .. address)
  writeMeta(metaPath, sid, memrecId, caption, address, desc, autoFormat)
  bridge.sessions[sid] = {
    memrecId = tonumber(memrecId) or 0,
    description = desc,
    scriptPath = scriptPath,
    metaPath = metaPath,
    created = os.time()
  }

  -- CE hands the whole parameters string to the editor as one argument.  A
  -- quoted path or any switch can be taken literally as part of the file name,
  -- so pass a bare path (the temp folder normally uses a short path).  Quote
  -- only when the path itself contains spaces.  Encoding/EOL are in the file.
  local params = scriptPath
  if scriptPath:find(' ', 1, true) then
    params = '"' .. scriptPath .. '"'
  end
  local launched, lerr = pcall(shellExecute, exe, params)
  if not launched then return nil, 'NP3: cannot launch Notepad3: ' .. tostring(lerr) end
  return sid
end

-- ---------------------------------------------------------------------------
-- NP3 default templates.  Unlike the mirrored CE Template items, these build
-- an AA script directly from the selected disassembly range.  The selected
-- instructions are commented out in the cave; the cave always ends with
-- "jmp returnhere" so execution goes back to the original function.
-- ---------------------------------------------------------------------------

-- Built-in fallback when the user template file is missing or entirely
-- invalid.  The deployed .tpl contains the same four specs in editable form.
local function fallbackBody(code)
  return table.concat({
    '[ENABLE]', '${METHOD_SCAN}',
    'alloc(newmem,2048,${INJECT_ADDR})',
    'label(returnhere)', 'label(originalcode)', 'label(exit)',
    '${METHOD_REGISTER}', '',
    'newmem:', '// 原始代码 / original code (commented out):',
    '${ORIGINAL_CODE}', code, '',
    'exit:', 'jmp returnhere', '',
    '${INJECT_ADDR}:', 'jmp newmem', '${NOP_COUNT_LINE}', 'returnhere:', '',
    '[DISABLE]', '${INJECT_ADDR}:', '${ORIG_BYTES_LINE}',
    'dealloc(newmem)', '${METHOD_UNREGISTER}'
  }, '\n')
end

local DEFAULT_TEMPLATE_SPECS = {
  { name = 'nop',   body = fallbackBody('${NOPS}') },
  { name = 'ret 1', body = fallbackBody('mov rax,1\nret') },
  { name = 'jmp',   body = fallbackBody('${JMP_LINE}') },
  { name = 'cmp',   body = fallbackBody('${CMP_EQUAL}') }
}

-- Parse the user template file in a sandbox.  Only harmless Lua libraries are
-- exposed: a template file is data, never code with access to CE or the OS.
-- Pure function so test/TestCeBridgeLua.ps1 can run it in a stock Lua 5.3.
local function parseUserTemplates(src)
  if type(src) ~= 'string' or src == '' then return nil end
  local env = {
    string = string, table = table, math = math,
    tonumber = tonumber, tostring = tostring, type = type,
    pairs = pairs, ipairs = ipairs, select = select
  }
  local chunk = load(src, '=np3_templates', 't', env)
  if not chunk then return nil end
  local ok, data = pcall(chunk)
  if not ok or type(data) ~= 'table' then return nil end
  local settings = (type(data.settings) == 'table') and data.settings or {}
  local list = data.templates or data
  if type(list) ~= 'table' then return nil end
  local method = tostring(settings.method or 'aob'):lower()
  if method ~= 'aob' and method ~= 'address' then method = 'aob' end
  local templates = {}
  for _, spec in ipairs(list) do
    if type(spec) == 'table' and type(spec.name) == 'string' and
       spec.name ~= '' and type(spec.body) == 'string' and spec.body ~= '' then
      local perMethod = (type(spec.method) == 'string') and spec.method:lower() or nil
      if perMethod ~= 'aob' and perMethod ~= 'address' then perMethod = nil end
      templates[#templates + 1] = {
        name = spec.name, body = spec.body,
        method = perMethod, ctFramework = spec.ctFramework
      }
      if #templates >= 32 then break end
    end
  end
  if #templates == 0 then return nil end
  return { method = method, templates = templates }
end

-- Render one template spec against the selected instruction range.
-- method = 'aob' (default) or 'address'.  Original instructions are always
-- kept as comments: a body that forgets ${ORIGINAL_CODE} gets it injected.
local function renderUserTemplate(spec, info, method)
  if type(spec) ~= 'table' or type(spec.body) ~= 'string' or
     type(info) ~= 'table' then
    return nil, 'bad template or selection'
  end
  local ins = info.lines or {}
  local bytes = info.bytes or {}
  if #ins == 0 or #bytes == 0 then return nil, 'no selected instructions' end
  local addr = info.addressExpr or string.format('%X', info.start or 0)
  local pad = #bytes - 5   -- "jmp newmem" is 5 bytes
  if pad < 0 then return nil, 'selection shorter than a 5-byte jmp' end
  method = (method == 'address') and 'address' or 'aob'

  local comments, bare, hex = {}, {}, {}
  for _, it in ipairs(ins) do
    local line = tostring(it.text or '')
    bare[#bare + 1] = line
    comments[#comments + 1] = '// ' ..
      string.format('%X', it.address or 0) .. ': ' .. line
  end
  for _, b in ipairs(bytes) do hex[#hex + 1] = string.format('%02X', b) end
  local bytesText = table.concat(hex, ' ')

  local jumpTarget, jmpLine, cmpEqual = '', '', ''
  for _, it in ipairs(ins) do
    local op = tostring(it.opcode or ''):lower()
    local par = tostring(it.parameters or ''):gsub('^%s+', ''):gsub('%s+$', '')
    if jmpLine == '' and par ~= '' and
       (it.isConditionalJump or (op:match('^j') and op ~= 'jmp')) then
      jumpTarget = par
      jmpLine = 'jmp ' .. par
    end
    if cmpEqual == '' and op == 'cmp' then
      local a, b = par:match('^([^,]+),%s*([^,]+)$')
      if a and b then
        local reg = a:gsub('%s+$', '')
        if reg:find('[', 1, true) then
          local alt = b:gsub('^%s+', ''):gsub('%s+$', '')
          if not alt:find('[', 1, true) then reg = alt end
        end
        cmpEqual = 'cmp ' .. reg .. ',' .. reg
      end
    end
  end

  local module = addr:match('^"([^"]+)"')
  local scan = ''
  if method == 'aob' then
    if module and module ~= '' then
      scan = 'aobscanmodule(INJECT,"' .. module .. '",' .. bytesText .. ')'
    else
      scan = 'aobscan(INJECT,' .. bytesText .. ')'
    end
  end
  local nops = {}
  for _ = 1, #ins do nops[#nops + 1] = 'nop' end

  local vars = {
    METHOD_SCAN = scan,
    METHOD_REGISTER = (method == 'aob') and 'registersymbol(INJECT)' or '',
    METHOD_UNREGISTER = (method == 'aob') and 'unregistersymbol(INJECT)' or '',
    INJECT_ADDR = (method == 'aob') and 'INJECT' or addr,
    ADDRESS = addr,
    HEX_ADDRESS = string.format('%X', info.start or 0),
    ORIGINAL_CODE = table.concat(comments, '\n'),
    ORIGINAL_CODE_BARE = table.concat(bare, '\n'),
    ORIG_BYTES = bytesText,
    ORIG_BYTES_LINE = 'db ' .. bytesText,
    NOP_COUNT = tostring(pad),
    NOP_COUNT_LINE = (pad > 0) and ('nop ' .. tostring(pad)) or '',
    NOPS = table.concat(nops, '\n'),
    INSTRUCTION_COUNT = tostring(#ins),
    JUMP_TARGET = jumpTarget,
    JMP_LINE = jmpLine,
    CMP_EQUAL = cmpEqual,
    SELECTED_TEXT = table.concat(bare, '\n'),
    NEWLINE = '\n', TAB = '\t'
  }

  local body = spec.body
  if not body:find('${ORIGINAL_CODE}', 1, true) then
    if body:find('newmem:', 1, true) then
      body = body:gsub('(newmem:%s*\n)', '%1${ORIGINAL_CODE}\n', 1)
    else
      body = '${ORIGINAL_CODE}\n' .. body
    end
  end
  for key in body:gmatch('%${([%w_]+)}') do
    if vars[key] == nil then
      return nil, 'unknown placeholder ' .. tostring(key)
    end
  end

  body = body:gsub('\r\n', '\n'):gsub('\r', '\n')
  local kept = {}
  for line in (body .. '\n'):gmatch('([^\n]*)\n') do
    local key = line:match('^%s*%${([%w_]+)}%s*$')
    if not (key and vars[key] == '') then kept[#kept + 1] = line end
  end
  body = table.concat(kept, '\n')
  -- Older copies of the default template declare label(originalcode) but,
  -- because the original instructions are emitted as comments, never define
  -- the label.  The .tpl is only deployed when absent, so backfill the
  -- definition here instead of overwriting a user's edited template.
  if body:find('label(originalcode)', 1, true) and
     not body:find('\n%s*originalcode:%s*\n') then
    body = body:gsub('(newmem:[^\n]*\n)', '%1originalcode:\n', 1)
  end
  local out = body:gsub('%${([%w_]+)}', function(k) return vars[k] or '' end)

  -- Normalize layout before handing the script to the editor: strip trailing
  -- blanks, collapse blank runs and give [ENABLE]/[DISABLE] a blank line on
  -- each side.  This keeps generated scripts readable in Notepad3 and CE.
  local lines = {}
  for line in (out .. '\n'):gmatch('([^\n]*)\n') do
    lines[#lines + 1] = line:gsub('%s+$', '')
  end
  local final = {}
  local function push(line)
    if line == '' then
      if #final > 0 and final[#final] ~= '' then final[#final + 1] = '' end
    else
      final[#final + 1] = line
    end
  end
  for _, line in ipairs(lines) do
    local upper = line:upper()
    if upper:match('^%s*%[ENABLE%]%s*$') or upper:match('^%s*%[DISABLE%]%s*$') then
      push('')
      push(line)
      push('')
    else
      push(line)
    end
  end
  while #final > 0 and final[1] == '' do table.remove(final, 1) end
  while #final > 0 and final[#final] == '' do table.remove(final) end
  return table.concat(final, '\r\n')
end

-- Read the selected disassembler range.  Keep disassembling until at least 5
-- bytes are covered, otherwise a 5-byte "jmp newmem" would clobber the next
-- instruction.  CE's Disassembler.getLastDisassembleData() supplies opcode,
-- parameters, bytes and isConditionalJump (celua.txt:3323-3346).
local function collectSelection()
  local ok, result = pcall(function()
    local mv = getMemoryViewForm()
    local dv = mv and mv.DisassemblerView
    if not dv then return nil end
    local start = dv.SelectedAddress
    local stop = dv.SelectedAddress2
    if type(start) ~= 'number' or start == 0 then return nil end
    if type(stop) ~= 'number' then stop = start end
    -- CE uses the lower of both addresses as the start of the range (see
    -- TfrmAutoInject.Codeinjection1Click and getSelectionSize), so a
    -- selection made upwards has to work the same way here.
    if stop < start then start, stop = stop, start end
    if stop - start > 4096 then return nil end
    local dis = createDisassembler()
    if not dis then return nil end
    local lines, bytes = {}, {}
    local addr = start
    -- The selection ends at the address behind the last selected byte
    -- (getSelectionSize = max - min), so the instruction at "stop" does not
    -- belong to it: covering it would overwrite a byte the user did not
    -- select.
    while (addr < stop or #bytes < 5) and #bytes < 64 do
      pcall(function() dis.disassemble(addr) end)
      local d = {}
      pcall(function() d = dis.getLastDisassembleData() or {} end)
      local raw = d.bytes or {}
      local n = #raw
      if n <= 0 then break end
      local op = tostring(d.opcode or '')
      local par = tostring(d.parameters or '')
      lines[#lines + 1] = {
        address = addr,
        opcode = op,
        parameters = par,
        text = (op .. ' ' .. par):gsub('%s+$', ''),
        bytes = raw,
        isJump = (d.isJump == true),
        isConditionalJump = (d.isConditionalJump == true)
      }
      for i = 1, n do bytes[#bytes + 1] = raw[i] end
      addr = addr + n
    end
    if #lines == 0 or #bytes < 5 then return nil end
    local expr = string.format('%X', start)
    local nameOk, name = pcall(getNameFromAddress, start)
    if nameOk and type(name) == 'string' and name:find('+', 1, true) then
      local mod = name:match('^([^+]+)') or ''
      local rest = name:match('%+.*$') or ''
      -- CE already wraps the module part of getNameFromAddress() in quotes
      -- (e.g. '"game.exe"+1234'); do not add a second pair, which would make
      -- the generated alloc/label lines show '""game.exe""+1234' in CE.
      if mod ~= '' then
        if mod:sub(1, 1) == '"' and mod:sub(-1) == '"' then
          expr = mod .. rest
        else
          expr = '"' .. mod .. '"' .. rest
        end
      end
    end
    return {
      start = start, finish = addr, lines = lines,
      bytes = bytes, addressExpr = expr
    }
  end)
  return (ok and result) or nil
end

-- ---------------------------------------------------------------------------
-- User template loading / hot reload / persisted method setting
-- ---------------------------------------------------------------------------

local function userTemplatePath()
  return (getCheatEngineDir() or '') .. 'autorun\\np3_user_templates.tpl'
end

local function templateStamp(path)
  local ok, value = pcall(function()
    if lfs and lfs.attributes then
      return lfs.attributes(path, 'modification') or 0
    end
    local f = io.open(path, 'rb')
    if not f then return 0 end
    local n = #(f:read('*a') or '')
    f:close()
    return n
  end)
  return (ok and value) or 0
end

local function readMethodSetting()
  local ok, value = pcall(function()
    if type(getSettingsOption) == 'function' then
      return getSettingsOption('NP3_TEMPLATE_METHOD')
    end
    return nil
  end)
  if ok and (value == 'aob' or value == 'address') then return value end
  return nil
end

local function writeMethodSetting(value)
  pcall(function()
    if type(setSettingsOption) == 'function' then
      setSettingsOption('NP3_TEMPLATE_METHOD', value)
    end
  end)
end

local function loadUserTemplates()
  local path = userTemplatePath()
  local src = nil
  local f = io.open(path, 'rb')
  if f then
    src = f:read('*a')
    f:close()
  end
  local parsed = src and parseUserTemplates(src) or nil
  if not parsed then
    parsed = { method = 'aob', templates = DEFAULT_TEMPLATE_SPECS }
    bridge.templateErrors = {
      src and 'user template file invalid; built-in templates in use'
          or 'user template file missing; built-in templates in use'
    }
  else
    bridge.templateErrors = {}
  end
  bridge.userTemplates = parsed
  bridge.templateStamp = templateStamp(path)
  bridge.templateMethod = readMethodSetting() or parsed.method or 'aob'
  return parsed
end

local function generateDefaultTemplate(spec)
  local info = collectSelection()
  if not info then return false, 'NP3: no usable selection in the disassembler' end
  local method = spec.method or bridge.templateMethod or 'aob'
  local text, err = renderUserTemplate(spec, info, method)
  if not text then return false, 'NP3: ' .. tostring(err) end
  local keepFramework = (spec.ctFramework == 'on') or
    (spec.ctFramework ~= 'off' and bridge.ctFramework == true)
  if not keepFramework then
    text = stripCtFramework(text)
  end
  if text == '' then return false, 'NP3: template produced no script' end

  -- CE's own syntax check (syntaxcheckonly, no writes).  Keep the result in
  -- bridge.lastPrecheck (shown by np3.dump()) instead of injecting "//"
  -- comment lines above [ENABLE]: that region is parsed as Lua by Notepad3,
  -- where "//" is an operator, so the injected comments caused false
  -- "undefined variable" diagnostics.  Notepad3's own lint inspects the
  -- opened script, so no diagnostic is lost by not polluting the script.
  bridge.lastPrecheck = 0
  if bridge.precheckTemplates ~= false and type(np3.checkScript) == 'function' then
    local ok, report = pcall(np3.checkScript, text)
    if ok and type(report) == 'string' then
      bridge.lastPrecheck = tonumber(report:match('^OK:(%d+)')) or 0
      -- CE 预检 / CE precheck findings stay available via np3.dump(); they are
      -- deliberately not written into the generated script.
      if bridge.lastPrecheck > 0 and bridge.echoToCe then
        print('NP3: CE 预检 / CE precheck: ' .. tostring(bridge.lastPrecheck) .. ' finding(s)')
        for line in report:gmatch('[^\r\n]+') do
          local detail = line:match('^%d+|[ew]|(.+)$')
          if detail then print('NP3:   ' .. detail) end
        end
      end
    end
  end

  local sid, serr = newSession(text, '默认模板/' .. tostring(spec.name), 0, nil, true)
  if not sid then return false, serr end
  pcall(_monoEnsure)
  return true, sid
end

-- CE's MenuItem offers clear()/delete(index) but no reliable Visible property,
-- so hot reload rebuilds the whole submenu instead of hiding pooled items.
local function rebuildTemplateMenu()
  local defRoot = bridge.defaultRoot
  if not defRoot then return end
  pcall(function() defRoot.clear() end)
  local specs = (bridge.userTemplates and bridge.userTemplates.templates) or {}
  for i, spec in ipairs(specs) do
    local child = createMenuItem(defRoot)
    child.Name = 'miNP3Default' .. tostring(i)
    child.Caption = spec.name
    child.OnClick = function()
      local good, msg = generateDefaultTemplate(spec)
      if not good then reportError(msg) end
    end
    defRoot.add(child)
  end
  local methodChild = createMenuItem(defRoot)
  methodChild.Name = 'miNP3TemplateMethod'
  methodChild.Caption =
    (bridge.templateMethod == 'aob' and '✓ 生成方法: AOB' or '　 生成方法: 地址')
  methodChild.OnClick = function()
    np3.setTemplateMethod((bridge.templateMethod == 'aob') and 'address' or 'aob')
  end
  defRoot.add(methodChild)
  bridge.methodChild = methodChild
  bridge.defaultChildren = specs
end

local function reloadTemplates()
  loadUserTemplates()
  rebuildTemplateMenu()
  return tostring(#bridge.userTemplates.templates) ..
         ' templates, method=' .. tostring(bridge.templateMethod)
end

np3.reloadTemplates = reloadTemplates
np3.setTemplateMethod = function(value)
  bridge.templateMethod = (value == 'address') and 'address' or 'aob'
  writeMethodSetting(bridge.templateMethod)
  rebuildTemplateMenu()
  return bridge.templateMethod
end
np3.setTemplatePrecheck = function(value)
  bridge.precheckTemplates = (value ~= false)
  return bridge.precheckTemplates
end

-- Run a mirrored CE template item and hand the generated script to Notepad3.
-- seedFramework = false skips the [ENABLE]/[DISABLE] seeding, which is what
-- rows that are plain switches need.
local function generateAndEdit(src, seedFramework)
  local form = ensureFactory()
  if not form then return false, 'NP3: no auto-assembler template factory' end
  -- The 1-byte exception JMP only exists on x86 targets.  CE hides the row
  -- itself, so refuse to generate a script CE cannot assemble instead of
  -- handing out a broken one.
  if jmp1OptionEnabled() and not targetSupportsJmp1() then
    return false, 'NP3: the 1-byte exception JMP is not supported by the target process'
  end
  pcall(function() form.assemblescreen.Lines.clear() end)
  -- CE writes the [DISABLE] restore section (dealloc/db) only when the
  -- [ENABLE]/[DISABLE] headers already exist.  With the option on, invoke
  -- CE's own "Cheat Table Framework Code" item before the template.
  local seeded = ''
  if seedFramework ~= false and bridge.ctFramework == true and
     bridge.ctFrameworkItem then
    pcall(function() bridge.ctFrameworkItem.doClick() end)
    pcall(function() seeded = form.assemblescreen.Lines.Text or '' end)
  end
  local ok, err = pcall(function() src.doClick() end)
  local text = ''
  pcall(function() text = form.assemblescreen.Lines.Text or '' end)
  pcall(function() form.assemblescreen.Lines.clear() end)
  if not ok then return false, 'NP3: CE template failed: ' .. tostring(err) end
  if text == '' or text == seeded then
    return false, 'NP3: template cancelled or produced no script'
  end
  if bridge.ctFramework == false then
    text = stripCtFramework(text)
  end
  if text == '' then return false, 'NP3: template produced no script' end

  local caption = itemCaption(src)
  local sid, serr = newSession(text, caption, 0, nil, true)
  if not sid then return false, serr end
  pcall(_monoEnsure)
  return true, sid
end

-- Mirror CE's native Template items on first popup.  Building this at CE
-- startup creates the hidden TfrmAutoInject factory, which flashes a window;
-- deferring it until the user actually opens "自动汇编" removes the flash.
local function lazyBuildMirror()
  if bridge.mirrorBuilt then return end
  local tpl = getTemplateMenu(ensureFactory())
  if not tpl or tpl.Count == 0 then return end
  local used = 0
  bridge.menuPairs = {}
  for i = 0, tpl.Count - 1 do
    local src = tpl.getItem(i)
    local cap = src and itemCaption(src) or ''
    local name = src and menuName(src) or ''
    if src and cap ~= '' then
      local child = createMenuItem(bridge.autoRoot)
      -- CE groups its JMP rows with separator rows; dropping them would make
      -- the 1-byte exception JMP look like a third radio option.
      local separator = (cap == '-')
      local isOption = (not separator) and
                       (name == 'mi5ByteJMP' or name == 'mi14ByteJMP' or
                        name == 'mi1ByteExceptionJMP' or
                        (cap:find('JMP', 1, true) ~= nil))
      local custom = (not separator) and customOptionKey(name, cap) or nil
      local radio = isOption and menuRadioItem(src)
      if not radio and (name == 'mi5ByteJMP' or name == 'mi14ByteJMP') then
        radio = true
      end
      if custom == 'ctFramework' then bridge.ctFrameworkItem = src end
      local pair = {
        child = child, src = src, name = name, caption = cap,
        separator = separator, isOption = (isOption or custom ~= nil),
        custom = custom, radio = radio
      }
      local setOk = pcall(function()
        child.Name = 'miNP3Tpl' .. tostring(i)
        child.Caption = cap
        if separator then
          -- A separator only groups the rows above it.
        elseif custom then
          child.OnClick = function()
            bridge[custom] = not bridge[custom]
            syncOptionState()
            if custom == 'runtimeHook' then
              -- CE 7.7 may implement that row as a real generator instead of a
              -- plain switch: run it and pick up whatever it produced.  A row
              -- that produces nothing is a switch, so no error is reported.
              generateAndEdit(src)
            end
          end
        else
          if isOption then
            child.OnClick = function()
              -- doClick() would not change the state of the factory's option
              -- row (see setMenuChecked), so apply it here.
              applyOptionState(pair)
              syncOptionState()
            end
          else
            child.OnClick = function()
              local good, msg = generateAndEdit(src)
              if not good then reportError(msg) end
            end
          end
        end
      end)
      if setOk then
        -- Insert before the "默认模板" submenu so CE's own templates stay on
        -- top, as they were before lazy creation was introduced.
        local inserted = pcall(function()
          bridge.autoRoot.insert(bridge.autoRoot.Count - 1, child)
        end)
        if not inserted then bridge.autoRoot.add(child) end
        bridge.menuPairs[#bridge.menuPairs + 1] = pair
        used = used + 1
      end
    end
  end
  bridge.menuCount = used
  bridge.mirrorBuilt = true
end

local function installMenu()
  if bridge.menuInstalled then return true end
  local ok, err = pcall(function()
    local mv = getMemoryViewForm()
    if not mv then error('no memory view form yet') end
    local pm = mv.debuggerpopup
    if not pm then
      pm = mv.DisassemblerView.getPopupMenu()
    end
    if not pm or not pm.Items then error('no disassembler popup menu') end

    local items = pm.Items
    for i = 0, items.Count - 1 do
      if menuName(items.getItem(i)) == 'miNP3AutoAssemble' then
        bridge.menuInstalled = true
        return
      end
    end

    local root = createMenuItem(pm)
    root.Name = 'miNP3AutoAssemble'
    -- CE's own row is localized ("Auto Assemble" / "自动汇编"), so the
    -- bridge's submenu must not carry the same caption: two identical rows
    -- in the same popup are indistinguishable.
    root.Caption = '自动汇编 (Notepad3)'
    bridge.autoRoot = root

    local defRoot = createMenuItem(root)
    defRoot.Name = 'miNP3DefaultTemplates'
    defRoot.Caption = '默认模板'
    bridge.defaultRoot = defRoot
    root.add(defRoot)

    loadUserTemplates()
    rebuildTemplateMenu()

    -- Insert immediately after CE's own "Auto Assemble" item; never modify it.
    local index = nil
    pcall(function()
      if mv.AutoInject1 then index = mv.AutoInject1.MenuIndex end
    end)
    if not index then
      for i = 0, items.Count - 1 do
        local it = items.getItem(i)
        local cap = it and itemCaption(it) or ''
        -- The localized caption may differ ("自动汇编器" ...), so match on the
        -- distinctive part and never on the bridge's own row.
        if (cap:find('Auto Assemble', 1, true) or cap:find('自动汇编', 1, true)) and
           menuName(it) ~= 'miNP3AutoAssemble' then
          index = it.MenuIndex
          break
        end
      end
    end
    if index and index >= 0 then
      items.insert(index + 1, root)
    else
      items.add(root)
    end

    -- Standalone toggle: analyse the call chain at the selected address.  It
    -- is not a submenu of the auto-assembler row, so it goes into the popup's
    -- own item list.  Hidden while the feature is disabled: a menu entry that
    -- visibly freezes CE is worse than no entry at all.
    if CHAIN_ENABLED then
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
    end

    local oldOnPopup = pm.OnPopup
    pm.OnPopup = function(sender)
      if oldOnPopup then pcall(oldOnPopup, sender) end
      lazyBuildMirror()
      if templateStamp(userTemplatePath()) ~= bridge.templateStamp then
        reloadTemplates()
      end
      syncOptionState()
    end

    bridge.menuInstalled = true
  end)
  if not ok then return false, err end
  return true
end

-- Double-clicking an AA record in CE's address list opens Notepad3 instead of
-- CE's built-in editor.  The hook returns false when Notepad3 is unavailable
-- or a session cannot be created, so CE opens its own editor in that case.
--
-- The record's CE edit lock (mr.beginEdit) is deliberately NOT taken.  CE's
-- own sources make that lock fatal for the native editor:
--   * MainUnit.pas AddressListAutoAssemblerEdit: while memrec.isBeingEdited
--     it only re-shows memrec.autoAssembleWindow - and since we intercept the
--     double click, CE never created that window, so "更改脚本" would do
--     nothing at all.
--   * addresslist.pas deleteSelected: records with isBeingEdited=true are
--     silently skipped, so the record could never be deleted.
-- The lock would also have to be released by a close-session frame that races
-- with Notepad3's exit, so any lost frame left the record locked forever.
-- Without the lock the record is resolved by ID on demand instead, which also
-- removes the dangling-pointer risk of caching the record object.
local function installEditorHook()
  if bridge.editorHookInstalled then return true end
  return pcall(function()
    local al = getAddressList()
    if not al then error('no address list') end
    al.OnAutoAssemblerEdit = function(list, mr)
      if not editorPath() then return false end
      local ok, script = pcall(function() return mr.Script or '' end)
      if not ok then return false end
      local label = 'CE 记录 #' .. tostring(mr.ID)
      -- No format request: the record holds the user's own script and opening
      -- it must never rewrite the text CE stores.
      local sid, err = newSession(script, label, mr.ID, label)
      if not sid then
        if bridge.echoToCe then print('NP3: ' .. tostring(err)) end
        return false
      end
      return true
    end
    bridge.editorHookInstalled = true
  end)
end

-- Explicit user action: add/update an AA memory record in CE's current
-- address list.  Never calls saveTable(); the .CT file stays untouched.
local function applyScript(payload)
  local head, script = string.match(payload or '', '^([^\n]*)\n(.*)$')
  if not head then return 'ERR:bad apply payload' end
  local sid, memid = string.match(head, '^([^|]*)|(%d*)$')
  if not sid then return 'ERR:bad session header' end
  if type(vtAutoAssembler) ~= 'number' then return 'ERR:vtAutoAssembler unavailable' end

  local al = getAddressList()
  if not al then return 'ERR:no address list' end
  local session = bridge.sessions[sid]
  local mr = nil
  local idNum = tonumber(memid or '') or 0
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
  local rid = mr.ID or 0
  if session then session.memrecId = rid end
  return 'OK:' .. tostring(rid)
end

-- After a successful save in Notepad3, push the current buffer into the CE
-- address list.  Idempotent: an existing record is updated in place, a missing
-- one is created.  A record can be missing because the script was generated
-- from the disassembler (it has no record yet) or because the user deleted it
-- in CE while it was open in Notepad3.
-- The record is resolved by its ID on every call instead of caching the
-- record object: the user may delete the record in CE while it is open in
-- Notepad3, and a cached pointer would then be a use-after-free.
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

local function closeSession(payload)
  local sid = (payload or ''):gsub('%s+$', '')
  local session = bridge.sessions[sid]
  bridge.sessions[sid] = nil
  if session then
    -- Only temp-file cleanup: the bridge holds no CE edit lock, so there is
    -- no endEdit() to pair with anything.  Safe to run off CE's main thread.
    pcall(function() os.remove(session.scriptPath) end)
    pcall(function() os.remove(session.metaPath) end)
  end
  return 'OK:0'
end

-- CE's AA editor checks scripts with autoassemble2(..., syntaxcheckonly=true),
-- which reports 'Error in line %s (%s) :%s' (rsErrorInLine in
-- autoassembler.pas) and also parses {$lua}/{$ccode} blocks.  Its Lua mode
-- checks with luaL_loadstring.  Reproduce both paths so Notepad3 can show CE's
-- real verdict including the line number, not just local heuristics.
local function parseCheckError(msg)
  msg = tostring(msg or '')
  local line = tonumber(msg:match('[Ee]rror in line%s*(%d+)')) or
               tonumber(msg:match('%]:(%d+):')) or 1
  local text = msg:match('%)%s*:%s*(.*)$') or msg
  text = text:gsub('^%s+', '')
  if text == '' then text = msg end
  return line, text
end

-- Returns "OK:<count>\n<line>|<e|w>|<message>..." for CMD_CHECK_SCRIPT.
local function checkScript(text)
  if type(text) ~= 'string' or text == '' then return 'ERR:empty script' end
  local upper = text:upper()
  local hasEnable = upper:find('[ENABLE]', 1, true) ~= nil
  local hasDisable = upper:find('[DISABLE]', 1, true) ~= nil
  local records = {}
  local function add(line, sev, msg)
    msg = tostring(msg or ''):gsub('[\r\n]', ' ')
    records[#records + 1] = tostring(line) .. '|' .. sev .. '|' .. msg
  end

  if hasEnable or hasDisable then
    if type(autoAssembleCheck) ~= 'function' then
      return 'ERR:CE has no autoAssembleCheck'
    end
    for _, enable in ipairs({ true, false }) do
      if enable or hasDisable then
        local ok, res, detail = pcall(autoAssembleCheck, text, enable)
        if not ok or res ~= true then
          local raw = not ok and tostring(res) or tostring(detail or res)
          local ln, msg = parseCheckError(raw)
          add(ln, 'e', (enable and '[ENABLE] ' or '[DISABLE] ') .. msg)
        end
      end
    end
  else
    local chunk, err = load(text, '=check', 't')
    if not chunk then
      local ln, msg = parseCheckError(tostring(err))
      add(ln, 'e', 'Lua: ' .. msg)
    end
  end
  pcall(_monoEnsure)
  return 'OK:' .. tostring(#records) .. '\n' .. table.concat(records, '\n')
end

np3.checkScript = checkScript

-- The AA detection entry is shared by every path that can disturb mono.  Set
-- after the mono module is defined; runScript calls it through this field so
-- the earlier definition site needs no forward declaration.
bridge.monoEnsure = _monoEnsure

np3.applySession = function(payload)
  local result
  local ok, err = pcall(function()
    onMainThread(function()
      result = applyScript(payload)
      pcall(_monoEnsure)
    end)
  end)
  if not ok then return 'ERR:' .. tostring(err) end
  return result or 'ERR:no result'
end

np3.monoState = function()
  local s = _monoSnapshot()
  s.ever = bridge.monoRecover.ever
  s.attempts = bridge.monoRecover.attempts
  s.nextAt = bridge.monoRecover.nextAt
  s.lastReason = bridge.monoRecover.lastReason
  return s
end

np3.monoRestore = function()
  bridge.monoRecover.attempts = 0
  bridge.monoRecover.nextAt = 0
  _monoEnsure()
  return 'requested'
end

np3.selftest = function()
  local lines = { 'bridge=' .. np3.version }
  local missing = {}
  for name, present in pairs(np3.capabilities()) do
    if not present then missing[#missing + 1] = name end
  end
  table.sort(missing)
  lines[#lines + 1] = 'capsMissing=' .. table.concat(missing, ',')
  lines[#lines + 1] = 'menu=' .. tostring(bridge.menuInstalled) ..
                      ' items=' .. tostring(bridge.menuCount or 0)
  lines[#lines + 1] = 'editorHook=' .. tostring(bridge.editorHookInstalled) ..
                      ' editLock=none (CE delete/更改脚本 stay enabled)'
  lines[#lines + 1] = 'factory=' .. tostring(bridge.factory ~= nil)
  lines[#lines + 1] = 'editor=' .. tostring(editorPath() or 'missing')
  local ms = _monoSnapshot()
  lines[#lines + 1] = 'mono=wanted=' .. tostring(ms.wanted) ..
                      ' attached=' .. tostring(ms.attached) ..
                      ' connected=' .. tostring(ms.connected) ..
                      ' failed=' .. tostring(ms.failed) ..
                      ' attempts=' .. tostring(bridge.monoRecover.attempts)
  lines[#lines + 1] = 'ctFramework=' .. tostring(bridge.ctFramework) ..
                      ' ctFrameworkItem=' .. tostring(bridge.ctFrameworkItem ~= nil) ..
                      ' runtimeHook=' .. tostring(bridge.runtimeHook)
  lines[#lines + 1] = 'userTemplates=' ..
                      tostring(bridge.userTemplates and #bridge.userTemplates.templates or 0) ..
                      ' method=' .. tostring(bridge.templateMethod) ..
                      ' precheck=' .. tostring(bridge.precheckTemplates) ..
                      ' lastPrecheck=' .. tostring(bridge.lastPrecheck) ..
                      ' file=' .. tostring(userTemplatePath()) ..
                      ' checkApi=' .. tostring(type(autoAssembleCheck) == 'function')
  for _, e in ipairs(bridge.templateErrors or {}) do
    lines[#lines + 1] = '  templateWarning: ' .. tostring(e)
  end
  local apis = { 'getMemoryViewForm', 'createAutoAssemblerForm',
                 'generateCodeInjectionScript', 'getAddressList',
                 'shellExecute', 'vtAutoAssembler' }
  for _, name in ipairs(apis) do
    lines[#lines + 1] = name .. '=' .. tostring(type(_G[name]) ~= 'nil')
  end
  -- Do not create the AA form here: selftest must not cause a window flash.
  local tpl = getTemplateMenu(bridge.factory)
  lines[#lines + 1] = 'templateItems=' .. tostring(tpl and tpl.Count or 0)
  if tpl then
    for i = 0, tpl.Count - 1 do
      local it = tpl.getItem(i)
      local cap = itemCaption(it)
      local key = customOptionKey(menuName(it), cap)
      lines[#lines + 1] = '  ' .. tostring(i) .. ': ' .. cap ..
                          ' [' .. menuName(it) .. '] option=' ..
                          tostring(key or '-') .. ' checked=' ..
                          tostring(menuChecked(it))
    end
  end
  return table.concat(lines, '\n')
end

-- ---------------------------------------------------------------------------
-- Connection handling
-- ---------------------------------------------------------------------------

local function handleConnection(pipe)
  sendFrame(pipe, KIND_INFO, 'CE bridge ready (Lua ' .. (_VERSION or '?') .. ')')
  while bridge.running do
    local cmd, payload = readFrame(pipe)
    if cmd == nil then break end

    if cmd == CMD_PING then
      sendFrame(pipe, KIND_RESULT, 'pong')
      sendFrame(pipe, KIND_DONE, '0')
    elseif cmd == CMD_RUN then
      local ok, err = pcall(executeOnMainThread, pipe, payload)
      if not ok then
        sendFrame(pipe, KIND_ERROR, 'NP3 bridge: ' .. tostring(err))
        sendFrame(pipe, KIND_DONE, '1')
      end
    elseif cmd == CMD_APPLY_SCRIPT then
      local result
      local ok, err = pcall(function()
        onMainThread(function() result = applyScript(payload) end)
      end)
      sendFrame(pipe, KIND_RESULT, ok and (result or 'ERR:no result') or
                                       ('ERR:' .. tostring(err)))
      sendFrame(pipe, KIND_DONE, ok and '0' or '1')
    elseif cmd == CMD_CLOSE_SESSION then
      -- No CE objects are touched here any more (temp-file cleanup only), so
      -- run it directly instead of waiting for CE's main thread: the close
      -- frame is what releases the session, and it must not depend on a
      -- responsive main thread (notably while Notepad3 is shutting down).
      local result
      local ok, err = pcall(function() result = closeSession(payload) end)
      sendFrame(pipe, KIND_RESULT, ok and (result or 'OK:0') or
                                       ('ERR:' .. tostring(err)))
      sendFrame(pipe, KIND_DONE, ok and '0' or '1')
    elseif cmd == CMD_CHECK_SCRIPT then
      local result
      local ok, err = pcall(function()
        onMainThread(function() result = checkScript(payload) end)
      end)
      sendFrame(pipe, KIND_RESULT, ok and (result or 'ERR:no result') or
                                       ('ERR:' .. tostring(err)))
      sendFrame(pipe, KIND_DONE, ok and '0' or '1')
    elseif cmd == CMD_SYNC_SCRIPT then
      local result
      local ok, err = pcall(function()
        onMainThread(function() result = syncSessionScript(payload) end)
      end)
      sendFrame(pipe, KIND_RESULT, ok and (result or 'ERR:no result') or
                                       ('ERR:' .. tostring(err)))
      sendFrame(pipe, KIND_DONE, ok and '0' or '1')
    elseif cmd == CMD_CLOSE then
      break
    else
      sendFrame(pipe, KIND_ERROR, 'NP3 bridge: unknown command ' .. tostring(cmd))
      sendFrame(pipe, KIND_DONE, '1')
    end
  end
  if type(pipe.destroy) == 'function' then pcall(function() pipe.destroy() end) end
end

-- ---------------------------------------------------------------------------
-- Server startup
-- ---------------------------------------------------------------------------

-- A missing CE API must stop only the bridge, never CE's autorun loader.
-- Keep the reason where diagnostics can read it and print it once in CE's Lua
-- window; otherwise the failure looks like "nothing happened".
local function reportStartupFailure(reason)
  bridge.running = false
  bridge.startupError = reason
  print('NP3 CE bridge: ' .. reason)
end

if type(createPipe) ~= 'function' then
  reportStartupFailure('createPipe is unavailable in this CE version')
  return
end
local okPipe, server = pcall(createPipe, PIPE_NAME, 65536, 65536, 4)
if not okPipe or not server or not server.valid then
  reportStartupFailure('failed to create pipe ' .. PIPE_NAME ..
                       (okPipe and '' or (': ' .. tostring(server))))
  return
end

if type(createThread) ~= 'function' then
  reportStartupFailure('createThread is unavailable in this CE version')
  return
end

bridge.server = server
bridge.running = true
bridge.startupError = nil

-- Create timers defensively: old CE builds may lack createTimer or return an
-- object without the expected methods.  Losing a timer must not kill startup.
local function newTimer(interval, callback)
  if type(createTimer) ~= 'function' then return nil end
  local okCreate, timer = pcall(createTimer, nil, false)
  if not okCreate or not timer then return nil end
  local okSetup = pcall(function()
    timer.Interval = interval
    timer.OnTimer = callback
    timer.Enabled = true
  end)
  if not okSetup then
    pcall(function() timer.destroy() end)
    return nil
  end
  return timer
end

-- Lazarus/CE pumps synchronize() from its main loop; a timer makes sure queued
-- calls are also processed while the main loop is busy.
if type(checkSynchronize) == 'function' then
  bridge.timer = newTimer(25, function() pcall(checkSynchronize, 0) end)
end

-- Forms/menus are not ready while autorun scripts load.  Retry the UI
-- integration a few times on the main thread, then keep whatever succeeded.
local initTimer
initTimer = newTimer(500, function()
  local attempts = (bridge.initAttempts or 0) + 1
  bridge.initAttempts = attempts
  pcall(installMenu)
  pcall(installEditorHook)
  if (bridge.menuInstalled and bridge.editorHookInstalled) or attempts > 20 then
    pcall(function()
      initTimer.Enabled = false
      initTimer.destroy()
    end)
  end
end)
bridge.initTimer = initTimer

-- Low-frequency watchdog: detect Mono lost by CE's own AA editor or other
-- non-bridge paths.  The timer callback runs on CE's main thread; the backoff
-- in _monoEnsure keeps a failing LaunchMonoDataCollector from being retried
-- on every tick (it may pop a modal dialog and block the bridge).
bridge.monoWatch = newTimer(2000, function()
  if bridge.running then pcall(_monoEnsure) end
end)

local function listener()
  while bridge.running do
    local okAccept, pipe = pcall(server.acceptConnection, true)
    if okAccept and pipe then
      pcall(handleConnection, pipe)
    elseif type(sleep) == 'function' then
      -- A broken pipe must not tear down the listener thread.
      pcall(sleep, 100)
    end
  end
end

local okThread, thread = pcall(createThread, listener)
if not okThread or not thread then
  for _, timer in ipairs({ bridge.timer, bridge.initTimer, bridge.monoWatch }) do
    if timer then
      pcall(function() timer.Enabled = false; timer.destroy() end)
    end
  end
  reportStartupFailure('cannot start CE bridge listener: ' .. tostring(thread))
  return
end
bridge.thread = thread
if bridge.echoToCe then
  print('NP3 CE bridge: listening on ' .. PIPE_NAME)
end
