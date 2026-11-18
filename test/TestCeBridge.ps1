param(
    # Alternate source, e.g. a pre-change copy, to show the regression cases failing.
    [string]$CeBridgeSource,
    [string]$CeLuaSource
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$CeBridgeSource) { $CeBridgeSource = Join-Path $root 'src\CeBridge.c' }
if (!$CeLuaSource) { $CeLuaSource = Join-Path $root 'src\CeLua.c' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-bridge-' + [guid]::NewGuid())
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

function Get-OptionalFunction([string]$path, [string]$signature) {
    $source = Get-Content $path -Raw
    if ($source.IndexOf($signature, [StringComparison]::Ordinal) -lt 0) { return }
    Get-Function $path $signature
}

try {
    $parts = @('// Extracted verbatim from src/CeBridge.c and src/CeLua.c')
    $parts += (Get-Function $CeBridgeSource 'static DWORD _CeBridge_DecodeLen(')
    $parts += (Get-Function $CeBridgeSource 'static void _CeBridge_EncodeLen(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_EncodeFrame(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_IsCeDirName(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_IsCeExeName(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_ParentDir(')
    $parts += (Get-Function $CeBridgeSource 'static size_t _CeBridge_BuildRoots(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_IsAaDeclaration(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_AaNameEqualsCI(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_WatchTokenIs(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_WatchTokenIn(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_IsWatchType(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_IsWatchFormat(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_IsWatchNameChar(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_WatchRegister(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_IsCondSpace(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_ParseWatchCondition(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_IsWatchSpecRest(')
    $parts += (Get-Function $CeBridgeSource 'static void _CeBridge_JoinWatchCond(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_IsIdentChar(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_LineWordIs(')
    $parts += (Get-Function $CeBridgeSource 'static void _CeBridge_SetWatchType(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_ClassifyLiteral(')
    $parts += (Get-Function $CeBridgeSource 'static void _CeBridge_InferSymbolType(')
    $parts += (Get-Function $CeBridgeSource 'static int _CeBridge_CollectWatchDirectives(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_IsWatchDirectiveLine(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_SkipToWatchToken(')
    $parts += (Get-Function $CeBridgeSource 'static size_t _CeBridge_WatchTokenLen(')
    $parts += (Get-Function $CeBridgeSource 'static void _CeBridge_ClassifyWatchLine(')
    $parts += (Get-Function $CeBridgeSource 'static int _CeBridge_CollectAaSymbols(')
    $parts += (Get-Function $CeBridgeSource 'static size_t _CeBridge_AaBracketLevel(')
    $parts += (Get-Function $CeBridgeSource 'static void _CeBridge_AaStateKey(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_AaAppend(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_AaEscapeLua(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_BuildAaRequest(')
    $parts += (Get-Function $CeBridgeSource 'static bool _CeBridge_BuildAaCancelRequest(')
    foreach ($sig in 'static void _CeBridge_AaOwnerSet(', 'static void _CeBridge_AaOwnerClear(',
                     'static bool _CeBridge_AaOwnerMatches(', 'static char* _CeBridge_AaOwnerCancelRequest(') {
        $part = Get-OptionalFunction $CeBridgeSource $sig
        if ($part) { $parts += $part }
    }
    $parts += (Get-Function $CeLuaSource 'static bool _CeLua_IsIdentStart(')
    $parts += (Get-Function $CeLuaSource 'static bool _CeLua_IsIdentChar(')
    $parts += (Get-Function $CeLuaSource 'static bool _CeLua_IsWordInList(')
    $parts += (Get-Function $CeLuaSource 'static bool _CeLua_IsStdLuaName(')
    $parts += (Get-Function $CeLuaSource 'static bool _CeLua_IsAmbiguousName(')
    $parts += (Get-Function $CeLuaSource 'static bool _CeLua_IsCeMarker(')
    $parts += (Get-Function $CeLuaSource 'static bool _CeLua_ScanUsesCeApi(')
    $inc = Join-Path $temp 'CeBridgeHelpers.inc'
    [IO.File]::WriteAllText($inc, ($parts -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $test = Join-Path $PSScriptRoot 'TestCeBridge.cpp'
    & $env:ComSpec /d /c "call `"$vcvars`" >nul && cd /d `"$temp`" && cl /nologo /EHsc /std:c++20 /W4 /WX /I`"$temp`" `"$test`" /Fe:TestCeBridge.exe"
    if ($LASTEXITCODE -ne 0) { throw 'CE bridge test build failed' }
    & (Join-Path $temp 'TestCeBridge.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CE bridge tests failed' }
} finally {
    Get-ChildItem $temp -File | Remove-Item
    Remove-Item $temp
}
