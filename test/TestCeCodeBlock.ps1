param(
    [string]$CeCompletionSource
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$CeCompletionSource) { $CeCompletionSource = Join-Path $root 'src\CeCompletion.c' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-block-' + [guid]::NewGuid())
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

function Get-Span([string]$path, [string]$startMark, [string]$endMark) {
    $source = Get-Content $path -Raw
    $a = $source.IndexOf($startMark, [StringComparison]::Ordinal)
    if ($a -lt 0) { throw "Missing $startMark in $path" }
    $b = $source.IndexOf($endMark, $a, [StringComparison]::Ordinal)
    if ($b -lt 0) { throw "Missing $endMark in $path" }
    $source.Substring($a, $b + $endMark.Length - $a)
}

try {
    $parts = @('// Extracted verbatim from src/CeCompletion.c')
    $parts += (Get-Span $CeCompletionSource '#define CE_BLOCK_LUA' '} CE_CODE_BLOCK;')
    $parts += (Get-Span $CeCompletionSource 'static const CE_CODE_BLOCK s_codeBlocks[] = {' '};')
    $parts += (Get-Function $CeCompletionSource 'static const CE_CODE_BLOCK* _FindCodeBlock(')
    $parts += (Get-Function $CeCompletionSource 'static void _BlockAppend(')
    $parts += (Get-Function $CeCompletionSource 'static void _RenderCodeBlock(')
    $parts += (Get-Span $CeCompletionSource 'typedef struct _ce_stmt_ctx {' '} CE_STMT_CTX;')
    $parts += (Get-Function $CeCompletionSource 'static CE_STMT_CTX _StatementContext(')
    $parts += (Get-Function $CeCompletionSource 'static bool _IsLuaBlockCloser(')
    $inc = Join-Path $temp 'CeCodeBlockHelpers.inc'
    [IO.File]::WriteAllText($inc, ($parts -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $test = Join-Path $PSScriptRoot 'TestCeCodeBlock.cpp'
    & $env:ComSpec /d /c "call `"$vcvars`" >nul && cd /d `"$temp`" && cl /nologo /EHsc /std:c++20 /W4 /WX /utf-8 /I`"$temp`" `"$test`" /Fe:TestCeCodeBlock.exe"
    if ($LASTEXITCODE -ne 0) { throw 'CE code block test build failed' }
    & (Join-Path $temp 'TestCeCodeBlock.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CE code block tests failed' }
} finally {
    Get-ChildItem $temp -File | Remove-Item
    Remove-Item $temp
}
