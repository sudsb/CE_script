// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeDiagnostics.h                                                             *
*   Inline diagnostics for Cheat Engine scripts.                              *
*                                                                             *
*   One renderer is shared by the local lint pass (CeLint) and the language   *
*   server client, so both show the same thing: a coloured squiggle under the *
*   offending token plus short ghost text at the end of that line.  Nothing   *
*   pops up, no line height changes and no dialog is involved.                *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_DIAGNOSTICS_H_
#define _CE_DIAGNOSTICS_H_

#include "TypeDefs.h"

#define CE_DIAG_TEXT_CCH   96
#define CE_DIAG_MAX_HINTS  64

// Allocate the dedicated styles / indicators.  Called once from the Scintilla
// set-up path.
void CeDiagnostics_Init(void);

// Style_SetLexer()/StyleClearAll() resets every style, extended ones included,
// so the ghost-text colours must be re-applied after a theme or scheme change
// even when the findings themselves did not change.  Cheap enough for an idle
// hook: it checks one style attribute and only re-applies when it was wiped.
void CeDiagnostics_RefreshStyles(void);

// Remove every hint (text and squiggle).  Safe to call at any time.
void CeDiagnostics_Clear(void);

// Collect a new hint set.  Add() may be called CE_DIAG_MAX_HINTS times between
// Begin() and End(); End() replaces the previous hints atomically, so a stale
// hint is never visible next to the new ones.
void CeDiagnostics_Begin(void);
void CeDiagnostics_Add(DocLn line, int startCol, int endCol, bool error, const char* utf8Text);
void CeDiagnostics_End(void);

// Convenience wrapper for checks that only know the line.
void CeDiagnostics_AddLine(DocLn line, bool error, const char* utf8Text);

#endif // _CE_DIAGNOSTICS_H_
