param()

# Regression harness for the shared LSP protocol layer.  It compiles the real
# product sources (as C) plus a C++ assertion driver; no Notepad3/Win32 header,
# Scintilla or running editor is required.

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$src = Join-Path $root 'src'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-lsp-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory $temp)

function Get-OptionalFunction([string]$path, [string]$signature) {
    $source = Get-Content $path -Raw
    $start = $source.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { return }
    $body = $source.IndexOf('{', $start)
    $end = [regex]::Match($source.Substring($body), '(?m)^}')
    if (!$end.Success) { throw "Missing end of $signature" }
    $source.Substring($start, $body + $end.Index + 1 - $start)
}

try {
    $sessionParts = @('// Extracted verbatim from src/CeLspClient.c')
    foreach ($sig in 'static bool _Celsp_ShouldRetryStart(', 'static bool _Celsp_DiagIsCurrent(',
                    'static bool _Celsp_MessageIsCurrent(', 'static bool _Celsp_RequestExpired(',
                    'static int _Celsp_PickPendingDrop(', 'static bool _Celsp_MergeChangeRange(',
                    'static bool _Celsp_DiagSeverityForSquiggle(',
                    'static void _Celsp_InsertionPosition(',
                    'static bool _Celsp_AllowIncremental(') {
        $part = Get-OptionalFunction (Join-Path $src 'CeLspClient.c') $sig
        if ($part) { $sessionParts += $part }
    }
    [IO.File]::WriteAllText((Join-Path $temp 'CeLspSessionHelpers.inc'),
        ($sessionParts -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

    $cSources = @('CeLspJson.c', 'CeLspFraming.c', 'CeLspPos.c') |
        ForEach-Object { '"' + (Join-Path $src $_) + '"' }

    $compileC = 'call "{0}" >nul && cd /d "{1}" && cl /nologo /c /TC /W4 /WX /utf-8 /I"{2}" {3}' -f `
        $vcvars, $temp, $src, ($cSources -join ' ')
    & $env:ComSpec /d /c $compileC
    if ($LASTEXITCODE -ne 0) { throw 'LSP C protocol sources failed to compile' }

    $test = Join-Path $PSScriptRoot 'TestCeLsp.cpp'
    $compileCpp = 'call "{0}" >nul && cd /d "{1}" && cl /nologo /EHsc /std:c++20 /W4 /WX /utf-8 /I"{1}" /I"{2}" "{3}" CeLspJson.obj CeLspFraming.obj CeLspPos.obj /Fe:TestCeLsp.exe' -f `
        $vcvars, $temp, $src, $test
    & $env:ComSpec /d /c $compileCpp
    if ($LASTEXITCODE -ne 0) { throw 'TestCeLsp build failed' }

    & (Join-Path $temp 'TestCeLsp.exe')
    if ($LASTEXITCODE -ne 0) { throw 'LSP protocol tests failed' }
} finally {
    Get-ChildItem $temp -File | Remove-Item -Force
    Remove-Item $temp -Force
}
