param()

# Regression harness for the LSP AutoC list builder.  It extracts the two
# static helpers from src/CeLspFeatures.c so the test exercises the real
# implementation without Scintilla, Notepad3 or Win32 headers.

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$source = Join-Path $root 'src\CeLspFeatures.c'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-lsplist-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory $temp)

function Assert-Contains([string]$path, [string[]]$needles) {
    $text = Get-Content $path -Raw
    foreach ($needle in $needles) {
        if ($text.IndexOf($needle, [StringComparison]::Ordinal) -lt 0) {
            throw "Missing '$needle' in $path"
        }
    }
}

function Get-Function([string]$path, [string]$signature) {
    $text = Get-Content $path -Raw
    $start = $text.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing $signature in $path" }
    $body = $text.IndexOf('{', $start)
    $end = [regex]::Match($text.Substring($body), '(?m)^}')
    if (!$end.Success) { throw "Missing end of $signature" }
    $text.Substring($start, $body + $end.Index + 1 - $start)
}

try {
    Assert-Contains (Join-Path $root 'src\CeSettings.c') @(
        'Settings2.CeLspSemanticTokens =', 'Settings2.CeLspInlayHints =',
        'Settings2.CeLspCodeLens =', 'Settings2.CeLspDocDiagnostics ='
    )
    Assert-Contains (Join-Path $root 'src\Config\Config.cpp') @(
        'L"CeLspSemanticTokens"', 'L"CeLspInlayHints"', 'L"CeLspCodeLens"',
        'L"CeLspDocDiagnostics"', 'L"CeLspServerArgs"'
    )
    Assert-Contains (Join-Path $root 'Build\Notepad3.ini') @(
        'CeUseLsp=', 'CeLspFallback=', 'CeLspServerPath=', 'CeLspServerArgs=',
        'CeLspDebounceMs=', 'CeLspSemanticTokens=', 'CeLspInlayHints=',
        'CeLspCodeLens=', 'CeLspDocDiagnostics='
    )

    $parts = @(
        '// Extracted verbatim from src/CeLspFeatures.c',
        '#include <stdbool.h>',
        '#include <stddef.h>',
        '#include <stdlib.h>',
        '#include <string.h>'
    )
    $parts += (Get-Function $source 'static bool _ListEnsure(')
    $parts += (Get-Function $source 'static bool _ListAdd(')
    $inc = Join-Path $temp 'CeLspListHelpers.inc'
    [IO.File]::WriteAllText($inc, ($parts -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $test = Join-Path $PSScriptRoot 'TestCeLspList.cpp'
    & $env:ComSpec /d /c "call `"$vcvars`" >nul && cd /d `"$temp`" && cl /nologo /EHsc /std:c++20 /W4 /WX /I`"$temp`" `"$test`" /Fe:TestCeLspList.exe"
    if ($LASTEXITCODE -ne 0) { throw 'TestCeLspList build failed' }

    & (Join-Path $temp 'TestCeLspList.exe')
    if ($LASTEXITCODE -ne 0) { throw 'LSP list builder tests failed' }
} finally {
    Get-ChildItem $temp -File | Remove-Item -Force
    Remove-Item $temp -Force
}
