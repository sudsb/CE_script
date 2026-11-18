// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeHighlight.h                                                               *
*   Dynamic highlighting of user-defined Cheat Engine symbols.               *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_HIGHLIGHT_H_
#define _CE_HIGHLIGHT_H_

#include "TypeDefs.h"

// Configure the indicator once, after Scintilla has been initialised.
void CeHighlight_Init(void);

// Re-apply the marks.  force bypasses the idle throttle (settings change,
// lexer/theme reset, buffer switch).  Safe to call from SCN_UPDATEUI.
void CeHighlight_Refresh(bool force);

// Text/lexer changes make the cached marks stale.
void CeHighlight_Invalidate(void);

#endif // _CE_HIGHLIGHT_H_
