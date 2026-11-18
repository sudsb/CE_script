param(
    [string]$CeCompletionSource
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$CeCompletionSource) { $CeCompletionSource = Join-Path $root 'src\CeCompletion.c' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-clipboard-' + [guid]::NewGuid())
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

try {
    $parts = @('// Extracted verbatim from src/CeCompletion.c')
    $parts += (Get-Function $CeCompletionSource 'static bool _GetClipboardVariable(')
    $inc = Join-Path $temp 'CeClipboardHelpers.inc'
    [IO.File]::WriteAllText($inc, ($parts -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $test = Join-Path $PSScriptRoot 'TestCeClipboard.cpp'
    & $env:ComSpec /d /c "call `"$vcvars`" >nul && cd /d `"$temp`" && cl /nologo /EHsc /std:c++20 /W4 /WX /utf-8 /I`"$temp`" `"$test`" /Fe:TestCeClipboard.exe"
    if ($LASTEXITCODE -ne 0) { throw 'CE clipboard test build failed' }
    & (Join-Path $temp 'TestCeClipboard.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CE clipboard tests failed' }
} finally {
    Get-ChildItem $temp -File | Remove-Item -Force -ErrorAction SilentlyContinue
    Remove-Item $temp -Force -ErrorAction SilentlyContinue
}
