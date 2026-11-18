#pragma once

#include "TypeDefs.h"
#include "StyleLexers/ce_completions_data.h"

bool CeCompletion_IsActive(void);

// Enter auto-indent policy: outside the CE scheme every line may inherit the
// previous line's indentation; CE scripts only do that inside Auto Assembler
// code, Lua and C sections always start at the left margin.
bool CeCompletion_AllowAutoIndent(void);

// One-time setup/teardown: register the per-category AutoC images and the
// modeless detail window class (called from the Scintilla init / WM_DESTROY).
void CeCompletion_Init(void);
void CeCompletion_Destroy(void);
// Re-apply list sizing after a settings change (no restart needed).
void CeCompletion_ApplyRuntime(void);

// The document-symbol index is rebuilt lazily; mark it stale on text changes.
void CeCompletion_InvalidateDocSymbols(void);
// Hide the detail window (called whenever the AutoC list is dismissed).
void CeCompletion_HideTip(void);
// Show the very same detail pane with content supplied by the LSP client.
// head/body are UTF-8; kind selects the CE_KIND_* colour strip.
void CeCompletion_ShowTipEx(const char* utf8Head, const char* utf8Body, int kind);
// Same pane, but a syntax fragment is painted right after the keyword in the
// head line: blue for a known signature, red when the name has no catalogue
// entry (unknown / suspicious keyword).
void CeCompletion_ShowTipWithSyntax(const char* utf8Head, const char* utf8Syntax,
                                    bool warnSyntax, const char* utf8Body, int kind);

// The complete reference pane: shows the whole local note of the keyword under
// the caret (or the item highlighted in the AutoC list) in a separate modeless
// window placed on the upper right of the detail pane.  The command toggles it.
void CeCompletion_ToggleDocWindow(void);
// Keep the (already visible) reference pane in sync with the item highlighted
// in the completion list.  The pane then closes together with that list.
void CeCompletion_FollowDocWindow(const char* pszName);
void CeCompletion_HideDocWindow(void);

// Reload the optional external UTF-8 dictionary (CeDictionary.txt or the
// configured path).  Safe to call while editing.
void CeCompletion_ReloadDictionary(void);

// True when the name is part of the built-in CE catalogue / keyword lists.
// Words from the user dictionary are deliberately not built-in.
bool CeCompletion_IsBuiltinKeyword(const char* pszName);

bool CeCompletion_HandleCharAdded(HWND hwnd, int ch);
// LSP path: keep CE's AA symbol auto-wrapping, but leave '.' / '(' handling
// to the language server's triggerCharacters (no legacy list / calltip).
bool CeCompletion_OnCharAddedForLsp(int ch);

// Called from SCN_AUTOCSELECTION for accepted methods.  Hides the detail pane
// immediately and records the selection so parentheses are still applied even
// when Scintilla does not deliver SCN_AUTOCCOMPLETED (e.g. a cancelled list).
void CeCompletion_HandleAutoCSelection(const char* pszSelected, int fillUpChar);
// Fallback invoked from SCN_UPDATEUI once the AutoC list is gone.
void CeCompletion_FlushPendingCompletion(void);

// fillUpChar is scn->ch for SCN_AUTOCCOMPLETED: a '(' fill-up is inserted by
// Scintilla itself, so the completion must not add another pair of brackets.
void CeCompletion_HandleAutoCCompleted(const char* pszSelected, int fillUpChar);

// Resolve the variable used by automatic function arguments:
// selection -> clipboard (when enabled) -> first alloc definition.
bool CeCompletion_ResolveVariable(char* pszOut, size_t cchOut);
// Variable named by the current editor selection (must be a defined symbol).
// Template insertion prefers this over the clipboard.
bool CeCompletion_GetSelectedVariable(char* pszOut, size_t cchOut);
// Clipboard-only variable lookup.  Template insertion falls back to it when
// nothing usable is selected; it never uses the first alloc definition.
bool CeCompletion_GetClipboardVariable(char* pszOut, size_t cchOut);
// Preview the documentation (signature + "--" comment) of the item that is
// currently highlighted in the autocompletion list.
void CeCompletion_HandleAutoCSelectionChange(const char* pszSelected);
void CeCompletion_HandleAutoCCancelled(void);
// Show the documentation of the currently highlighted list item (the list
// itself does not send a selection notification when it first opens).
void CeCompletion_ShowCurrentItemTip(void);

// Space separated, priority-ordered list of "word?N" items (N = CE_KIND_*
// image id), combining the bundled catalogue and the cached document-defined
// symbols.  Ranking uses the language in effect at the caret (Lua / AA / C),
// common CE usage and user-defined variables/functions.  The caller must set
// SC_ORDER_CUSTOM before passing the list to SCI_AUTOCSHOW.
char* CeCompletion_CreateWordList(const char* pszRoot);
const CE_COMPLETION_ENTRY* CeCompletion_Find(const char* pszName);

// Language in effect at the caret (CE_LANG_*), used by the LSP completion path
// to rank the server list the same way the local list is ranked.
int CeCompletion_CurrentMode(void);
// Context weight of one item in |mode|: language group (AA / Lua / C), common
// CE usage and call-shape bonuses.  Higher ranks first; callers keep their own
// order for equal weights.  |kind| is the CE_KIND_* image id.
int CeCompletion_ContextRank(const char* pszName, int kind, int mode);

// Extra weight for np3watch comment vocabulary (directives, types, formats and
// on/off switches).  Returns 0 for unrelated names.  The LSP adapter adds it
// only while the caret is on an np3watch directive line, so the vocabulary is
// ranked ahead of generic AA/Lua items exactly in the context that uses it.
int CeCompletion_WatchRank(const char* pszName);

// Local word sources that the LSP list must not lose: the bundled common
// variable dictionary and the optional external dictionary.  Returns the
// number of entries written to |out| whose name contains |pszRoot|
// (case-insensitive, same rule as the local list).
typedef struct CE_WORD_HINT {
    const char* name;    // UTF-8, points into the static tables
    const char* detail;  // UTF-8 signature / description (may be empty)
    int         kind;    // CE_KIND_*
} CE_WORD_HINT;

int CeCompletion_CollectExtraWords(const char* pszRoot, CE_WORD_HINT* out, int max);
void CeCompletion_InsertEntry(const CE_COMPLETION_ENTRY* pEntry);

// Code-block skeletons are listed like ordinary words but expand to multiple
// lines when accepted.  The LSP client needs these two hooks because it owns
// the AutoC list (and therefore the completion notifications) by default.
bool CeCompletion_IsCodeBlockName(const char* pszName);
bool CeCompletion_InsertCodeBlock(const char* pszName);

// Multiline UTF-8 usage notes (function, syntax, example, constraints,
// related keywords, pitfalls and AA context) for a keyword, or NULL when no
// hand-written note exists.  Shared by the AutoC detail pane and the reference
// panel.
const char* CeCompletion_FindDoc(const char* pszName);

// Name of the item currently highlighted in the AutoC list (UTF-8), or an
// empty string when no list is active.  Used by F1 to open the local reference
// at that keyword.
const char* CeCompletion_CurrentItem(void);
// Record the item highlighted in an externally driven (LSP) completion list, so
// the reference pane command resolves the same keyword as the local list path.
// NULL or "" clears it.
void CeCompletion_SetCurrentItem(const char* pszName);
