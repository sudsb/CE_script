param(
    # Alternate sources, e.g. a pre-change copy, to show the regression cases failing.
    [string]$CeLintSource,
    [string]$CeCompletionSource
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$CeLintSource) { $CeLintSource = Join-Path $root 'src\CeLint.c' }
if (!$CeCompletionSource) { $CeCompletionSource = Join-Path $root 'src\CeCompletion.c' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-lint-' + [guid]::NewGuid())
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

function Get-Array([string]$path, [string]$signature) {
    $source = Get-Content $path -Raw
    $start = $source.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing $signature in $path" }
    $end = $source.IndexOf('};', $start, [StringComparison]::Ordinal)
    if ($end -lt 0) { throw "Missing end of $signature" }
    $source.Substring($start, $end + 2 - $start)
}

try {
    $parts = @('// Extracted verbatim from src/CeLint.c and src/CeCompletion.c')
    $parts += (Get-Function $CeLintSource 'static char _Lower(')
    $parts += (Get-Array $CeLintSource 'static const ASM_RULE s_asmRules[] = {')
    $parts += (Get-Array $CeLintSource 'static const LUA_ARITY s_luaArity[] = {')
    $parts += (Get-Function $CeLintSource 'static int _SplitArgs(')
    $parts += (Get-Function $CeLintSource 'static bool _SpanIsNumeric(')
    $parts += (Get-Function $CeLintSource 'static bool _SpanIsPlainIdent(')
    $parts += (Get-Function $CeLintSource 'static bool _IsKnownBareOperand(')
    $parts += (Get-Function $CeLintSource 'static bool _AsmOperandsExpected(')
    $parts += (Get-Function $CeLintSource 'static bool _IsLabelDefinitionLine(')
    $parts += (Get-Function $CeLintSource 'static int _CountCallArgs(')
    $parts += (Get-Function $CeLintSource 'static int _LuaMinArgs(')
    $parts += (Get-Function $CeCompletionSource 'static const char* _SubstringCI(')
    $parts += (Get-Function $CeCompletionSource 'static int _MatchBonus(')
    $parts += (Get-Function $CeCompletionSource 'static int __cdecl _CompareNamePtr(')
    $parts += (Get-Function $CeCompletionSource 'static bool _CatalogueContainsCI(')
    $parts += (Get-Function $CeCompletionSource 'static void _AddExtraWords(')
    $parts += (Get-Array $CeCompletionSource 'static const char* const s_commonCeNames[] = {')
    $parts += (Get-Array $CeCompletionSource 'static const char* const s_cOnlyKeywords[] = {')
    $parts += (Get-Function $CeCompletionSource 'static bool _NameInList(')
    $parts += (Get-Function $CeCompletionSource 'static CE_GROUP _EntryGroup(')
    $parts += (Get-Function $CeCompletionSource 'static int _GroupBonus(')
    $parts += (Get-Function $CeCompletionSource 'static int _ScoreCatalogue(')
    $parts += (Get-Function $CeCompletionSource 'static int _ScoreDocSym(')
    $parts += (Get-Function $CeCompletionSource 'static int __cdecl _CompareCand(')
    $inc = Join-Path $temp 'CeLintHelpers.inc'
    [IO.File]::WriteAllText($inc, ($parts -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $test = Join-Path $PSScriptRoot 'TestCeLint.cpp'
    & $env:ComSpec /d /c "call `"$vcvars`" >nul && cd /d `"$temp`" && cl /nologo /EHsc /std:c++20 /W4 /WX /I`"$temp`" `"$test`" /Fe:TestCeLint.exe"
    if ($LASTEXITCODE -ne 0) { throw 'CE lint test build failed' }
    & (Join-Path $temp 'TestCeLint.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CE lint tests failed' }
} finally {
    Get-ChildItem $temp -File | Remove-Item
    Remove-Item $temp
}
