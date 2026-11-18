// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeFormat.h                                                                  *
*   Conservative document formatter for Cheat Engine scripts.                 *
*                                                                             *
*   Lua lines get canonical indentation and single spaces; in AA only labels, *
*   section markers and comments stay at column 0 while every command or      *
*   instruction gets one indent level; C lines only lose trailing blanks.     *
*   The indent unit is the editor's own IndentWidth / TabsAsSpaces setting,   *
*   so a TAB press and the formatter agree.  Multi-line strings/comments are  *
*   never touched.                                                            *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_FORMAT_H_
#define _CE_FORMAT_H_

#include "TypeDefs.h"

// Format the current document in place (single undo action).  Returns true
// when the document changed.  Auto Assembler memory operands are compacted as
// part of the format ("[rdi - 18]" -> "[rdi-18]").
bool CeFormat_Document(void);

// Only remove the blanks Cheat Engine's assembler rejects inside AA memory
// operands.  Leaves indentation, comments, strings and every other byte of
// the document untouched (single undo action, no pointless undo when nothing
// changed).  Used on save when the full formatter is switched off.
bool CeFormat_StripAsmSpaces(void);

#endif // _CE_FORMAT_H_
