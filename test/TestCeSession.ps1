# encoding: UTF-8
# Focused harness: extract pure helpers from src/CeSession.c / src/CeBridge.c,
# compile TestCeSession.cpp and run it.  Mirrors TestCeBridge.ps1.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'src'
$session = Join-Path $src 'CeSession.c'
$bridge = Join-Path $src 'CeBridge.c'
$test = Join-Path $PSScriptRoot 'TestCeSession.cpp'

if (-not (Test-Path $session)) {
    Write-Host 'FAIL: src/CeSession.c is missing (CE session feature not implemented yet)'
    exit 1
}

function Get-Function([string]$path, [string]$signature) {
    $text = Get-Content -Raw -Encoding UTF8 $path
    $start = $text.IndexOf($signature)
    if ($start -lt 0) { throw "function not found: $signature in $path" }
    $open = $text.IndexOf('{', $start)
    if ($open -lt 0) { throw "opening brace not found: $signature" }
    $end = [regex]::Match($text.Substring($open), '(?m)^}').Index + $open + 1
    if ($end -le $open) { throw "closing brace not found: $signature" }
    return $text.Substring($start, $end - $start)
}

$sessionText = Get-Content -Raw -Encoding UTF8 $session
$bridgeText = Get-Content -Raw -Encoding UTF8 $bridge
$metaStruct = [regex]::Match($sessionText, '(?s)typedef struct _CE_SESSION_META\s*\{.*?\}\s*CE_SESSION_META;')
if (-not $metaStruct.Success) { throw 'CE_SESSION_META definition not found' }

# Regression: the apply request used to block the UI thread forever when CE's
# main-thread synchronize() was delayed.  Require bounded reads, a no-reply
# path for session close, and a worker thread in the session module.
if (-not $bridgeText.Contains('_ReadWithTimeout')) {
    Write-Host 'FAIL: bounded pipe reads are missing'
    exit 1
}
if (-not $bridgeText.Contains('CeBridge_SendNoReply')) {
    Write-Host 'FAIL: no-reply close-session path is missing'
    exit 1
}
if (-not $sessionText.Contains('_ApplyThread')) {
    Write-Host 'FAIL: apply request is not off the UI thread'
    exit 1
}
if (-not $sessionText.Contains('s_ceSessionApplyBusy')) {
    Write-Host 'FAIL: repeated-click guard is missing'
    exit 1
}
if (-not $sessionText.Contains('s_ceSessionApplyStart')) {
    Write-Host 'FAIL: stuck-worker watchdog is missing'
    exit 1
}
if (-not $sessionText.Contains('void CeSession_ReportResult(HWND hwnd)')) {
    Write-Host 'FAIL: deferred result reporting is missing'
    exit 1
}
# The toolbar button must not run deployment/INI work on the UI thread, session
# close must not touch the pipe on the UI thread, and no modal message box may
# block the editor while an apply request is in flight.
$np3Text = Get-Content -Raw -Encoding UTF8 (Join-Path $src 'Notepad3.c')
$caseBlock = [regex]::Match($np3Text, '(?s)case IDM_CE_ADD_LIST:.*?break;')
if (-not $caseBlock.Success) { throw 'IDM_CE_ADD_LIST case not found' }
if ($caseBlock.Value.Contains('EnsureDeployed')) {
    Write-Host 'FAIL: button handler still runs deployment on the UI thread'
    exit 1
}
# The action must be deferred out of WM_COMMAND, and the worker must never
# post UI messages carrying heap-allocated buffers (that reentered the output
# panel while the toolbar command was still on the stack).
if (-not $caseBlock.Value.Contains('WM_APP_CE_ADD_LIST')) {
    Write-Host 'FAIL: add-to-list is not deferred to its own message'
    exit 1
}
if (-not $np3Text.Contains('case WM_APP_CE_ADD_LIST:') -or
    -not $np3Text.Contains('CeSession_ReportResult')) {
    Write-Host 'FAIL: deferred request/result handlers are missing'
    exit 1
}
if (-not $sessionText.Contains('WM_APP_CE_ADD_RESULT')) {
    Write-Host 'FAIL: worker result message is missing'
    exit 1
}

# Regression: a successful save of a CE session must mirror the buffer back into
# the CE record.  Without this, re-opening from CE shows stale text and the
# beginEdit-locked record cannot be deleted (CE refuses silently).  The sync
# must run off the UI thread and must be silent (no result message).
$syncBridgeHdr = Get-Content -Raw -Encoding UTF8 (Join-Path $src 'CeBridge.h')
if (-not $syncBridgeHdr.Contains('CE_BRIDGE_CMD_SYNC_SCRIPT')) {
    Write-Host 'FAIL: CMD_SYNC_SCRIPT constant is missing in CeBridge.h'
    exit 1
}
if (-not $sessionText.Contains('void CeSession_SyncSavedScript(HWND hwnd)')) {
    Write-Host 'FAIL: CeSession_SyncSavedScript is not implemented'
    exit 1
}
if (-not $sessionText.Contains('_SyncThread')) {
    Write-Host 'FAIL: save sync is not off the UI thread'
    exit 1
}
$syncFn = [regex]::Match($sessionText, '(?ms)^void CeSession_SyncSavedScript\(.*?^}')
$syncThreadFn = [regex]::Match($sessionText, '(?ms)^static unsigned __stdcall _SyncThread\(.*?^}')
if (-not $syncFn.Success -or -not $syncThreadFn.Success -or
    -not $syncThreadFn.Value.Contains('CE_BRIDGE_CMD_SYNC_SCRIPT')) {
    Write-Host 'FAIL: save sync does not send CMD_SYNC_SCRIPT'
    exit 1
}
if (-not $np3Text.Contains('CeSession_SyncSavedScript(Globals.hwndMain)')) {
    Write-Host 'FAIL: FileSave does not trigger the CE script sync'
    exit 1
}
# Regression: a session that had no record yet gets one on its first save.  The
# new record id lives in the sidecar, so the sync worker has to write it back:
# without that the next save asks for record 0 again and creates a second one.
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
$applyFn = [regex]::Match($sessionText, '(?ms)^static unsigned __stdcall _ApplyThread\(.*?^}')
if (-not $applyFn.Success -or $applyFn.Value.Contains('_CeSession_PostWide')) {
    Write-Host 'FAIL: worker still posts UI messages with heap buffers'
    exit 1
}
$sendFn = [regex]::Match($bridgeText, '(?ms)^bool CeBridge_SendRequest\(.*?^}')
if (-not $sendFn.Success -or $sendFn.Value.Contains('CeBridge_PostLine')) {
    Write-Host 'FAIL: request reader still posts UI messages from the worker'
    exit 1
}
if (-not $sessionText.Contains('_CeSession_Log')) {
    Write-Host 'FAIL: step-by-step diagnostic log is missing'
    exit 1
}
if (-not $bridgeText.Contains('_NoReplyThread')) {
    Write-Host 'FAIL: no-reply request is not off the UI thread'
    exit 1
}
if ($sessionText.Contains('MessageBoxW')) {
    Write-Host 'FAIL: modal message boxes still block the apply path'
    exit 1
}

# Enhanced checking: CE's own autoAssembleCheck runs on a worker thread and its
# findings are merged into the local lint pipeline as external diagnostics.
$bridgeHdr = Get-Content -Raw -Encoding UTF8 (Join-Path $src 'CeBridge.h')
$lintText = Get-Content -Raw -Encoding UTF8 (Join-Path $src 'CeLint.c')
if (-not $bridgeHdr.Contains('CE_BRIDGE_CMD_CHECK_SCRIPT')) {
    Write-Host 'FAIL: CMD_CHECK_SCRIPT constant is missing'
    exit 1
}
if (-not $bridgeText.Contains('_CheckThread') -or
    -not $bridgeText.Contains('CeBridge_ReportCheck')) {
    Write-Host 'FAIL: asynchronous online check is missing'
    exit 1
}
if (-not $np3Text.Contains('IDM_CE_CHECK_ONLINE')) {
    Write-Host 'FAIL: online-check command is missing'
    exit 1
}
if (-not $lintText.Contains('CeLint_SetExternalDiags')) {
    Write-Host 'FAIL: CE findings are not merged into local lint'
    exit 1
}
if (-not $bridgeText.Contains('static bool _CeBridge_ParseCheckRecord(')) {
    Write-Host 'FAIL: CE check-record parser is missing'
    exit 1
}

# User-editable templates: a default .tpl is embedded as RCDATA and deployed
# only when absent (never overwrite user edits); an editor command is exposed.
$resHdr = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'language\common_res.h')
$rcText = Get-Content -Raw -Encoding UTF8 (Join-Path $src 'Notepad3.rc')
if (-not $resHdr.Contains('IDR_CE_DEFAULT_TEMPLATES') -or
    -not $resHdr.Contains('IDM_CE_EDIT_TEMPLATES')) {
    Write-Host 'FAIL: template resource / editor command IDs are missing'
    exit 1
}
if (-not $rcText.Contains('IDR_CE_DEFAULT_TEMPLATES RCDATA')) {
    Write-Host 'FAIL: default template file is not embedded'
    exit 1
}
if (-not $bridgeText.Contains('np3_user_templates.tpl')) {
    Write-Host 'FAIL: template file is not deployed'
    exit 1
}
$deployFn = [regex]::Match($bridgeText, '(?ms)^static void _DeploySideFiles\(.*?^}')
if (-not $deployFn.Success -or -not $deployFn.Value.Contains('_IsRegularFile')) {
    Write-Host 'FAIL: user template edits are not protected from overwrite'
    exit 1
}
if (-not $np3Text.Contains('IDM_CE_EDIT_TEMPLATES')) {
    Write-Host 'FAIL: edit-template command is not wired'
    exit 1
}
# "Edit CE default template" must open the file in Notepad3 itself, not
# through the system file association.
$editFn = [regex]::Match($bridgeText, '(?ms)^void CeBridge_EditUserTemplates\(.*?^}')
if (-not $editFn.Success -or
    -not $editFn.Value.Contains('GetModuleFileNameW(NULL, exePath') -or
    -not $editFn.Value.Contains('ShellExecuteW(hwnd, L"open", exePath, params')) {
    Write-Host 'FAIL: edit-template command does not open the file in Notepad3'
    exit 1
}
# Whole-line comments in the Lua region must be skipped before the identifier
# scan, otherwise injected "//" notes trigger false undefined-variable hits.
if (-not $lintText.Contains('comment line must be skipped before the identifier scan')) {
    Write-Host 'FAIL: Lua lint does not skip whole-line comments'
    exit 1
}

# Regression: code generated by the right-click auto-assemble menu only got
# formatted on the first save, so a freshly opened script looked untouched.
# The bridge flags such sessions in the sidecar; the load path must consume the
# request exactly once and format the document right away.
# (ASCII only: Windows PowerShell reads a BOM-less .ps1 as ANSI.)
$formatFn = [regex]::Match($sessionText, '(?ms)^bool CeSession_ConsumeFormatRequest\(.*?^}')
if (-not $formatFn.Success) {
    Write-Host 'FAIL: CeSession_ConsumeFormatRequest is missing'
    exit 1
}
if (-not $formatFn.Value.Contains('s_formatOnOpen = false')) {
    Write-Host 'FAIL: the format request is not one-shot'
    exit 1
}
if (-not $sessionText.Contains('s_formatOnOpen = (meta.format != 0);')) {
    Write-Host 'FAIL: the sidecar format request is not read'
    exit 1
}
if (-not $np3Text.Contains('CeSession_ConsumeFormatRequest()') -or
    -not $np3Text.Contains('CeFormat_Document()')) {
    Write-Host 'FAIL: the load path does not format generated CE scripts'
    exit 1
}

# Regression: the CE menu used to append "Chinese / English" literals, which
# doubled every label and left the whole menu missing in languages without a CE
# popup.  Labels must come from the language resources (English fallback) and
# the popup must be created on demand.
if ($bridgeText -match '/ Run Script|/ Cancel Run|/ CE Online Syntax Check|/ Add to CE Address List') {
    Write-Host 'FAIL: the CE menu still concatenates Chinese and English literals'
    exit 1
}
foreach ($sid in 'IDS_MUI_CE_MENU_REVIEW', 'IDS_MUI_CE_MENU_RUN', 'IDS_MUI_CE_MENU_CANCELRUN') {
    if (-not $bridgeText.Contains($sid)) {
        Write-Host ('FAIL: the CE menu does not use ' + $sid)
        exit 1
    }
}
if (-not $bridgeText.Contains('CreatePopupMenu')) {
    Write-Host 'FAIL: languages without a CE menu never get one'
    exit 1
}
# Merged commands: one run row (auto-detects CE) and one review row.
if (-not $bridgeText.Contains('_RemoveMenuCommand(hmenu, IDM_CE_CHECK)') -or
    -not $bridgeText.Contains('_RemoveMenuCommand(hmenu, IDM_CE_CHECK_ONLINE)') -or
    -not $bridgeText.Contains('_RemoveMenuCommand(hmenu, IDM_CE_RUN_LOCAL)') -or
    -not $bridgeText.Contains('_RemoveMenuCommand(hmenu, IDM_CE_RUN_CE)')) {
    Write-Host 'FAIL: the duplicated check/run menu rows are not removed'
    exit 1
}
if (-not $bridgeText.Contains('void CeBridge_ReviewScript')) {
    Write-Host 'FAIL: the merged code review command is missing'
    exit 1
}
if (-not $np3Text.Contains('case IDM_CE_REVIEW:')) {
    Write-Host 'FAIL: the code review command is not dispatched'
    exit 1
}
# Toolbar: log window, code review, format document, cancel run.
foreach ($idt in 'IDT_CE_LOG', 'IDT_CE_REVIEW', 'IDT_CE_FORMAT', 'IDT_CE_CANCEL') {
    if (-not $np3Text.Contains($idt)) {
        Write-Host ('FAIL: ' + $idt + ' is not offered on the toolbar')
        exit 1
    }
}
# Deploying over an existing script must ask first; Help keeps only the
# shortcut help plus the function-doc query.
if (-not $bridgeText.Contains('IDS_MUI_CE_DEPLOY_OVERWRITE') -or
    -not $bridgeText.Contains('_WriteBridgeFile')) {
    Write-Host 'FAIL: an existing bridge script is overwritten without asking'
    exit 1
}
if (-not $bridgeText.Contains('IDS_MUI_HELP_SHORTCUTS')) {
    Write-Host 'FAIL: the help entry is not renamed to shortcut help'
    exit 1
}
if (-not $bridgeText.Contains('_RemoveMenuCommand(hmenu, IDM_HELP_ONLINEDOCUMENTATION)') -or
    -not $bridgeText.Contains('_RemoveMenuCommand(hmenu, IDM_HELP_ADMINEXE)') -or
    -not $bridgeText.Contains('_RemoveMenuCommand(hmenu, IDM_HELP_UPDATEWEBSITE)') -or
    -not $bridgeText.Contains('_RemoveMenuCommand(hmenu, IDM_HELP_LOCALDOCUMENTATION)')) {
    Write-Host 'FAIL: Notepad3-only help entries are still in the menu'
    exit 1
}
$dialogsText = Get-Content -Raw -Encoding UTF8 (Join-Path $src 'Dialogs.c')
if (-not $dialogsText.Contains('IDS_MUI_ABOUT_CAPTION')) {
    Write-Host 'FAIL: the About box is not renamed to CEscript'
    exit 1
}

$parts = @()
$parts += $metaStruct.Value
$parts += Get-Function $bridge 'static bool _CeBridge_DeadlineReached('
$parts += Get-Function $bridge 'static void _CeBridge_EncodeLen('
$parts += Get-Function $bridge 'static bool _CeBridge_EncodeFrame('
$parts += Get-Function $session 'static bool _CeSession_BuildApplyPayload('
$parts += Get-Function $session 'static bool _CeSession_ParseMeta('
$parts += Get-Function $session 'static bool _CeSession_ParseReply('
$parts += Get-Function $session 'static bool _CeSession_MetaPathFromScript('
$parts += Get-Function $session 'static bool _CeSession_ReplaceMetaMemrecId('
$parts += Get-Function $bridge 'static bool _CeBridge_ParseCheckRecord('

$temp = Join-Path $env:TEMP 'np3-test-ce-session'
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$inc = Join-Path $temp 'CeSessionHelpers.inc'
[System.IO.File]::WriteAllText($inc, ($parts -join "`r`n`r`n"), (New-Object System.Text.UTF8Encoding($false)))

try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vcvars = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
    if (-not $vcvars) { Write-Host 'FAIL: vcvars64.bat not found'; exit 1 }

    $exe = Join-Path $temp 'TestCeSession.exe'
    $cmd = 'call "' + $vcvars + '" && cl /nologo /EHsc /std:c++20 /W4 /WX /I"' + $temp + '" "' + $test + '" /Fe:"' + $exe + '"'
    & cmd /d /c $cmd
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL: TestCeSession.cpp did not compile'; exit 1 }

    & $exe
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL: TestCeSession assertions'; exit 1 }
    Write-Host 'PASS: TestCeSession'
}
finally {
    if (Test-Path $temp) { Remove-Item -Recurse -Force $temp }
}
