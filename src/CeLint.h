// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLint.h                                                                    *
*   Syntax checks for Cheat Engine scripts (Lua / Auto Assembler / C).        *
*                                                                             *
*   Default rule set - structural only:                                       *
*     brackets, quotes, block comments, Lua if/for/while/function/end,        *
*     {$lua} / {$asm} / {$ccode} marker pairing, AOB byte patterns.           *
*   These are the mistakes that make Cheat Engine refuse to activate a        *
*   script and they have no plausible "it is actually valid" reading.         *
*                                                                             *
*   Heuristic "will this work?" checks (unknown AA statement, unknown         *
*   register/symbol, undefined label, alloc/dealloc pairing, duplicate        *
*   definition, reserved-name collision, Lua arity ...) are opt-in via        *
*   [Settings2] CeLintStrict=1: unknown names are usually newer CE features,  *
*   symbols injected by other scripts or {$include} content, so they must     *
*   not be reported by default.                                               *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_LINT_H_
#define _CE_LINT_H_

#include "TypeDefs.h"

#define CE_LINT_MAX_DIAGS   200
#define CE_LINT_MSG_CCH     160

typedef struct _ce_diag {
    int  line;          // 1-based
    int  col;           // 0-based byte column, < 0 = whole line
    int  len;           // token length in bytes, <= 0 = until end of line
    bool error;         // false == warning
    bool alert;         // reserved-symbol conflict: show inline even as warning
    char message[CE_LINT_MSG_CCH];
} CE_DIAG;

typedef struct _ce_lint {
    CE_DIAG items[CE_LINT_MAX_DIAGS];
    int     count;
    int     errors;
    int     warnings;
} CE_LINT;

// Findings from CE's own autoAssembleCheck (CMD_CHECK_SCRIPT).  They are
// merged into CeLint_Run so the same squiggle/ghost-text renderer displays
// them alongside the local heuristics; the caller replaces the whole set.
typedef struct _ce_external_diag {
    int  line;                       // 1-based
    bool error;                     // false == warning
    char message[CE_LINT_MSG_CCH];
} CE_EXTERNAL_DIAG;

void CeLint_SetExternalDiags(const CE_EXTERNAL_DIAG* items, int count);
void CeLint_ClearExternalDiags(void);

// Run every enabled check against the current Scintilla document.
void CeLint_Run(CE_LINT* lint);

// Render the findings as inline hints (squiggle + end-of-line ghost text).
void CeLint_ApplyAnnotations(const CE_LINT* lint);
void CeLint_ClearAnnotations(void);

// Called from SCN_MODIFIED (a Scintilla notification): only sets a flag, the
// next idle pass drops the hints that no longer match the text.  Nothing is
// sent back to Scintilla from inside the notification.
void CeLint_Invalidate(void);

// Timer hook (never called from a Scintilla notification): runs only when the
// inputs changed and only re-renders the hints when the findings changed.
// Returns true when the debounce is still pending, so the caller re-arms the
// one-shot timer; otherwise the final state would never be analysed after the
// user stops typing.
bool CeLint_OnIdle(void);

// Menu command: show the full report (also refreshes the inline hints).
void CeLint_ShowReport(HWND hwndParent);

// Same report for the merged "Code Review" command.  |bOnlineChecked| is true
// when CE's own autoAssembleCheck already contributed, which suppresses the
// "bridge not connected" note.
void CeLint_ShowReview(HWND hwndParent, bool bOnlineChecked);

// Explicit reminder before a CE script is saved/activated: warns when a
// user-defined symbol collides with a CE keyword or reserved word (only with
// CeLintStrict enabled).
void CeLint_WarnReservedSymbols(HWND hwndParent);

#endif // _CE_LINT_H_
