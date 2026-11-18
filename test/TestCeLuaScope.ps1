param(
    # Alternate sources, e.g. a pre-change copy, to show the regression cases failing.
    [string]$CeScriptSource,
    [string]$CeScriptHeader
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$CeScriptSource) { $CeScriptSource = Join-Path $root 'src\CeScript.c' }
if (!$CeScriptHeader) { $CeScriptHeader = Join-Path $root 'src\CeScript.h' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-lua-scope-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory $temp)

function Get-Function([string]$path, [string]$signature) {
    $source = Get-Content $path -Raw
    # Skip prototypes ("static bool f(...);") that precede the definition.
    $start = -1
    $from = 0
    while (($hit = $source.IndexOf($signature, $from, [StringComparison]::Ordinal)) -ge 0) {
        $body = $source.IndexOf('{', $hit)
        $semi = $source.IndexOf(';', $hit)
        if (($body -ge 0) -and (($semi -lt 0) -or ($body -lt $semi))) {
            $start = $hit
            break
        }
        $from = $hit + $signature.Length
    }
    if ($start -lt 0) { throw "Missing $signature in $path" }
    $body = $source.IndexOf('{', $start)
    $end = [regex]::Match($source.Substring($body), '(?m)^}')
    if (!$end.Success) { throw "Missing end of $signature" }
    $source.Substring($start, $body + $end.Index + 1 - $start)
}

function Get-Span([string]$path, [string]$startMark, [string]$endMark) {
    $source = Get-Content $path -Raw
    $a = $source.IndexOf($startMark, [StringComparison]::Ordinal)
    if ($a -lt 0) { throw "Missing $startMark in $path" }
    $b = $source.IndexOf($endMark, $a, [StringComparison]::Ordinal)
    if ($b -lt 0) { throw "Missing $endMark in $path" }
    $source.Substring($a, $b + $endMark.Length - $a)
}

try {
    $parts = @('// Extracted verbatim from src/CeScript.c and src/CeScript.h')
    $parts += (Get-Span $CeScriptHeader '#define CE_LUA_MAX_SYMS' '} CE_LUA_MODEL;')
    $parts += (Get-Function $CeScriptSource 'static bool _IsIdentStart(')
    $parts += (Get-Function $CeScriptSource 'static bool _IsIdentChar(')
    $parts += (Get-Function $CeScriptSource 'static bool _IsBlank(')
    $parts += (Get-Function $CeScriptSource 'const char* CeScript_SkipBlanks(')
    $parts += (Get-Function $CeScriptSource 'bool CeScript_SectionMarker(')
    $parts += (Get-Span $CeScriptSource 'typedef enum _ce_tok_kind {' '} CE_TOK;')
    $parts += (Get-Function $CeScriptSource 'static bool _LuaTokIs(')
    $parts += (Get-Function $CeScriptSource 'static bool _LuaWordIs(')
    $parts += (Get-Function $CeScriptSource 'static int _LuaLongBracket(')
    $parts += (Get-Function $CeScriptSource 'static const char* _LuaLongEnd(')
    $parts += (Get-Function $CeScriptSource 'static int _LuaTokenize(')
    $parts += (Get-Function $CeScriptSource 'static void _LuaAddSym(')
    $parts += (Get-Function $CeScriptSource 'static void _LuaOpenBlock(')
    $parts += (Get-Function $CeScriptSource 'static void _LuaCloseBlock(')
    $parts += (Get-Function $CeScriptSource 'static void _LuaCommitFuncName(')
    $parts += (Get-Function $CeScriptSource 'static bool _LuaAssignTarget(')
    $parts += (Get-Function $CeScriptSource 'void CeLuaScope_Init(')
    $parts += (Get-Function $CeScriptSource 'void CeLuaScope_Line(')
    $parts += (Get-Function $CeScriptSource 'void CeLuaScope_Finish(')
    $parts += (Get-Function $CeScriptSource 'int CeLuaScope_BlockDepthAt(')
    $parts += (Get-Function $CeScriptSource 'int CeLuaScope_Resolve(')
    $parts += (Get-Function $CeScriptSource 'bool CeLuaScope_DeclaredAnywhere(')
    $inc = Join-Path $temp 'CeLuaScopeHelpers.inc'
    [IO.File]::WriteAllText($inc, ($parts -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $test = Join-Path $PSScriptRoot 'TestCeLuaScope.cpp'
    & $env:ComSpec /d /c "call `"$vcvars`" >nul && cd /d `"$temp`" && cl /nologo /EHsc /std:c++20 /W4 /WX /utf-8 /I`"$temp`" `"$test`" /Fe:TestCeLuaScope.exe"
    if ($LASTEXITCODE -ne 0) { throw 'CE Lua scope test build failed' }
    & (Join-Path $temp 'TestCeLuaScope.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CE Lua scope tests failed' }
} finally {
    Get-ChildItem $temp -File | Remove-Item
    Remove-Item $temp
}
