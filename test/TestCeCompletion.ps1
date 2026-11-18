param(
    # Alternate sources, e.g. a pre-change copy, to show the regression cases failing.
    [string]$Notepad3Source,
    [string]$CeCompletionSource,
    [string]$CeTemplateSource
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$Notepad3Source) { $Notepad3Source = Join-Path $root 'src\Notepad3.c' }
if (!$CeCompletionSource) { $CeCompletionSource = Join-Path $root 'src\CeCompletion.c' }
if (!$CeTemplateSource) { $CeTemplateSource = Join-Path $root 'src\CeTemplate.c' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-completion-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory $temp)

function Get-Function([string]$path, [string]$signature) {
    $source = Get-Content $path -Raw
    $start = $source.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing $signature in $path" }
    $body = $source.IndexOf('{', $start)
    $end = [regex]::Match($source.Substring($body), '(?m)^}')
    if (!$end.Success) { throw "Missing end of $signature" }
    $source.Substring($start, $body + $end.Index + 1 - $start)
}

function Get-Block([string]$path, [string]$signature, [string]$endMarker) {
    $source = Get-Content $path -Raw
    $start = $source.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing $signature in $path" }
    $end = $source.IndexOf($endMarker, $start, [StringComparison]::Ordinal)
    if ($end -lt 0) { throw "Missing '$endMarker' after $signature in $path" }
    $source.Substring($start, $end + $endMarker.Length - $start)
}

try {
    $parts = @('// Extracted verbatim from src/CeCompletion.c, src/CeTemplate.c and src/Notepad3.c')
    $parts += (Get-Function $CeCompletionSource 'const char* CeCompletion_FindDoc(')
    $parts += (Get-Function $CeCompletionSource 'static int _CeTipBodyWidth(')
    $parts += (Get-Function $CeCompletionSource 'static int _CeTipClientHeight(')
    $parts += (Get-Function $CeCompletionSource 'static const char* _SubstringCI(')
    $parts += (Get-Function $CeCompletionSource 'static bool _MatchesRoot(')
    $parts += (Get-Function $CeCompletionSource 'static bool _CandidateMatches(')
    $parts += (Get-Function $CeCompletionSource 'static bool _IsIdentifierLike(')
    $parts += (Get-Function $CeCompletionSource 'static bool _IsIdentifier(')
    $parts += (Get-Function $CeCompletionSource 'static bool _InKeywordListLinear(')
    $parts += (Get-Function $CeCompletionSource 'static bool _InKeywordList(')
    $parts += (Get-Block $CeCompletionSource 'typedef enum _ce_group {' '} CE_GROUP;')
    $parts += (Get-Block $CeCompletionSource 'static const char* const s_commonCeNames[] = {' '};')
    $parts += (Get-Block $CeCompletionSource 'static const char* const s_cOnlyKeywords[] = {' '};')
    $parts += (Get-Function $CeCompletionSource 'static bool _NameInList(')
    $parts += (Get-Function $CeCompletionSource 'static CE_GROUP _EntryGroup(')
    $parts += (Get-Function $CeCompletionSource 'static int _GroupBonus(')
    $parts += (Get-Function $CeCompletionSource 'int CeCompletion_ContextRank(')
    $parts += (Get-Block $CeCompletionSource 'static const char* const s_watchDirectives[] = {' '};')
    $parts += (Get-Block $CeCompletionSource 'static const char* const s_watchWords[] = {' '};')
    $parts += (Get-Block $CeCompletionSource 'static const char* const s_watchRegisters[] = {' '};')
    $parts += (Get-Function $CeCompletionSource 'static bool _NameIsWatchRegister(')
    $parts += (Get-Function $CeCompletionSource 'int CeCompletion_WatchRank(')
    $parts += (Get-Function $CeCompletionSource 'static bool _LooksLikeCall(')
    $parts += (Get-Function $CeCompletionSource 'static bool _IsCeBuiltinVariable(')
    $parts += (Get-Function $CeCompletionSource 'static void _InsertCallCompletion(')
    $parts += (Get-Function $CeCompletionSource 'void CeCompletion_HandleAutoCCompleted(')
    $parts += (Get-Function $CeCompletionSource 'void CeCompletion_ShowCurrentItemTip(')
    $parts += (Get-Function $CeCompletionSource 'bool CeCompletion_HandleCharAdded(')
    $parts += (Get-Function $CeCompletionSource 'bool CeCompletion_OnCharAddedForLsp(')
    $parts += (Get-Function $CeCompletionSource 'bool CeCompletion_GetSelectedVariable(')
    $parts += (Get-Function $CeTemplateSource 'bool CeTemplate_Insert(')
    $parts += (Get-Function $Notepad3Source 'inline static bool _IsAutoCNotification(')
    $parts += (Get-Function $Notepad3Source 'inline static bool _UseFullNotifyHandler(')
    $inc = Join-Path $temp 'CeCompletionHelpers.inc'
    [IO.File]::WriteAllText($inc, ($parts -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $test = Join-Path $PSScriptRoot 'TestCeCompletion.cpp'
    & $env:ComSpec /d /c "call `"$vcvars`" >nul && cd /d `"$temp`" && cl /nologo /EHsc /std:c++20 /W4 /WX /I`"$temp`" `"$test`" /Fe:TestCeCompletion.exe"
    if ($LASTEXITCODE -ne 0) { throw 'CE completion test build failed' }
    & (Join-Path $temp 'TestCeCompletion.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CE completion tests failed' }
} finally {
    Get-ChildItem $temp -File | Remove-Item
    Remove-Item $temp
}
