param(
    # Alternate sources, e.g. a pre-change copy, to show the regression cases failing.
    [string]$CeHighlightSource,
    [string]$CeDiagnosticsSource
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$CeHighlightSource) { $CeHighlightSource = Join-Path $root 'src\CeHighlight.c' }
if (!$CeDiagnosticsSource) { $CeDiagnosticsSource = Join-Path $root 'src\CeDiagnostics.c' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-highlight-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory $temp)

function Get-Function([string]$path, [string]$signature) {
    $source = Get-Content $path -Raw
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

try {
    $parts = @('// Extracted verbatim from src/CeHighlight.c and src/CeDiagnostics.c')
    $parts += (Get-Function $CeHighlightSource 'static bool _IsIdentStart(')
    $parts += (Get-Function $CeHighlightSource 'static bool _IsIdentChar(')
    $parts += (Get-Function $CeHighlightSource 'static unsigned char _Lower(')
    $parts += (Get-Function $CeHighlightSource 'static int _CompareNameToSpan(')
    $parts += (Get-Function $CeHighlightSource 'static int __cdecl _CompareDefByName(')
    $parts += (Get-Function $CeHighlightSource 'static bool _IsDefName(')
    $parts += (Get-Function $CeHighlightSource 'static DocPos _BlockCommentEnd(')
    $parts += (Get-Function $CeDiagnosticsSource 'static bool _IsPrimaryHint(')
    $parts += (Get-Function $CeDiagnosticsSource 'static const char* _FallbackText(')
    $parts += (Get-Function $CeDiagnosticsSource 'static void _CopyShortText(')
    $parts += (Get-Function $CeDiagnosticsSource 'static void _AppendDiagText(')
    $parts += (Get-Function $CeDiagnosticsSource 'static void _MergeLineMessage(')
    $inc = Join-Path $temp 'CeHighlightHelpers.inc'
    [IO.File]::WriteAllText($inc, ($parts -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $test = Join-Path $PSScriptRoot 'TestCeHighlight.cpp'
    & $env:ComSpec /d /c "call `"$vcvars`" >nul && cd /d `"$temp`" && cl /nologo /EHsc /std:c++20 /W4 /WX /utf-8 /I`"$temp`" `"$test`" /Fe:TestCeHighlight.exe"
    if ($LASTEXITCODE -ne 0) { throw 'CE highlight test build failed' }
    & (Join-Path $temp 'TestCeHighlight.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CE highlight tests failed' }
} finally {
    Get-ChildItem $temp -File | Remove-Item
    Remove-Item $temp
}
