// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeSettings.h                                                                *
*   Cheat Engine editor options, persisted in [Settings2].                    *
*                                                                             *
*   The dialog edits the existing Settings2 struct in place so the UI can   *
*   stay simple, but it snapshots Settings2 on open: OK keeps the edits and  *
*   persists them, Cancel/ESC/close restores the snapshot.  Consumers read   *
*   Settings2 at the point of use, so no restart is required.                *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_SETTINGS_H_
#define _CE_SETTINGS_H_

#include "TypeDefs.h"

#define CE_DEFAULT_AUTO_ARG_FUNCTIONS L"registersymbol, unregistersymbol, dealloc, label"

// Show the (modeless) CE options / template window.
void CeSettings_Show(HWND hwndParent, bool focusTemplates);

// Small modal one-line input box built with the same runtime template as the
// CE settings dialog.  |buffer| holds the initial value and receives the
// confirmed value; returns false when cancelled.
bool CeSettings_InputBox(HWND hwndParent, LPCWSTR title, LPCWSTR prompt,
                         WCHAR* buffer, size_t cch);

// Restore the shipped CE defaults (settings dialog "reset" button and startup).
void CeSettings_ResetDefaults(void);

bool CeSettings_AllowClipboard(void);
bool CeSettings_AutoArguments(void);
bool CeSettings_AutoPairCode(void);
bool CeSettings_AutoFormatOnSave(void);
bool CeSettings_DynamicCompletions(void);
bool CeSettings_WrapAsmSymbols(void);
bool CeSettings_HighlightUserSymbols(void);

// Indentation is no longer a separate CE option: the formatter, templates and
// generated counterpart code all follow the editor's own IndentWidth plus the
// TabsAsSpaces flag of the current file.
int  CeSettings_IndentWidth(void);
// Write one indent level into |buf| (a TAB or N spaces); returns its length.
int  CeSettings_IndentUnit(char* pszBuf, size_t cchBuf);

// Optional external UTF-8 dictionary (empty = <module dir>\CeDictionary.txt).
const WCHAR* CeSettings_DictionaryPath(void);

// Case-insensitive membership test against the comma separated list.
bool CeSettings_IsAutoArgFunction(const char* utf8Name);

#endif // _CE_SETTINGS_H_
