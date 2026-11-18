# encoding: UTF-8
# Bridge script self-check: required integration points + real Lua 5.3 syntax check.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$luaPath = Join-Path $root 'ce_bridge\np3_ce_bridge.lua'
$luaDll = 'E:\Cheat Engine\Cheat Engine 7.7\lua53-64.dll'

$text = Get-Content -Raw -Encoding UTF8 $luaPath
$required = @(
    'CMD_APPLY_SCRIPT', 'CMD_CLOSE_SESSION', 'CMD_CHECK_SCRIPT', 'CMD_SYNC_SCRIPT',
    'miNP3AutoAssemble', 'miNP3DefaultTemplates', 'parseUserTemplates',
    'renderUserTemplate', 'collectSelection', 'generateDefaultTemplate', 'checkScript',
    'autoAssembleCheck', 'parseCheckError',
    'debuggerpopup', 'createAutoAssemblerForm',
    'OnAutoAssemblerEdit', 'createMemoryRecord', 'getMemoryRecordByID',
    'np3.selftest', 'np3.applySession'
)
$missing = @($required | Where-Object { -not $text.Contains($_) })
if ($missing.Count -gt 0) {
    Write-Host ('FAIL: bridge Lua is missing: ' + ($missing -join ', '))
    exit 1
}

# Regression: CE passes shellExecute's parameters as one argument.  Extra
# "/utf8 /crlf" switches made Notepad3 treat the whole string as a filename
# ("加载 \utf8 \crlf ... 时遇到错误").  Encoding/EOL must come from the file.
if ($text -match '/utf8|/crlf') {
    Write-Host 'FAIL: session launcher still uses /utf8 or /crlf switches'
    exit 1
}
# Even a single quoted path can be treated as a literal file name by the
# editor's command-line parser, so pass a bare path whenever possible.
if (-not $text.Contains('local params = scriptPath')) {
    Write-Host 'FAIL: session launcher must pass a bare path (no switches, no quotes)'
    exit 1
}
if (-not $text.Contains("scriptFile:write('\239\187\191')")) {
    Write-Host 'FAIL: session script must carry a UTF-8 BOM for auto-detection'
    exit 1
}
# CE's 5/14-byte and 1-byte-exception JMP rows are options: keep them in the
# mirrored menu and mirror CE's checked/radio state so the selection is visible.
if ($text.Contains('and not isOption then')) {
    Write-Host 'FAIL: JMP option items must not be skipped'
    exit 1
}
if (-not $text.Contains('if isOption then')) {
    Write-Host 'FAIL: mirrored menu needs an option-item branch'
    exit 1
}
if (-not $text.Contains('syncOptionState')) {
    Write-Host 'FAIL: checked-state synchronization is missing'
    exit 1
}
if (-not $text.Contains('.Checked')) {
    Write-Host 'FAIL: checked-state handling is missing'
    exit 1
}
# Regression: CE's Lua doClick() only fires OnClick (menuItem_doClick in CE's
# LuaMenu.pas); the check/radio bookkeeping of TMenuItem.Click never runs.  The
# option rows must therefore set the state of the factory's item themselves,
# otherwise "14 byte JMP" / "1 byte exception JMP" never change anything and
# the generated script keeps CE's initial 5-byte JMP.
if (-not $text.Contains('applyOptionState')) {
    Write-Host 'FAIL: option rows do not apply their state'
    exit 1
}
if (-not $text.Contains('setMenuChecked')) {
    Write-Host 'FAIL: the checked state of the source item is never written'
    exit 1
}
$fnOpt = [regex]::Match($text, '(?ms)^local function applyOptionState\(.*?^end\s*$')
if (-not $fnOpt.Success) { Write-Host 'FAIL: applyOptionState not found'; exit 1 }
if (-not $fnOpt.Value.Contains('setMenuChecked')) {
    Write-Host 'FAIL: applyOptionState does not set the checked state'
    exit 1
}
if (-not $fnOpt.Value.Contains('other.radio')) {
    Write-Host 'FAIL: the radio group is not cleared when another row is picked'
    exit 1
}
# Regression: CE hides the JMP rows depending on the target process
# (TfrmAutoInject.emplate1Click).  The hidden factory's Template menu never
# opens, so the bridge has to apply that rule itself.
if (-not $text.Contains('targetSupportsFarJmp')) {
    Write-Host 'FAIL: the target architecture rule for the JMP rows is missing'
    exit 1
}
if (-not $text.Contains('targetSupportsJmp1')) {
    Write-Host 'FAIL: the target architecture rule for the 1-byte JMP is missing'
    exit 1
}
if (-not $text.Contains('pair.child.Visible')) {
    Write-Host 'FAIL: the visibility of the mirrored rows is not synchronized'
    exit 1
}
if (-not $text.Contains('jmp1OptionEnabled')) {
    Write-Host 'FAIL: the 1-byte exception JMP is not guarded against x64 targets'
    exit 1
}
# A failing menu action must be reported: the bridge's Lua window is not shown
# by default, so a silent failure looks like "nothing happened".
if (-not $text.Contains('reportError')) {
    Write-Host 'FAIL: menu failures are not reported'
    exit 1
}
if (-not $text.Contains('showMessage')) {
    Write-Host 'FAIL: menu failures are not shown in CE'
    exit 1
}
# CE's own row is localized ("Auto Assemble" / the Chinese caption), so the
# bridge's submenu must not use the same caption.  The check stays ASCII only:
# Windows PowerShell reads a BOM-less .ps1 with the ANSI code page, so a
# Chinese literal in this file would not match the UTF-8 script content.
if ($text -notmatch "root\.Caption = '[^']*\(Notepad3\)'") {
    Write-Host 'FAIL: the bridge submenu still collides with CE''s own caption'
    exit 1
}
# The runtime-hook row must not be a dead switch: it has to run CE's handler.
$fnMirror = [regex]::Match($text, '(?ms)^local function lazyBuildMirror\(.*?^end\s*$')
if (-not $fnMirror.Success) { Write-Host 'FAIL: lazyBuildMirror not found'; exit 1 }
if (-not $fnMirror.Value.Contains("custom == 'runtimeHook'")) {
    Write-Host 'FAIL: the runtime-hook row is not handled'
    exit 1
}
if ($text -notmatch "custom == 'runtimeHook' then[\s\S]{0,400}?generateAndEdit") {
    Write-Host 'FAIL: the runtime-hook row is a dead switch'
    exit 1
}
# The disassembler selection ends behind the last selected byte (CE's
# getSelectionSize = max - min), so the instruction at that address must not
# be part of the generated script: it would overwrite unselected bytes.
if (-not $text.Contains('addr < stop')) {
    Write-Host 'FAIL: the selection still covers the byte behind the selection'
    exit 1
}

# "CT 表框架代码" is a checked option (default on), not a generated script
# item.  Turning it off strips the [ENABLE]/[DISABLE] frame from templates.
if (-not $text.Contains('bridge.ctFramework = true')) {
    Write-Host 'FAIL: CT framework option default is missing'
    exit 1
}
if (-not $text.Contains('stripCtFramework')) {
    Write-Host 'FAIL: CT framework stripping is missing'
    exit 1
}
if (-not $text.Contains('customOptionKey')) {
    Write-Host 'FAIL: CT framework option detection is missing'
    exit 1
}
if (-not $text.Contains('CheatTablecompliantcodee1')) {
    Write-Host 'FAIL: CE framework-code menu item is not handled'
    exit 1
}
# CE only emits the dealloc/db disable section when [ENABLE]/[DISABLE] already
# exist.  With the option on, invoke CE's own framework item BEFORE the
# template; with it off, strip frames added by templates that insert their own.
if (-not $text.Contains('ctFrameworkItem = src')) {
    Write-Host 'FAIL: CE framework menu item is not captured'
    exit 1
}
if (-not $text.Contains('bridge.ctFrameworkItem.doClick()')) {
    Write-Host 'FAIL: framework is not pre-seeded before template generation'
    exit 1
}
# "运行时 Hook 脚本生成" is also an option, and every option shows its state
# as a leading check mark in the caption.
if (-not $text.Contains('miRuntimeHooking')) {
    Write-Host 'FAIL: runtime-hook option is not detected'
    exit 1
}
if (-not $text.Contains('bridge.runtimeHook')) {
    Write-Host 'FAIL: runtime-hook option state is missing'
    exit 1
}
if (-not $text.Contains('✓ ')) {
    Write-Host 'FAIL: unified check-mark state display is missing'
    exit 1
}

# "默认模板" submenu: nop / ret 1 / jmp / cmp, generated from the selected
# disassembly range instead of CE's built-in Template menu.
foreach ($needle in @('miNP3DefaultTemplates', 'generateDefaultTemplate',
                       'CMD_CHECK_SCRIPT', 'autoAssembleCheck', 'parseCheckError')) {
    if (-not $text.Contains($needle)) {
        Write-Host ('FAIL: default templates / online check missing: ' + $needle)
        exit 1
    }
}

# User-editable templates: a sandbox-loaded .tpl file, a pure renderer, hot
# reload, AOB by default with a switchable method, and no startup AA window.
$tplPath = Join-Path $root 'ce_bridge\np3_user_templates.tpl'
if (-not (Test-Path -LiteralPath $tplPath)) {
    Write-Host 'FAIL: user-editable template file is missing'
    exit 1
}
$tplText = Get-Content -Raw -Encoding UTF8 $tplPath
foreach ($needle in @('parseUserTemplates', 'renderUserTemplate',
                       'loadUserTemplates', 'np3.reloadTemplates',
                       'np3.setTemplateMethod', 'np3.setTemplatePrecheck',
                       'CE 预检', 'createAutoAssemblerForm')) {
    if (-not $text.Contains($needle)) {
        Write-Host ('FAIL: user-template integration missing: ' + $needle)
        exit 1
    }
}
if ($text.Contains('nop - 注释选中代码并直接返回原函数')) {
    Write-Host 'FAIL: default template captions still contain descriptions'
    exit 1
}
foreach ($cap in @("name = 'nop'", "name = 'ret 1'", "name = 'jmp'", "name = 'cmp'")) {
    if (-not $tplText.Contains($cap)) {
        Write-Host ('FAIL: plain template name missing: ' + $cap)
        exit 1
    }
}
if (-not $tplText.Contains('aob')) {
    Write-Host 'FAIL: AOB default method is missing from the template file'
    exit 1
}
# CE startup must not create/show the hidden TfrmAutoInject factory.
$installFn = [regex]::Match($text, '(?ms)^local function installMenu\(.*?^end\s*$')
if (-not $installFn.Success) { Write-Host 'FAIL: installMenu not found'; exit 1 }
if ($installFn.Value.Contains('ensureFactory')) {
    Write-Host 'FAIL: startup still creates the AA form (visible flash)'
    exit 1
}
if (-not $text.Contains('form.Visible = false')) {
    Write-Host 'FAIL: factory form is not explicitly hidden'
    exit 1
}
# CE's own script editor must not be blocked: only the bridge's own template
# factory may be hidden, never every TfrmAutoInject that CE creates.
if ($text -notmatch "bridge\.creatingFactory and candidate and\s*\r?\n\s*tostring\(candidate\.ClassName") {
    Write-Host 'FAIL: TfrmAutoInject hiding is not guarded by the factory flag'
    exit 1
}
$factoryFn = [regex]::Match($text, '(?ms)^local function ensureFactory\(.*?^end\s*$')
if (-not $factoryFn.Success) { Write-Host 'FAIL: ensureFactory not found'; exit 1 }
if (-not $factoryFn.Value.Contains('bridge.creatingFactory')) {
    Write-Host 'FAIL: AA form hiding is not scoped to the factory instance'
    exit 1
}
# CE's createAutoAssemblerForm() calls Show after the add-notification, so
# hiding from the notification alone still flashes one frame.  The factory
# must be parked off-screen (poDesigned) and hidden from OnShow as well.
foreach ($needle in @(
    'hideFactoryForm(candidate, true)', 'candidate.OnShow = function(sender)',
    'hideFactoryForm(sender, true)', 'hideFactoryForm(form, true)',
    'form.AlphaBlend = true', 'form.AlphaBlendValue = 0',
    'form.Width = 1', 'form.Height = 1',
    'form.Position = 0', 'form.Left = -32000', 'form.Top = -32000')) {
    if (-not $text.Contains($needle)) {
        Write-Host ('FAIL: AA factory no-flash guard missing: ' + $needle)
        exit 1
    }
}
# Double-clicking an existing AA record must still open Notepad3.
if (-not $text.Contains('installEditorHook')) {
    Write-Host 'FAIL: AA record double-click no longer routes to Notepad3'
    exit 1
}
if ($text -notmatch 'al\.OnAutoAssemblerEdit\s*=') {
    Write-Host 'FAIL: AddressList.OnAutoAssemblerEdit is not taken over'
    exit 1
}
# ...but the hook must fall back to CE's own editor when Notepad3 is unusable.
$hookFn = [regex]::Match($text, '(?ms)^local function installEditorHook\(.*?^end\s*$')
if (-not $hookFn.Success) { Write-Host 'FAIL: installEditorHook not found'; exit 1 }
if (-not $hookFn.Value.Contains('return false')) {
    Write-Host 'FAIL: editor hook must fall back to CE editor on failure'
    exit 1
}
# The bridge must NOT take CE's edit lock (mr.beginEdit).  CE's own sources make
# that lock fatal: while memrec.isBeingEdited, MainUnit.AddressListAutoAssemblerEdit
# only re-shows memrec.autoAssembleWindow (nil here, because the double click is
# intercepted), so CE's "更改脚本" would silently do nothing; and
# addresslist.deleteSelected skips isBeingEdited records, so the record could
# never be deleted.  A lock taken here also had to be released by a close frame
# that races with Notepad3's exit, so a lost frame left it locked forever.
if ($text -match '\bmr\.beginEdit\s*\(') {
    Write-Host 'FAIL: bridge takes CE''s edit lock (blocks delete and 更改脚本)'
    exit 1
}
# ...and it must not cache the record object either: the record may be deleted
# in CE while it is open in Notepad3, and a cached pointer would be a
# use-after-free.  The record is resolved by ID on demand instead.
if ($text -match 'session\.mr\s*=') {
    Write-Host 'FAIL: bridge caches the CE record object (dangling pointer risk)'
    exit 1
}
if ($text -match 'local\s+mr\s*=\s*session\.mr') {
    Write-Host 'FAIL: syncSessionScript uses the cached record object'
    exit 1
}
# The right-click "自动汇编" path must still launch Notepad3.
$genFn = [regex]::Match($text, '(?ms)^local function generateAndEdit\(.*?^end\s*$')
if (-not $genFn.Success) { Write-Host 'FAIL: generateAndEdit not found'; exit 1 }
if (-not $genFn.Value.Contains('newSession')) {
    Write-Host 'FAIL: right-click 自动汇编 no longer opens Notepad3'
    exit 1
}
# Generated template code must ask the editor for a one-time format; without it
# the freshly opened script only looks tidy after its first save.  A record's
# own script must never carry that request: opening it may not rewrite the text
# CE stores.
if (-not $text.Contains("f:write('format='")) {
    Write-Host 'FAIL: writeMeta does not emit the format request'
    exit 1
}
if (-not $genFn.Value.Contains('newSession(text, caption, 0, nil, true)')) {
    Write-Host 'FAIL: mirrored CE template code is not formatted on open'
    exit 1
}
$defFn = [regex]::Match($text, '(?ms)^local function generateDefaultTemplate\(.*?^end\s*$')
if (-not $defFn.Success -or -not $defFn.Value.Contains(', 0, nil, true)')) {
    Write-Host 'FAIL: default template code is not formatted on open'
    exit 1
}
$hookFn = [regex]::Match($text, '(?ms)^local function installEditorHook\(.*?^end\s*$')
if (-not $hookFn.Success -or
    $hookFn.Value.Contains('newSession(script, label, mr.ID, label, true)')) {
    Write-Host 'FAIL: opening a record must not reformat its stored script'
    exit 1
}
if ($tplText.Contains('==]')) {
    Write-Host 'FAIL: template file collides with the Lua long-bracket delimiter'
    exit 1
}
# Precheck findings must never be injected as "//" comments into the script:
# the header before [ENABLE] is parsed as Lua, where "//" is an operator.
if ($text.Contains('text = table.concat(notes')) {
    Write-Host 'FAIL: precheck comments are injected into the generated script'
    exit 1
}
if (-not $text.Contains('bridge.lastPrecheck = tonumber')) {
    Write-Host 'FAIL: precheck result is not retained without polluting the script'
    exit 1
}

if (-not (Test-Path $luaDll)) { Write-Host 'FAIL: lua53-64.dll not found'; exit 1 }
$dllLiteral = $luaDll.Replace('\', '\\')
$cs = (@'
using System;
using System.Runtime.InteropServices;
public static class Np3LuaSyntax {
    [DllImport("__NP3_LUA_DLL__", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr luaL_newstate();
    [DllImport("__NP3_LUA_DLL__", CallingConvention = CallingConvention.Cdecl)]
    private static extern int luaL_loadbufferx(IntPtr L, byte[] buff, UIntPtr sz, string name, string mode);
    [DllImport("__NP3_LUA_DLL__", CallingConvention = CallingConvention.Cdecl)]
    private static extern void luaL_openlibs(IntPtr L);
    [DllImport("__NP3_LUA_DLL__", CallingConvention = CallingConvention.Cdecl)]
    private static extern int lua_pcallk(IntPtr L, int nargs, int nresults, int errfunc, IntPtr ctx, IntPtr k);
    [DllImport("__NP3_LUA_DLL__", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr lua_tolstring(IntPtr L, int idx, out UIntPtr len);
    [DllImport("__NP3_LUA_DLL__", CallingConvention = CallingConvention.Cdecl)]
    private static extern void lua_close(IntPtr L);

    public static int Check(byte[] source) {
        IntPtr L = luaL_newstate();
        if (L == IntPtr.Zero) return -1;
        try {
            return luaL_loadbufferx(L, source, new UIntPtr((uint)source.Length), "=np3_ce_bridge.lua", null);
        }
        finally { lua_close(L); }
    }

    public static string Run(byte[] chunk) {
        IntPtr L = luaL_newstate();
        if (L == IntPtr.Zero) return "no Lua state";
        try {
            luaL_openlibs(L);
            int rc = luaL_loadbufferx(L, chunk, new UIntPtr((uint)chunk.Length), "=test", null);
            if (rc != 0) return "load error " + rc;
            rc = lua_pcallk(L, 0, -1, 0, IntPtr.Zero, IntPtr.Zero);
            if (rc != 0) {
                UIntPtr len;
                IntPtr msg = lua_tolstring(L, -1, out len);
                return "runtime error " + rc + ": " + Marshal.PtrToStringAnsi(msg);
            }
            return "OK";
        }
        finally { lua_close(L); }
    }
}
'@).Replace('__NP3_LUA_DLL__', $dllLiteral)
Add-Type -TypeDefinition $cs

$bytes = [System.IO.File]::ReadAllBytes($luaPath)
$rc = [Np3LuaSyntax]::Check($bytes)
if ($rc -ne 0) { Write-Host ('FAIL: Lua syntax check returned ' + $rc); exit 1 }

# Behavioural check with the exact two script shapes from the user's report:
# with the CT framework off, only the code between [ENABLE] and [DISABLE]
# remains (no dealloc/db disable section, no CE description comments).
$fn = [regex]::Match($text, '(?ms)^local function stripCtFramework\(.*?^end\s*$')
if (-not $fn.Success) { Write-Host 'FAIL: stripCtFramework function not found'; exit 1 }
$fnKey = [regex]::Match($text, '(?ms)^local function customOptionKey\(.*?^end\s*$')
if (-not $fnKey.Success) { Write-Host 'FAIL: customOptionKey function not found'; exit 1 }
$sampleLines = @(
    '[ENABLE]',
    "//从此处到'[DISABLE]'的代码将用于启用修改",
    'alloc(newmem,2048,thorium.exe+21DC70) ',
    'label(returnhere)',
    'label(originalcode)',
    'label(exit)',
    'newmem:',
    'originalcode:',
    'mov eax,eax',
    'xor [rsp+20],rax',
    'exit:',
    'jmp returnhere',
    'thorium.exe+21DC70:',
    'jmp newmem',
    'nop 2',
    'returnhere:',
    '',
    '',
    '[DISABLE]',
    '//从此处到代码末尾的代码将用于禁用修改',
    'dealloc(newmem)',
    'thorium.exe+21DC70:',
    'db 8B C0 48 31 44 24 20',
    '//mov eax,eax',
    '//xor [rsp+20],rax'
)
$sampleLua = 'local sample = "' + ($sampleLines -join '\r\n') + '"'
$testChunk = $fnKey.Value + "`n" + $fn.Value + "`n" + $sampleLua + "`n" + @'
assert(customOptionKey('CheatTablecompliantcodee1', 'CT表框架代码') == 'ctFramework', 'name miss')
assert(customOptionKey('x', 'CT 表框架代码' .. '\t' .. 'Ctrl+Alt+T') == 'ctFramework', 'localized caption miss')
assert(customOptionKey('x', 'Cheat Table Framework Code') == 'ctFramework', 'english caption miss')
assert(customOptionKey('miRuntimeHooking', '运行时 Hook 脚本生成') == 'runtimeHook', 'runtime hook name miss')
assert(customOptionKey('x', 'Runtime hook script generation') == 'runtimeHook', 'runtime hook caption miss')
assert(customOptionKey('Codeinjection1', '代码注入') == nil, 'generation item misclassified')
local out = stripCtFramework(sample)
assert(out:find('alloc(newmem', 1, true), 'missing alloc')
assert(out:find('[ENABLE]', 1, true) == nil, 'ENABLE kept')
assert(out:find('[DISABLE]', 1, true) == nil, 'DISABLE kept')
assert(out:find('dealloc', 1, true) == nil, 'dealloc kept')
assert(out:find('db 8B', 1, true) == nil, 'disable bytes kept')
assert(out:find('从此处到', 1, true) == nil, 'framework comment kept')
assert(out:find('returnhere:', 1, true), 'missing returnhere')
assert(out:match('returnhere:%s*$') ~= nil, 'body not trimmed')
local plain = 'alloc(newmem,2048)\r\nlabel(returnhere)\r\n'
assert(stripCtFramework(plain) == plain, 'plain script changed')
'@
$brc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($testChunk))
if ($brc -ne 'OK') { Write-Host ('FAIL: bridge Lua behaviour assertions: ' + $brc); exit 1 }

# --- Regression: saved edits must flow back to the CE memory record.  Sidecar
# memrecId used to be hardcoded 0, so C could never target the original record.
# The record is resolved by ID on every call: the bridge holds no CE edit lock,
# so the record may be deleted in CE while it is open in Notepad3.
if (-not $text.Contains("f:write('memrecId='")) {
    Write-Host 'FAIL: writeMeta does not emit the real memrecId'
    exit 1
}
if ($text.Contains("f:write('memrecId=0\\n')")) {
    Write-Host 'FAIL: writeMeta still hardcodes memrecId=0'
    exit 1
}
$fnSync = [regex]::Match($text, '(?ms)^local function syncSessionScript\(.*?^end\s*$')
if (-not $fnSync.Success) { Write-Host 'FAIL: syncSessionScript function not found'; exit 1 }
if (-not $fnSync.Value.Contains('getMemoryRecordByID')) {
    Write-Host 'FAIL: syncSessionScript must resolve the record by ID'
    exit 1
}
if ($fnSync.Value.Contains('endEdit')) {
    Write-Host 'FAIL: syncSessionScript must not touch the CE edit lock'
    exit 1
}
if (-not $fnSync.Value.Contains('mr.Script')) {
    Write-Host 'FAIL: syncSessionScript must update mr.Script with saved text'
    exit 1
}
if (-not $text.Contains('CMD_SYNC_SCRIPT')) {
    Write-Host 'FAIL: command dispatcher is missing CMD_SYNC_SCRIPT'
    exit 1
}

# Behavioural check of syncSessionScript with a fake session/record.  Verifies
# the saved script replaces mr.Script on the record that the sidecar memrecId
# points at (not a freshly created one), that no CE edit lock is taken or
# released, and that a record deleted in CE is reported instead of crashing.
$syncChunk = @'
bridge = {}
'@ + "`n" + $fnSync.Value + "`n" + @'
local calls = { scriptSets = 0, endEdits = 0, begins = 0, created = 0, lastScript = nil }
local mrStore = { ID = 4242 }
local fakeMr = setmetatable({}, {
  __index = function(_, k) return mrStore[k] end,
  __newindex = function(_, k, v)
    if k == 'Script' then calls.scriptSets = calls.scriptSets + 1; calls.lastScript = v end
    mrStore[k] = v
  end,
})
fakeMr.endEdit = function() calls.endEdits = calls.endEdits + 1 end
fakeMr.beginEdit = function() calls.begins = calls.begins + 1 end
local fakeSession = { memrecId = 4242 }
local freshSession = { memrecId = 0 }
vtAutoAssembler = 1
getAddressList = function()
  return { getMemoryRecordByID = function(id)
    if id == 4242 and not calls.deleted then return fakeMr end
    return nil
  end, createMemoryRecord = function() calls.created = calls.created + 1; return fakeMr end }
end
bridge.sessions = { ['sid-1'] = fakeSession, ['sid-2'] = freshSession }
local out = syncSessionScript('sid-1|4242\n[ENABLE]\nalloc(newmem,8)\n[DISABLE]\n')
assert(out == 'OK:4242', 'sync reply wrong: ' .. tostring(out))
assert(calls.scriptSets == 1, 'mr.Script not set exactly once')
assert(calls.lastScript and calls.lastScript:find('alloc(newmem,8)', 1, true), 'saved script not applied')
assert(calls.endEdits == 0, 'sync must not release a CE edit lock it never took')
assert(calls.begins == 0, 'sync must not take a CE edit lock')
assert(calls.created == 0, 'sync must not create a record when one exists')
-- A session that has no record yet (a freshly generated script) must get one
-- on its first save, and the new id has to be remembered for the next save.
local fresh = syncSessionScript('sid-2|0\n[ENABLE]\nalloc(newmem,8)\n[DISABLE]\n')
assert(fresh == 'OK:4242', 'memrecId=0 must create a record: ' .. tostring(fresh))
assert(calls.created == 1, 'memrecId=0 must create exactly one record')
assert(freshSession.memrecId == 4242, 'the new id must be remembered in the session')
assert(calls.scriptSets == 2, 'the created record must receive the saved script')
-- The user may delete the record in CE while it is open in Notepad3: the sync
-- must re-create it instead of losing the edit.
calls.deleted = true
local gone = syncSessionScript('sid-1|4242\n[ENABLE]\nalloc(newmem,8)\n[DISABLE]\n')
assert(gone == 'OK:4242', 'deleted record must be re-created: ' .. tostring(gone))
assert(calls.created == 2, 'deleted record must be re-created exactly once')
assert(calls.scriptSets == 3, 're-created record must receive the saved script')
local bad = syncSessionScript('missing|0\nx')
assert(bad and bad:sub(1, 3) == 'ERR', 'unknown session must return an error')
'@
$brc2 = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($syncChunk))
if ($brc2 -ne 'OK') { Write-Host ('FAIL: syncSessionScript behaviour: ' + $brc2); exit 1 }

# --- Call-chain analysis: the two in-memory tables and their two views.
# The functions are concatenated in source order: a `local function` is only
# visible to the code above it, so the declaration order in the bridge file is
# part of the contract.
$chainFns = @()
foreach ($name in @('chainAppendRegs', 'chainAppendStack', 'chainEvict',
                     'chainBuildChain', 'chainBuildStackView')) {
    $m = [regex]::Match($text, ('(?ms)^local function ' + $name + '\(.*?^end\s*$'))
    if (-not $m.Success) { Write-Host ('FAIL: chain function not found: ' + $name); exit 1 }
    $chainFns += [pscustomobject]@{ Index = $m.Index; Value = $m.Value }
}
$chainFns = $chainFns | Sort-Object Index | ForEach-Object { $_.Value }
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
assert(#chainBuildChain(state, 1) == 0, 'evicted hit must be gone')
assert(#state.stack <= 256 * 64, 'stack table must stay bounded: ' .. #state.stack)
'@
$chainRc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($chainChunk))
if ($chainRc -ne 'OK') { Write-Host ('FAIL: call-chain table assertions: ' + $chainRc); exit 1 }

# --- Call-chain analysis: stack scan and frame unwinding.
$scanFns = @()
foreach ($name in @('chainScanStack', 'chainMarkReturns', 'chainMarkByScan')) {
    $m = [regex]::Match($text, ('(?ms)^local function ' + $name + '\(.*?^end\s*$'))
    if (-not $m.Success) { Write-Host ('FAIL: chain scan function not found: ' + $name); exit 1 }
    $scanFns += [pscustomobject]@{ Index = $m.Index; Value = $m.Value }
}
$scanFns = $scanFns | Sort-Object Index | ForEach-Object { $_.Value }
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
  { slotAddr = 0x1000, offset = 0,   value = 0x1100 },
  { slotAddr = 0x1008, offset = 8,   value = 0x402000 },
  { slotAddr = 0x1010, offset = 16,  value = 0x403000 },
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

# --- Call-chain analysis: name resolution must spend its budget on the frames.
$resolveFn = [regex]::Match($text, '(?ms)^local function chainResolveSlots\(.*?^end\s*$')
if (-not $resolveFn.Success) { Write-Host 'FAIL: chainResolveSlots not found'; exit 1 }
$resolveChunk = $resolveFn.Value + "`n" + @'
local looked = {}
getNameFromAddress = function(addr)
  looked[#looked + 1] = addr
  return string.format('sym_%X', addr)
end
-- The frame sits far down the stack, behind 100 ordinary slots.  With a
-- budget of 2 the frame must still be resolved, otherwise the call chain
-- would show the frame without its name.
local slots = {}
for i = 1, 100 do slots[i] = { slotAddr = i, offset = i * 8, value = 0x1000 + i, symbol = '', isRet = false } end
slots[100].isRet = true
slots[100].value = 0xABCDEF
chainResolveSlots(slots, 2)
assert(slots[100].symbol == 'sym_ABCDEF', 'the frame must be resolved first: ' .. tostring(slots[100].symbol))
assert(#looked == 2, 'the resolution budget must be honoured: ' .. #looked)
assert(looked[1] == 0xABCDEF, 'the frame must be the first lookup')
'@
$resolveRc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($resolveChunk))
if ($resolveRc -ne 'OK') { Write-Host ('FAIL: call-chain resolve assertions: ' + $resolveRc); exit 1 }

# --- Call-chain analysis: the wiring that talks to CE.
foreach ($needle in @('chainOnHit', 'chainStart', 'chainStop', 'chainResolveSlots',
                       'chainSelectedAddress', 'np3.chainStart', 'np3.chainStop',
                       'np3.chainDump', 'debug_continueFromBreakpoint',
                       'CHAIN_MAX_FRAMES')) {
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
# Reloading the bridge script must not leave the previous breakpoint behind.
if (-not $text.Contains('if bridge.chain and bridge.chain.bpId then')) {
    Write-Host 'FAIL: reloading the bridge leaves the old call-chain breakpoint behind'
    exit 1
}
# The capture must never let an exception escape into CE's debugger.
if (-not $text.Contains("logEvent('chain error: '")) {
    Write-Host 'FAIL: breakpoint errors are not trapped'
    exit 1
}
# The target must be continued even when the capture failed.
if (-not $text.Contains('debug_continueFromBreakpoint(co_run)')) {
    Write-Host 'FAIL: the target is not continued after a hit'
    exit 1
}

# --- Call-chain analysis: the menu toggle and the three-pane window.
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
# The three panes: registers on the left, call chain on the right, raw stack
# below.
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
# CE's ListView has no selection event, so the selection has to be polled.
if (-not $text.Contains('ItemIndex')) {
    Write-Host 'FAIL: the call-chain window does not follow the selected snapshot'
    exit 1
}
# Rebuilding the list drops the selection, so it has to be restored.
if (-not $text.Contains('state.lvRegs.ItemIndex = state.selSeq')) {
    Write-Host 'FAIL: rebuilding the register list drops the selected snapshot'
    exit 1
}
# Regression: the ListView must be a report view.  Without vsReport it stays an
# icon view and has no sub-items, so every row fails to fill.
if (-not $text.Contains("lv.ViewStyle = 'vsReport'")) {
    Write-Host 'FAIL: the call-chain panes are not in report view'
    exit 1
}
# Regression: a fresh item's SubItems is an empty list, so the values have to
# be appended.  Assigning them by index is out of range and aborts the refresh.
if ($text.Contains('SubItems[0] =')) {
    Write-Host 'FAIL: the call-chain panes assign sub-items by index'
    exit 1
}
if (-not $text.Contains('item.SubItems.add(')) {
    Write-Host 'FAIL: the call-chain panes do not append their sub-items'
    exit 1
}
# The column header is set through Caption, like CE's own list views.
if ($text.Contains('col.Text = ')) {
    Write-Host 'FAIL: the call-chain column headers do not use Caption'
    exit 1
}
# A window that fails to open must be reported, not swallowed.
if (-not $text.Contains('NP3: call-chain window: ')) {
    Write-Host 'FAIL: a failing call-chain window is not reported to the user'
    exit 1
}

# --- Call-chain analysis is temporarily disabled: it could hang CE.
# The feature is kept in the source for later diagnosis, but neither the
# menu entry nor chainStart() may activate it while the flag is false.
if (-not $text.Contains('local CHAIN_ENABLED = false')) {
    Write-Host 'FAIL: the call-chain feature is not disabled'
    exit 1
}
if (([regex]::Matches($text, 'if not CHAIN_ENABLED then')).Count -lt 1 -or
    ([regex]::Matches($text, 'if CHAIN_ENABLED then')).Count -lt 1) {
    Write-Host 'FAIL: chainStart or the menu entry is not gated by CHAIN_ENABLED'
    exit 1
}
$startFn = [regex]::Match($text, '(?ms)^local function chainStart\(.*?^end\s*$')
if (-not $startFn.Success) { Write-Host 'FAIL: chainStart not found'; exit 1 }
$disabledChunk = 'local CHAIN_ENABLED = false' + "`n" + 'bridge = {}' + "`n" +
                 $startFn.Value + "`n" + @'
debug_setBreakpoint = function()
  error('must not install a breakpoint while the feature is disabled')
end
local started, err = chainStart(0x401000)
assert(started == false, 'disabled chainStart must refuse to start')
assert(err and err:find('禁用', 1, true), 'disabled reason must be reported: ' .. tostring(err))
assert(bridge.chain == nil, 'disabled chainStart must not touch session state')
'@
$disabledRc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($disabledChunk))
if ($disabledRc -ne 'OK') { Write-Host ('FAIL: disabled call-chain assertions: ' + $disabledRc); exit 1 }

# Default-template behaviour: four user-editable specs from the real .tpl file,
# AOB by default, switchable to fixed-address, original code always commented.
$fnParse = [regex]::Match($text, '(?ms)^local function parseUserTemplates\(.*?^end\s*$')
if (-not $fnParse.Success) { Write-Host 'FAIL: parseUserTemplates not found'; exit 1 }
$fnRender = [regex]::Match($text, '(?ms)^local function renderUserTemplate\(.*?^end\s*$')
if (-not $fnRender.Success) { Write-Host 'FAIL: renderUserTemplate not found'; exit 1 }
$fnCheck = [regex]::Match($text, '(?ms)^local function parseCheckError\(.*?^end\s*$')
if (-not $fnCheck.Success) { Write-Host 'FAIL: parseCheckError not found'; exit 1 }
$tplLua = 'local tplSrc = [==[' + $tplText + ']==]'
$defaultChunk = $tplLua + "`n" + $fnParse.Value + "`n" + $fnRender.Value + "`n" + $fnCheck.Value + "`n" + @'
local info = {
  start = 0x401000,
  addressExpr = '"thorium.exe"+21DC70',
  lines = {
    { address = 0x401000, opcode = 'cmp', parameters = 'rax,rbx',
      text = 'cmp rax,rbx', bytes = { 0x48, 0x39, 0xD8 } },
    { address = 0x401003, opcode = 'mov', parameters = 'eax,eax',
      text = 'mov eax,eax', bytes = { 0x8B, 0xC0, 0x90, 0x90 } }
  },
  bytes = { 0x48, 0x39, 0xD8, 0x8B, 0xC0, 0x90, 0x90 }
}
local jumpInfo = {
  start = 0x401000,
  addressExpr = '"thorium.exe"+21DC70',
  lines = {
    { address = 0x401000, opcode = 'je', parameters = 'thorium.exe+21E000',
      text = 'je thorium.exe+21E000', isConditionalJump = true,
      bytes = { 0x0F, 0x84, 0x0A, 0x00, 0x00, 0x00, 0x90 } }
  },
  bytes = { 0x0F, 0x84, 0x0A, 0x00, 0x00, 0x00, 0x90 }
}
local parsed = parseUserTemplates(tplSrc)
assert(parsed, 'parseUserTemplates returned nil')
assert(parsed.method == 'aob', 'default method is not AOB: ' .. tostring(parsed.method))
assert(#parsed.templates == 4, 'expected 4 default templates')
assert(parsed.templates[1].name == 'nop', 'first name is not nop')
assert(parsed.templates[2].name == 'ret 1', 'second name is not ret 1')
assert(parsed.templates[3].name == 'jmp', 'third name is not jmp')
assert(parsed.templates[4].name == 'cmp', 'fourth name is not cmp')
for _, t in ipairs(parsed.templates) do
  assert(not t.name:find(' - ', 1, true), 'descriptive suffix remains: ' .. t.name)
end

local function hasBareOriginal(script)
  for line in (script .. '\n'):gmatch('([^\r\n]*)\r?\n') do
    if line:match('^%s*cmp%s+rax,rbx%s*$') then return true end
  end
  return false
end

local function hasTrailingBlank(script)
  for line in (script .. '\n'):gmatch('([^\r\n]*)\r?\n') do
    if line:match('%s$') then return true end
  end
  return false
end

local aob = renderUserTemplate(parsed.templates[1], info, 'aob')
assert(aob, 'AOB render failed')
assert(aob:find('aobscan', 1, true), 'AOB scan missing')
assert(aob:find('registersymbol(INJECT)', 1, true), 'AOB symbol not registered')
assert(aob:find('// 401000: cmp rax,rbx', 1, true), 'AOB original code not commented')
assert(not hasBareOriginal(aob), 'original code was left uncommented')
assert(aob:find('jmp returnhere', 1, true), 'no return to original function')
assert(aob:find('[DISABLE]', 1, true), 'no disable section')
assert(aob:find('db 48 39 D8 8B C0 90 90', 1, true), 'restore bytes missing')
assert(aob:find('\r\noriginalcode:\r\n', 1, true), 'originalcode label is not defined')
assert(not hasTrailingBlank(aob), 'trailing whitespace in generated script')
assert(not aob:find('\r\n\r\n\r\n', 1, true), 'blank run was not collapsed')
assert(aob:find('^%[ENABLE%]\r\n\r\n'), 'no blank line after [ENABLE]')
assert(aob:find('\r\n\r\n[DISABLE]', 1, true), 'no blank line before [DISABLE]')
assert(not aob:find('CE 预检', 1, true), 'precheck note leaked into generated script')

local byAddr = renderUserTemplate(parsed.templates[1], info, 'address')
assert(byAddr, 'address render failed')
assert(byAddr:find('aobscan', 1, true) == nil, 'address method still uses AOB')
assert(byAddr:find('"thorium.exe"+21DC70', 1, true), 'address expression missing')
assert(byAddr ~= aob, 'methods produce identical output')

local ret = renderUserTemplate(parsed.templates[2], info, 'aob')
assert(ret and ret:find('mov rax,1', 1, true), 'ret 1 missing mov rax,1')
assert(ret:find('\r\nret\r\n', 1, true), 'ret 1 missing ret')
assert(ret:find('// 401000: cmp rax,rbx', 1, true), 'ret 1 original not commented')
local jmp = renderUserTemplate(parsed.templates[3], jumpInfo, 'aob')
assert(jmp and jmp:find('jmp thorium.exe+21E000', 1, true), 'jmp target not preserved')
local cmpScript = renderUserTemplate(parsed.templates[4], info, 'aob')
assert(cmpScript and cmpScript:find('\r\ncmp rax,rax\r\n', 1, true), 'cmp not equalized')
assert(cmpScript:find('// 401000: cmp rax,rbx', 1, true), 'cmp original not commented')

-- A user template that forgets ${ORIGINAL_CODE} must still keep the original
-- instructions as comments ("never delete original code").
local bare = { name = 'bare', body = '[ENABLE]\nnewmem:\njmp returnhere\n[DISABLE]\n' }
local forced = renderUserTemplate(bare, info, 'aob')
assert(forced and forced:find('// 401000: cmp rax,rbx', 1, true),
       'original-code comment injection missing')

-- The .tpl is only deployed when absent, so an older default template that
-- declares label(originalcode) without defining it must still render cleanly.
local legacy = { name = 'legacy',
  body = '[ENABLE]\nlabel(originalcode)\nnewmem:\n${ORIGINAL_CODE}\njmp returnhere\n[DISABLE]\n' }
local upgraded = renderUserTemplate(legacy, info, 'aob')
assert(upgraded and upgraded:find('\r\noriginalcode:\r\n', 1, true),
       'legacy template did not get originalcode label')

-- Unknown placeholders must fail loudly instead of producing a broken script.
local bad, badErr = renderUserTemplate({ name = 'bad', body = '${NO_SUCH_VAR}' }, info, 'aob')
assert(bad == nil, 'unknown placeholder was not rejected')
assert(badErr and badErr:find('NO_SUCH_VAR', 1, true), 'error omits placeholder name')

-- The loader must be sandboxed: CE API calls in a template file cannot run.
local evil = parseUserTemplates('return { templates = { { name="x", body=getMemoryViewForm() } } }')
assert(evil == nil or #evil.templates == 0, 'sandbox allowed a CE API call')
-- Method is a setting that the user can change in the file.
local custom = parseUserTemplates('return { settings = { method = "address" }, templates = { { name = "only", body = "${ADDRESS}" } } }')
assert(custom and custom.method == 'address', 'method setting not honoured')
local only = renderUserTemplate(custom.templates[1], info, custom.method)
assert(only and only:find('"thorium.exe"+21DC70', 1, true), 'method setting not applied')

local ln, msg = parseCheckError('Error in line 12 (mov junk) :Unknown instruction')
assert(ln == 12, 'CE error line not parsed')
assert(msg == 'Unknown instruction', 'CE error message not parsed')
local ln2 = parseCheckError('[string "=check"]:7: unexpected symbol')
assert(ln2 == 7, 'Lua error line not parsed')
'@
$drc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($defaultChunk))
if ($drc -ne 'OK') { Write-Host ('FAIL: default template assertions: ' + $drc); exit 1 }

# Regression: CE's getNameFromAddress() already wraps the module part in
# quotes ('"game.exe"+3FDB').  collectSelection() must not add another pair,
# otherwise generated scripts show '""game.exe""+3FDB' and CE cannot assemble
# the alloc()/label() lines.  Exercise both the quoted and bare-name shapes.
$fnCollect = [regex]::Match($text, '(?ms)^local function collectSelection\(.*?^end\s*$')
if (-not $fnCollect.Success) { Write-Host 'FAIL: collectSelection not found'; exit 1 }
$collectChunk = $fnCollect.Value + "`n" + @'
local testedName = nil
getMemoryViewForm = function()
  return { DisassemblerView = { SelectedAddress = 0x401000, SelectedAddress2 = 0x401005 } }
end
createDisassembler = function()
  return {
    disassemble = function(addr) end,
    getLastDisassembleData = function()
      return { opcode = 'nop', parameters = '', bytes = { 0x90, 0x90, 0x90, 0x90, 0x90 } }
    end
  }
end
getNameFromAddress = function(addr)
  return testedName
end
testedName = '"cheatengine-x86_64-SSE4-AVX2.exe"+3FDB'
local quoted = collectSelection()
assert(quoted ~= nil, 'quoted module: no selection collected')
assert(quoted.addressExpr == '"cheatengine-x86_64-SSE4-AVX2.exe"+3FDB',
       'quoted module: addressExpr was double-quoted: ' .. quoted.addressExpr)
assert(not quoted.addressExpr:find('""', 1, true),
       'quoted module: found doubled quotes: ' .. quoted.addressExpr)
testedName = 'game.exe+401000'
local bare = collectSelection()
assert(bare ~= nil, 'bare module: no selection collected')
assert(bare.addressExpr == '"game.exe"+401000',
       'bare module: addressExpr not quoted: ' .. bare.addressExpr)
'@
$collectRc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($collectChunk))
if ($collectRc -ne 'OK') { Write-Host ('FAIL: collectSelection quoting assertions: ' + $collectRc); exit 1 }

# Menu option behaviour: CE's Lua doClick() never updates the checked state of
# a menu item, so the mirrored rows have to apply it themselves.  The JMP rows
# also follow CE's own visibility rule (5/14 byte JMP: 64-bit x86 target,
# 1-byte exception JMP: x86 target).
$menuFns = @()
foreach ($name in @('menuChecked', 'menuRadioItem', 'menuVisible', 'setMenuChecked',
                    'itemCaption', 'applyOptionState', 'targetSupportsFarJmp',
                    'targetSupportsJmp1', 'syncOptionState')) {
    $m = [regex]::Match($text, '(?ms)^local function ' + $name + '\(.*?^end\s*$')
    if (-not $m.Success) { Write-Host ('FAIL: menu helper not found: ' + $name); exit 1 }
    $menuFns += $m.Value
}
$menuChunk = @'
bridge = { menuPairs = {}, echoToCe = false }
'@ + "`n" + ($menuFns -join "`n") + "`n" + @'
local function fakeItem(name, caption, checked)
  return { Name = name, Caption = caption, Checked = checked }
end

local five = fakeItem('mi5ByteJMP', '5 Byte JMP (<2GB Distance)', true)
local fourteen = fakeItem('mi14ByteJMP', '14 Byte JMP (>2GB Distance)', false)
local exception = fakeItem('mi1ByteExceptionJMP', '1 Byte Exception JMP', false)
local childFive, childFourteen, childException = {}, {}, {}
bridge.menuPairs = {
  { src = five, child = childFive, name = 'mi5ByteJMP', isOption = true, radio = true },
  { src = fourteen, child = childFourteen, name = 'mi14ByteJMP', isOption = true, radio = true },
  { src = exception, child = childException, name = 'mi1ByteExceptionJMP',
    isOption = true, radio = false },
}

-- Picking the 14 byte JMP must select it and clear the 5 byte JMP.
applyOptionState(bridge.menuPairs[2])
assert(fourteen.Checked == true, '14 byte JMP was not selected')
assert(five.Checked == false, '5 byte JMP stayed selected')
-- The 1-byte exception JMP is a switch, not a radio row.
applyOptionState(bridge.menuPairs[3])
assert(exception.Checked == true, '1 byte exception JMP was not switched on')
applyOptionState(bridge.menuPairs[3])
assert(exception.Checked == false, '1 byte exception JMP was not switched off')

-- CE's visibility rule for the JMP rows.
targetIs64Bit = function() return true end
targetIsX86 = function() return true end
assert(targetSupportsFarJmp() == true, 'far JMP must be available on an x64 target')
assert(targetSupportsJmp1() == true, '1-byte JMP must be available on an x86 target')
targetIs64Bit = function() return false end
assert(targetSupportsFarJmp() == false, 'far JMP must be hidden on a 32 bit target')
targetIsX86 = function() return false end
assert(targetSupportsJmp1() == false, '1-byte JMP must be hidden on a non x86 target')

-- The mirrored rows follow that rule when the menu is opened.
targetIs64Bit = function() return true end
targetIsX86 = function() return true end
syncOptionState()
assert(childFive.Visible == true, '5 byte JMP row must be visible on an x64 target')
assert(childFourteen.Visible == true, '14 byte JMP row must be visible on an x64 target')
assert(childException.Visible == true, '1-byte JMP row must be visible on an x86 target')
targetIs64Bit = function() return false end
targetIsX86 = function() return false end
syncOptionState()
assert(childFive.Visible == false, '5 byte JMP row must be hidden on a 32 bit target')
assert(childFourteen.Visible == false, '14 byte JMP row must be hidden on a 32 bit target')
assert(childException.Visible == false, '1-byte JMP row must be hidden on a non x86 target')
'@
$mrc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($menuChunk))
if ($mrc -ne 'OK') { Write-Host ('FAIL: menu option assertions: ' + $mrc); exit 1 }

# Mono auto-restore: read-only snapshot + pure decision (Task 1).  These run in
# stock Lua with CE globals stubbed; all CE side effects stay in _monoEnsure.
$monoFns = @()
foreach ($name in @('menuChecked', 'menuVisible', '_monoSnapshot', '_monoRecoverDecision')) {
    $m = [regex]::Match($text, '(?ms)^local function ' + $name + '\(.*?^end\s*$')
    if (-not $m.Success) { Write-Host ('FAIL: mono helper not found: ' + $name); exit 1 }
    $monoFns += $m.Value
}
$monoChunk = @'
local sdValue = 0
local pipe = { Connected = true }
miMonoActivate = { Checked = true }
miMonoTopMenuItem = { Visible = true }
MainForm = { miMonoActivate = miMonoActivate }
getOpenedProcessID = function() return 7 end
getCurrentThreadID = function() return 7 end
isPaused = function() return false end
readInteger = function(a) if a == 'SD' then return sdValue end return 0 end
libmono = { monopipes = { [7] = pipe }, ProcessID = 7,
            MDC_ShuttingDownAddress = 'SD', fail = false, abort = false }
monopipe = pipe
'@ + "`n" + ($monoFns -join "`n") + "`n" + @'
local s = _monoSnapshot()
assert(s.wanted == true and s.attached == true and s.connected == true, 'healthy snapshot wrong')
assert(s.failed == false and s.paused == false and s.pid == 7 and s.target == true, 'snapshot flags wrong')
miMonoActivate.Checked = false
assert(_monoSnapshot().wanted == false, 'explicit close not seen')
miMonoActivate.Checked = true
sdValue = 1
assert(_monoSnapshot().failed == true, 'shutdown flag not seen')
sdValue = 0
monopipe = nil
libmono.monopipes = {}
assert(_monoSnapshot().attached == false, 'missing pipe still attached')

local function mk(over)
  local t = { wanted=true, attached=false, connected=false, failed=false,
              paused=false, pid=7, target=true }
  for k,v in pairs(over or {}) do t[k] = v end
  return t
end
local function rec(over)
  local r = { ever=true, attempts=0, nextAt=0, pid=7, wanted=true }
  for k,v in pairs(over or {}) do r[k] = v end
  return r
end
local healthy = mk({ attached=true, connected=true })
local d = _monoRecoverDecision(healthy, rec(), 1000)
assert(d.act == 'healthy' and d.setEver == true, 'healthy decision wrong')
local lost = mk()
assert(_monoRecoverDecision(lost, rec(), 1000).act == 'restore', 'lost not restoring')
assert(_monoRecoverDecision(lost, rec({ever=false}), 1000).reason == 'never-active', 'never-active')
assert(_monoRecoverDecision(mk({wanted=false}), rec(), 1000).reason == 'not-wanted', 'not-wanted')
assert(_monoRecoverDecision(mk({paused=true}), rec(), 1000).reason == 'paused', 'paused')
assert(_monoRecoverDecision(mk({target=false}), rec(), 1000).reason == 'not-target', 'not-target')
assert(_monoRecoverDecision(lost, rec({nextAt=1500}), 1000).reason == 'cooldown', 'cooldown')
assert(_monoRecoverDecision(lost, rec({attempts=3}), 1000).reason == 'exhausted', 'exhausted')
local pc = _monoRecoverDecision(lost, rec({attempts=3, pid=9}), 1000)
assert(pc.act == 'restore' and pc.resetPid == true, 'pid change not resetting')
assert(_monoRecoverDecision(mk({failed=true}), rec(), 1000).reason == 'failed-state', 'failed-state')
assert(_monoRecoverDecision(mk({attached=true}), rec(), 1000).reason == 'disconnected', 'disconnected')
-- Review: explicit close must not erase |ever| (reopen should still be recoverable)
assert(_monoRecoverDecision(mk({wanted=false}), rec(), 1000).setEver == nil,
       'not-wanted must not clear ever')
-- Review: target with unreadable Visible (nil) must be treated as non-target
miMonoTopMenuItem.Visible = nil
assert(_monoSnapshot().target == false, 'nil Visible must not count as mono target')
'@
$monoRc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($monoChunk))
if ($monoRc -ne 'OK') { Write-Host ('FAIL: mono snapshot/decision assertions: ' + $monoRc); exit 1 }

# Mono auto-restore: orchestration (Task 2).  Run _monoEnsure end-to-end with
# CE globals stubbed: it must restore once, honour explicit close, back off on
# failure, and never write to a nil notifyPipe.
$ensureFns = @()
foreach ($name in @('menuChecked', 'menuVisible', 'setMenuChecked', 'sendFrame',
                    '_monoSnapshot', '_monoRecoverDecision', '_monoLog', '_monoEnsure')) {
    $m = [regex]::Match($text, '(?ms)^local function ' + $name + '\(.*?^end\s*$')
    if (-not $m.Success) { Write-Host ('FAIL: mono ensure helper not found: ' + $name); exit 1 }
    $ensureFns += $m.Value
}
$ensureChunk = @'
KIND_OUTPUT = 0
local logText = ''
print = function(...) logText = logText .. tostring((...)) .. '\n' end
local launchCalls = 0
local pipe = { Connected = true }
miMonoActivate = { Checked = true }
miMonoTopMenuItem = { Visible = true }
MainForm = { miMonoActivate = miMonoActivate }
getOpenedProcessID = function() return 7 end
getCurrentThreadID = function() return 7 end
isPaused = function() return false end
readInteger = function(a) return 0 end
libmono = { monopipes = { [7] = pipe }, ProcessID = 7,
            MDC_ShuttingDownAddress = 'SD', fail = false, abort = false }
monopipe = pipe
inMainThread = function() return true end
synchronize = function() error('unexpected synchronize') end
LaunchMonoDataCollector = function()
  launchCalls = launchCalls + 1
  monopipe = { Connected = true }
  libmono.monopipes[7] = monopipe
  libmono.ProcessID = 7
  return true
end
bridge = { monoRecover = { ever=true, attempts=0, nextAt=0, lastReason='', pid=7 },
           echoToCe=false, monoEnsuring=false }
getProperty = function() return nil end
'@ + "`n" + ($ensureFns -join "`n") + "`n" + @'
-- healthy: no action
_monoEnsure()
assert(launchCalls == 0, 'healthy mono should not be touched')
-- lost + ever -> restore once; success restores Checked and clears backoff
monopipe = nil; libmono.monopipes = {}
libmono.ProcessID = 7
_monoEnsure()
assert(launchCalls == 1, 'restore not attempted')
assert(miMonoActivate.Checked == true, 'Checked not restored after success')
assert(bridge.monoRecover.attempts == 0, 'attempts not cleared on success')
assert(bridge.monoRecover.ever == true, 'ever not set')
-- user close -> no restore
miMonoActivate.Checked = false
monopipe = nil; libmono.monopipes = {}
bridge.monoRecover.nextAt = 0
_monoEnsure()
assert(launchCalls == 1, 'closed mono was re-enabled')
-- failure -> attempts++ and cooldown, logged
miMonoActivate.Checked = true
LaunchMonoDataCollector = function() launchCalls = launchCalls + 1; return 0 end
monopipe = nil; libmono.monopipes = {}
bridge.monoRecover = { ever=true, attempts=0, nextAt=0, lastReason='', pid=7 }
bridge.monoEnsuring = false
_monoEnsure()
assert(bridge.monoRecover.attempts == 1, 'failed attempt not counted')
assert(bridge.monoRecover.nextAt > 0, 'cooldown not set')
assert(logText:find('[MONO]', 1, true), 'no mono log line')
-- Review: truthy return but still disconnected must count as failure
LaunchMonoDataCollector = function() launchCalls = launchCalls + 1; return true end
monopipe = nil; libmono.monopipes = {}; libmono.ProcessID = 0
bridge.monoRecover = { ever=true, attempts=0, nextAt=0, lastReason='', pid=7 }
bridge.monoEnsuring = false
_monoEnsure()
assert(bridge.monoRecover.attempts == 1, 'truthy-but-disconnected not counted')
-- Review: three failures stop auto restore (exhausted)
LaunchMonoDataCollector = function() launchCalls = launchCalls + 1; return 0 end
bridge.monoRecover = { ever=true, attempts=3, nextAt=0, lastReason='', pid=7 }
bridge.monoEnsuring = false
local beforeExhaust = launchCalls
_monoEnsure()
assert(launchCalls == beforeExhaust, 'exhausted backoff still attempted restore')
-- Review: off-main-thread call must queue via synchronize, not restore inline
inMainThread = function() return false end
local syncCalls = 0
synchronize = function() syncCalls = syncCalls + 1 end
bridge.monoRecover = { ever=true, attempts=0, nextAt=0, lastReason='', pid=7 }
bridge.monoEnsuring = false
local beforeSync = launchCalls
_monoEnsure()
assert(syncCalls == 1, 'off-thread call did not synchronize')
assert(launchCalls == beforeSync, 'off-thread call restored inline')
inMainThread = function() return true end
'@
$ensureRc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($ensureChunk))
if ($ensureRc -ne 'OK') { Write-Host ('FAIL: mono ensure assertions: ' + $ensureRc); exit 1 }

# Mono auto-restore wiring (Task 3): the detection must actually be invoked by
# the AA code paths and the watchdog, plus manual entry points for diagnostics.
foreach ($needle in @('_monoSnapshot', '_monoRecoverDecision', '_monoEnsure',
                      'bridge.monoEnsure', 'np3.monoState', 'np3.monoRestore',
                      'bridge.monoRecover', '[MONO]')) {
    if (-not $text.Contains($needle)) {
        Write-Host ('FAIL: mono auto-restore missing: ' + $needle); exit 1
    }
}
# Review: watchdog writes must never share the command pipe (frame-protocol
# corruption).  _monoLog may only use print(), not sendFrame.
$logFn = [regex]::Match($text, '(?ms)^local function _monoLog\(.*?^end\s*$')
if (-not $logFn.Success -or $logFn.Value.Contains('sendFrame')) {
    Write-Host 'FAIL: _monoLog must not write to the shared pipe'; exit 1
}
if ($text.Contains('bridge.notifyPipe')) {
    Write-Host 'FAIL: notifyPipe must be removed (second pipe writer)'; exit 1
}
$runFn = [regex]::Match($text, '(?ms)^local function runScript\(.*?^end\s*$')
if (-not $runFn.Success -or -not $runFn.Value.Contains('pcall(bridge.monoEnsure)')) {
    Write-Host 'FAIL: runScript does not run mono ensure'; exit 1
}
if ($text -notmatch 'newTimer\(2000') {
    Write-Host 'FAIL: mono watchdog interval missing'; exit 1
}
if ($text -notmatch "mono=wanted=") {
    Write-Host 'FAIL: selftest does not report mono state'; exit 1
}

# Version compatibility: run the real bridge script in a stock Lua state while
# CE APIs that older versions may lack are absent.  The bridge must degrade
# instead of aborting its autorun load with "attempt to call a nil value".
$compatHook = @'
_G.print = function() end
_G.createPipe = function()
  return { valid = true, acceptConnection = function() return nil end }
end
_G.createTimer = function() return { destroy = function() end } end
_G.createThread = function() return {} end
_G.getCheatEngineDir = function() return '' end
_G.getOpenedProcessID = function() return 0 end
_G.debug_setBreakpoint = nil
local loaded = assert(load([===[__BRIDGE_SRC__]===], '=np3_ce_bridge'))
loaded()
assert(NP3_CE_BRIDGE.running == true, 'missing hook API must not stop the bridge')
local caps = np3.capabilities()
assert(caps.debug_setBreakpoint == false, 'missing hook API not reported')
assert(caps.createPipe == true, 'present pipe API not reported')
local ok, err = pcall(np3.hook, 4096, function() end)
assert(ok == false, 'np3.hook silently ignored a missing CE debugger API')
assert(tostring(err):find('unavailable in this CE version', 1, true),
       'unclear hook error: ' .. tostring(err))
local report = np3.selftest()
assert(report:find('capsMissing=', 1, true) and
       report:find('debug_setBreakpoint', 1, true),
       'selftest does not report the missing CE API')
'@
$compatNoPipe = @'
local printed = ''
_G.print = function(...) printed = printed .. tostring((...)) .. '\n' end
_G.createPipe = nil
_G.createTimer = function() return { destroy = function() end } end
_G.createThread = function() return {} end
local loaded = assert(load([===[__BRIDGE_SRC__]===], '=np3_ce_bridge'))
loaded()
assert(NP3_CE_BRIDGE.running == false, 'bridge runs with no pipe API')
assert(tostring(NP3_CE_BRIDGE.startupError):find('createPipe', 1, true),
       'startup failure does not name createPipe')
assert(printed:find('createPipe', 1, true), 'no actionable startup message')
'@
$compatNoThread = @'
local printed = ''
_G.print = function(...) printed = printed .. tostring((...)) .. '\n' end
_G.createPipe = function()
  return { valid = true, acceptConnection = function() return nil end }
end
_G.createTimer = function() return { destroy = function() end } end
_G.createThread = nil
local loaded = assert(load([===[__BRIDGE_SRC__]===], '=np3_ce_bridge'))
loaded()
assert(NP3_CE_BRIDGE.running == false, 'bridge runs with no thread API')
assert(NP3_CE_BRIDGE.initTimer == nil, 'timers started before the thread API was checked')
assert(tostring(NP3_CE_BRIDGE.startupError):find('createThread', 1, true),
       'startup failure does not name createThread')
assert(printed:find('createThread', 1, true), 'no actionable thread message')
'@
$compatNoTimer = @'
_G.print = function() end
_G.createPipe = function()
  return { valid = true, acceptConnection = function() return nil end }
end
_G.createTimer = function() error('no timer API in this CE version') end
_G.createThread = function() return {} end
local loaded = assert(load([===[__BRIDGE_SRC__]===], '=np3_ce_bridge'))
loaded()
assert(NP3_CE_BRIDGE.running == true, 'timer failure must not stop the bridge')
assert(NP3_CE_BRIDGE.initTimer == nil and NP3_CE_BRIDGE.monoWatch == nil,
       'failed timer creation was retained')
'@
foreach ($compatTemplate in @($compatHook, $compatNoPipe, $compatNoThread, $compatNoTimer)) {
    $chunkText = $compatTemplate.Replace('__BRIDGE_SRC__', $text)
    $compatRc = [Np3LuaSyntax]::Run([System.Text.Encoding]::UTF8.GetBytes($chunkText))
    if ($compatRc -ne 'OK') { Write-Host ('FAIL: CE version compatibility: ' + $compatRc); exit 1 }
}

Write-Host 'PASS: CE bridge Lua self-check'
