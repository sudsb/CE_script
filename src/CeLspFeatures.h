// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspFeatures.h                                                              *
*   Maps LSP capabilities onto Notepad3/Scintilla UI.                          *
*                                                                             *
*   All entry points run on the UI thread.  Keeps Scintilla's native AutoC     *
*   list and the existing CE detail pane (no new owner-drawn popup).           *
*                                                                             *
*******************************************************************************/

#pragma once

#include "TypeDefs.h"
#include "CeLspJson.h"
#include "CeLspClient.h"

bool CeLspFeatures_IsEnabled(void);   // setting + CE lexer (ignores server readiness)
bool CeLspFeatures_IsActive(void);    // enabled + server ready
bool CeLspFeatures_OwnsList(void);    // the visible AutoC list came from LSP

// Editor notification hooks.  Return true when the legacy CE completion path
// must be skipped.
bool CeLspFeatures_OnCharAdded(HWND hwnd, int ch);
bool CeLspFeatures_OnAutoCCharDeleted(HWND hwnd);
void CeLspFeatures_OnAutoCSelectionChange(const char* text);
void CeLspFeatures_OnAutoCCancelled(void);
void CeLspFeatures_OnAutoCSelection(const char* text, int fillUpChar);
void CeLspFeatures_OnAutoCCompleted(const char* text, int fillUpChar);
void CeLspFeatures_FlushPendingCompletion(void);
void CeLspFeatures_OnUpdateUI(void);
bool CeLspFeatures_OnDwellStart(DocPos position);
void CeLspFeatures_OnDwellEnd(void);
void CeLspFeatures_ShowCurrentItemTip(void);

// Document lifecycle.
void CeLspFeatures_OnDocumentOpened(void);
void CeLspFeatures_OnDocumentChanged(void);
void CeLspFeatures_OnDocumentSaved(void);
void CeLspFeatures_OnDocumentClosed(void);

// Server message handlers (dispatched by CeLspClient.c on the UI thread).
void CeLspFeatures_OnCompletion(const CELSP_JSON* result, const CELSP_PENDING* req);
void CeLspFeatures_OnSignatureHelp(const CELSP_JSON* result);
void CeLspFeatures_OnHover(const CELSP_JSON* result);
void CeLspFeatures_OnDiagnostics(const CELSP_JSON* params);
void CeLspFeatures_OnLocations(const CELSP_JSON* result, int kind);
void CeLspFeatures_OnDocumentSymbols(const CELSP_JSON* result);
void CeLspFeatures_OnWorkspaceSymbols(const CELSP_JSON* result);
void CeLspFeatures_OnFormatting(const CELSP_JSON* result);
void CeLspFeatures_OnSemanticTokens(const CELSP_JSON* result);
void CeLspFeatures_OnInlayHints(const CELSP_JSON* result);
void CeLspFeatures_OnCodeLens(const CELSP_JSON* result);
void CeLspFeatures_OnDocumentLinks(const CELSP_JSON* result);
void CeLspFeatures_OnDocumentHighlight(const CELSP_JSON* result);
void CeLspFeatures_OnLogMessage(const char* message, int level);
void CeLspFeatures_OnServerStatus(int status, const char* message);

// Commands (called from the CE menu / accelerators in Notepad3.c).
void CeLspFeatures_GotoDefinition(void);
void CeLspFeatures_GotoDeclaration(void);
void CeLspFeatures_FindReferences(void);
void CeLspFeatures_ShowDocumentSymbols(void);
void CeLspFeatures_ShowWorkspaceSymbols(void);
void CeLspFeatures_ShowDiagnostics(void);
void CeLspFeatures_FormatDocument(void);

// Whole-document views re-pulled after every didChange flush.
void CeLspFeatures_RefreshDecorations(void);

// Caret-debounced documentHighlight (IDT_TIMER_CELSP_HILITE fired by Notepad3.c).
void CeLspFeatures_OnHighlightTimer(void);

// SCN_DOUBLECLICK: Ctrl+double-click on a documentLink target opens the file.
// True when the click was consumed (the default hyperlink handling is skipped).
bool CeLspFeatures_OnClickNotify(DocPos position);
