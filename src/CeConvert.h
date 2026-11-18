// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeConvert.h                                                                 *
*   Context-menu conversions for Cheat Engine scripts.                        *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_CONVERT_H_
#define _CE_CONVERT_H_

#include "TypeDefs.h"

typedef enum _ce_sel_kind {
    CE_SEL_NONE = 0,
    CE_SEL_NUMBER,     // integer / float / hex literal
    CE_SEL_MEMORY,     // [rax+000001F8] (optionally with a size prefix)
    CE_SEL_REGISTER,   // rax / eax / ax / ...
    CE_SEL_SYMBOL      // a user-defined keyword / variable name
} CE_SEL_KIND;

// Classify the current selection.  Only a non-empty selection is considered,
// so an ordinary right click never exposes the conversion submenus.
CE_SEL_KIND CeConvert_ClassifySelection(void);

// Execute one of the IDM_CE_CONV_* / IDM_CE_GEN_* commands on the selection.
// Returns false when the command or the selection is not applicable.
bool CeConvert_Execute(unsigned int cmd);

// Right-click "rename": prompt for a new identifier and replace every
// whole-word occurrence in the document as a single undoable action.
// |clickPos| is the editor position the context menu was opened on (-1 for
// "use the caret"); a non-empty selection always takes precedence.
bool CeConvert_RenameSymbol(HWND hwnd, DocPos clickPos);

#endif // _CE_CONVERT_H_
