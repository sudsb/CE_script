param(
    [string]$CeLintSource,
    [string]$CeScriptSource
)

# Integration harness: compile the real CeLint.c / CeScript.c against a tiny
# fake Scintilla document and run every check on small scripts.  This is the
# only test that exercises the whole lint pipeline (line modes, section model,
# Lua scope model, diagnostics) without a GUI.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$CeLintSource) { $CeLintSource = Join-Path $root 'src\CeLint.c' }
if (!$CeScriptSource) { $CeScriptSource = Join-Path $root 'src\CeScript.c' }
$src = Join-Path $root 'src'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('np3-ce-lint-it-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory $temp)
[void](New-Item -ItemType Directory (Join-Path $temp 'StyleLexers'))

function Write-Ascii([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

try {
    Write-Ascii (Join-Path $temp 'TypeDefs.h') @'
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef long long DocPos;
typedef unsigned long long DocPosU;
typedef int DocLn;
'@

    Write-Ascii (Join-Path $temp 'Helpers.h') @'
#pragma once
#include "TypeDefs.h"
#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) ((void)(P))
#endif
void* AllocMem(size_t cb, DWORD flags);
void FreeMem(void* p);
typedef struct SETTINGS2 { bool CeLintSemantic; } SETTINGS2;
extern SETTINGS2 Settings2;
'@

    Write-Ascii (Join-Path $temp 'SciCall.h') @'
#pragma once
#include "TypeDefs.h"
#define SC_EOL_CRLF 0
#define SC_EOL_CR   1
#define SC_EOL_LF   2
DocPos Sci_GetDocEndPosition(void);
const char* SciCall_GetRangePointer(DocPos start, DocPos len);
DocLn SciCall_LineFromPosition(DocPos pos);
int SciCall_GetPropertyInt(const char* key, int defaultValue);
DocPos SciCall_GetCurrentPos(void);
int SciCall_GetEOLMode(void);
void SciCall_BeginUndoAction(void);
void SciCall_EndUndoAction(void);
void SciCall_SetSel(DocPos anchor, DocPos caret);
void SciCall_ReplaceSel(const char* text);
void SciCall_SetEmptySelection(DocPos pos);
void SciCall_ScrollCaret(void);
'@

    Write-Ascii (Join-Path $temp 'Styles.h') @'
#pragma once
typedef struct EDITLEXER { int dummy; } EDITLEXER;
extern EDITLEXER lexCE;
EDITLEXER* Style_GetCurrentLexerPtr(void);
'@

    Write-Ascii (Join-Path $temp 'CeCompletion.h') @'
#pragma once
bool CeCompletion_IsBuiltinKeyword(const char* name);
'@

    foreach ($h in @('CeScript.h', 'CeLint.h', 'CeDiagnostics.h')) {
        Copy-Item (Join-Path $src $h) (Join-Path $temp $h)
    }
    Copy-Item (Join-Path $src 'StyleLexers\ce_completions_data.h') (Join-Path $temp 'StyleLexers\ce_completions_data.h')
    Copy-Item (Join-Path $src 'StyleLexers\ce_completions_data.h') (Join-Path $temp 'ce_completions_data.h')
    Copy-Item (Join-Path $src 'StyleLexers\ce_completions_data.c') (Join-Path $temp 'ce_completions_data.c')
    Copy-Item $CeScriptSource (Join-Path $temp 'CeScriptHarness.c')
    Copy-Item (Join-Path $PSScriptRoot 'TestCeLintIntegration.c') (Join-Path $temp 'TestCeLintIntegration.c')

    # The report dialogs need real UI resources; cut the source just before them.
    $lintText = [IO.File]::ReadAllText($CeLintSource)
    $cut = $lintText.IndexOf('void CeLint_ShowReport(', [StringComparison]::Ordinal)
    if ($cut -lt 0) { throw 'CeLint_ShowReport marker not found' }
    Write-Ascii (Join-Path $temp 'CeLintHarness.c') $lintText.Substring(0, $cut)

    $test = Join-Path $PSScriptRoot 'TestCeLintIntegration.c'
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $units = 'TestCeLintIntegration.c CeLintHarness.c CeScriptHarness.c ce_completions_data.c'
    & $env:ComSpec /d /c "call `"$vcvars`" >nul && cd /d `"$temp`" && cl /nologo /TC /utf-8 /W3 /I`"$temp`" /I`"$PSScriptRoot`" $units /Fe:TestCeLintIntegration.exe"
    if ($LASTEXITCODE -ne 0) { throw 'CE lint integration harness build failed' }
    & (Join-Path $temp 'TestCeLintIntegration.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CE lint integration tests failed' }
} finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}
