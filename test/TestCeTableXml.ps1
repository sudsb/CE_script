param(
    [string]$CeTableXmlSource
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$CeTableXmlSource) { $CeTableXmlSource = Join-Path $root 'src\CeTableXml.c' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-tablexml-' + [guid]::NewGuid())
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
    $parts = @('// Extracted verbatim from src/CeTableXml.c')
    $parts += (Get-Function $CeTableXmlSource 'static char _Lower(')
    $parts += (Get-Function $CeTableXmlSource 'static const char* _FindCI(')
    $parts += (Get-Function $CeTableXmlSource 'static void _Unescape(')
    $tagHelper = Get-OptionalFunction $CeTableXmlSource 'static const char* _FindTag('
    if ($tagHelper) { $parts += $tagHelper }
    $parts += (Get-Function $CeTableXmlSource 'static char* _ExtractTag(')
    $inc = Join-Path $temp 'CeTableXmlHelpers.inc'
    [IO.File]::WriteAllText($inc, ($parts -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $test = Join-Path $PSScriptRoot 'TestCeTableXml.cpp'
    & $env:ComSpec /d /c "call `"$vcvars`" >nul && cd /d `"$temp`" && cl /nologo /EHsc /std:c++20 /W4 /WX /utf-8 /I`"$temp`" `"$test`" /Fe:TestCeTableXml.exe"
    if ($LASTEXITCODE -ne 0) { throw 'CE table XML test build failed' }
    & (Join-Path $temp 'TestCeTableXml.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CE table XML tests failed' }
} finally {
    Get-ChildItem $temp -File | Remove-Item -Force -ErrorAction SilentlyContinue
    Remove-Item $temp -Force -ErrorAction SilentlyContinue
}
