// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspFeatures.c                                                              *
*   LSP -> Scintilla UI adapter.  See CeLspFeatures.h.                        *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <commctrl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Notepad3.h"
#include "resource.h"
#include "SciCall.h"
#include "PathLib.h"
#include "Dialogs.h"
#include "CeCompletion.h"
#include "CeScript.h"
#include "CeFormat.h"
#include "CeLint.h"
#include "CePanel.h"
#include "StyleLexers/ce_completions_data.h"
#include "lexers_x/SciXLexer.h"
#include "CeLspJson.h"
#include "CeLspClient.h"
#include "CeLspPos.h"
#include "CeLspFeatures.h"

#define CELSP_ITEM_LIMIT    1200
#define CELSP_TIP_BODY_MAX  2048
#define CELSP_MAX_DOC_BYTES (1024 * 1024) // full-document sync cap

// Server diagnostics use free marker slots (0-9 are the NP3 markers, 21+ the
// fold/history set) so they never collide with the local lint renderer.
#define MARKER_LSP_ERROR   10
#define MARKER_LSP_WARNING 11

typedef struct CELSP_ITEM
{
    char label[256];
    char insert[512];
    char detail[512];
    char doc[CELSP_TIP_BODY_MAX];
    int  kind;      // CE_KIND_* AutoC image id
    bool snippet;
    int  weight;    // CeCompletion_ContextRank() in the language at the caret
    int  order;     // server order, tie breaker for equal weights
} CELSP_ITEM;

static CELSP_ITEM* s_items = NULL;
static int         s_itemCount = 0;
static int         s_itemCap = 0;

static char*  s_list = NULL;
static size_t s_listCap = 0u;
static size_t s_listLen = 0u;   // maintained explicitly: no strlen() per item

static DocPos s_wordStart = 0;
static char   s_root[128] = { 0 };
static char   s_pendingName[256] = { 0 };
static bool   s_pending = false;
static bool   s_lspList = false;   // the currently visible AutoC list came from LSP

// Whole-document decorations (semantic tokens / hints / code lens / links /
// highlights / server diagnostics).  Defined in the feature section below.
static DocPos s_lastHlPos = -1;
static DocPos s_hoverPos = -1;   // position the pending hover request was sent for
static void _ClearDecorations(void);
static void _RenderAnnotations(void);
static void _EnsureLspDecorInit(void);
static bool _ParseRange(const CELSP_JSON* range, DocPos* pStart, DocPos* pEnd);

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------

static int _LspKindToCe(int kind)
{
    switch (kind) {
    case 14: return CE_KIND_KEYWORD;
    case 3:  return CE_KIND_FUNCTION;
    case 6:  return CE_KIND_VARIABLE;
    case 15: return CE_KIND_AA;
    case 25: return CE_KIND_ASM;
    case 7:  return CE_KIND_TYPE;
    case 21: return CE_KIND_CONSTANT;
    case 9:  return CE_KIND_SECTION;
    default: return CE_KIND_NONE;
    }
}

static const char* _ItemText(const CELSP_JSON* value)
{
    if (!value) {
        return NULL;
    }
    const char* const str = CeLspJson_String(value);
    if (str) {
        return str;
    }
    return CeLspJson_GetString(value, "value");
}

static void _Copy(char* dst, size_t cap, const char* src)
{
    dst[0] = '\0';
    if (!src) {
        return;
    }
    const size_t n = strlen(src);
    const size_t copy = (n < (cap - 1u)) ? n : (cap - 1u);
    memcpy(dst, src, copy);
    dst[copy] = '\0';
}

// Word around a document position (the editor's own word chars, so CE names
// with '.'/'_' resolve the same way the server resolves them).
static bool _WordAtPosition(DocPos position, char* out, size_t cap)
{
    out[0] = '\0';
    if ((position < 0) || (position > SciCall_GetTextLength())) {
        return false;
    }
    const DocPos start = SciCall_WordStartPosition(position, true);
    const DocPos end = SciCall_WordEndPosition(position, true);
    if (end <= start) {
        return false;
    }
    DocPos len = end - start;
    if (len > (DocPos)(cap - 1u)) {
        len = (DocPos)(cap - 1u);
    }
    memcpy(out, SciCall_GetRangePointer(start, len), (size_t)len);
    out[len] = '\0';
    return true;
}

static bool _ListEnsure(size_t needed)
{
    if (needed <= s_listCap) {
        return true;
    }
    size_t cap = (s_listCap == 0u) ? 8192u : s_listCap;
    while (cap < needed) {
        cap *= 2u;
    }
    char* const grown = (char*)realloc(s_list, cap);
    if (!grown) {
        return false;
    }
    s_list = grown;
    s_listCap = cap;
    return true;
}

static bool _ListAdd(const char* src, size_t len)
{
    size_t used = s_listLen;
    // +2 covers the optional leading separator and the terminating NUL.
    if (!_ListEnsure(used + len + 2u)) {
        return false;
    }
    if (used == 0u) {
        s_list[0] = ' ';  // leading separator, skipped by AutoCShow
        s_list[1] = '\0';
        used = 1u;
    }
    // Write at the tracked offset.  Deriving it from strlen() would scan the
    // bytes just copied (they contain no NUL) into stale or uninitialised heap
    // data and place the terminator outside the buffer.
    memcpy(s_list + used, src, len);
    s_list[used + len] = '\0';
    s_listLen = used + len;
    return true;
}

static const CELSP_ITEM* _FindItem(const char* label)
{
    if (!label) {
        return NULL;
    }
    for (int i = 0; i < s_itemCount; ++i) {
        if (strcmp(s_items[i].label, label) == 0) {
            return &s_items[i];
        }
    }
    return NULL;
}

// Highest context weight first; equal weights keep the server order.
static int _CompareItem(const void* pA, const void* pB)
{
    const CELSP_ITEM* const a = (const CELSP_ITEM*)pA;
    const CELSP_ITEM* const b = (const CELSP_ITEM*)pB;
    if (a->weight != b->weight) {
        return (a->weight > b->weight) ? -1 : 1;
    }
    return (a->order < b->order) ? -1 : 1;
}

static void _Log(const char* message, int level)
{
    WCHAR wide[1024];
    MultiByteToWideChar(CP_UTF8, 0, message ? message : "", -1,
                        wide, (int)COUNTOF(wide));
    wide[COUNTOF(wide) - 1] = L'\0';
    CePanel_Log(level, wide);
}

// Expand the small subset of LSP snippet syntax our built-in server emits:
// "$0", "$1" and "${n:default}".  |*caret| gets the byte offset of the first
// tab stop, or the end of the text when there is none.
static void _ExpandSnippet(const char* src, char* out, size_t cap, int* caret)
{
    size_t n = 0u;
    *caret = -1;
    if (!src || (cap == 0u)) {
        return;
    }
    for (const char* p = src; *p && (n + 1u < cap);) {
        if ((p[0] == '\\') && p[1]) {
            out[n++] = p[1];
            p += 2;
            continue;
        }
        if (p[0] == '$') {
            const char digit = p[1];
            if ((digit >= '0') && (digit <= '9')) {
                if (*caret < 0) {
                    *caret = (int)n;
                }
                p += 2;
                continue;
            }
            if ((p[1] == '{') && (p[2] >= '0') && (p[2] <= '9')) {
                const char* const close = strchr(p + 3, '}');
                if (close) {
                    const char* const colon = strchr(p + 3, ':');
                    const char* def = (colon && (colon < close)) ? (colon + 1) : NULL;
                    const size_t defLen = def ? (size_t)(close - def) : 0u;
                    if (*caret < 0) {
                        *caret = (int)n;
                    }
                    for (size_t i = 0u; (i < defLen) && (n + 1u < cap); ++i) {
                        out[n++] = def[i];
                    }
                    p = close + 1;
                    continue;
                }
            }
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    if (*caret < 0) {
        *caret = (int)n;
    }
}

// ----------------------------------------------------------------------------
// Activation
// ----------------------------------------------------------------------------

bool CeLspFeatures_IsEnabled(void)
{
    // Full-document sync copies the whole buffer on every edit: very large
    // scripts stay on the local fallback instead of stalling the UI thread.
    return Settings2.CeUseLsp && CeCompletion_IsActive() &&
           (SciCall_GetLength() <= (DocPos)CELSP_MAX_DOC_BYTES);
}

bool CeLspFeatures_IsActive(void)
{
    return CeLspFeatures_IsEnabled() && CeLspClient_IsReady();
}

bool CeLspFeatures_OwnsList(void)
{
    return s_lspList && SciCall_AutoCActive();
}

// ----------------------------------------------------------------------------
// Editor notification hooks
// ----------------------------------------------------------------------------

static void _RootAtCaret(char* root, size_t cap, DocPos* pWordStart)
{
    root[0] = '\0';
    const DocPos pos = SciCall_GetCurrentPos();

    // Walk left with exactly the character set the server uses for its prefix
    // (CeLspPos_IsIdentChar).  Scintilla's WordStartPosition() treats the
    // character before the caret as part of the word even when it is a space,
    // which yielded a " " prefix: the server then saw an empty prefix and
    // returned the whole catalogue while the client thought a word was typed.
    DocPos start = pos;
    while ((start > 0) && ((pos - start) < (DocPos)(cap - 1u))) {
        const unsigned char prev = (unsigned char)SciCall_GetCharAt(start - 1);
        if (!CeLspPos_IsIdentChar(prev)) {
            break;
        }
        --start;
    }
    // Section markers such as {$lua}/{$asm} are not identifier-shaped.  '$' is
    // a word character in some Scintilla word sets, so the walk above stops
    // either right after "{$" or between '{' and '$'.  Normalise both cases so
    // the catalogue entries match and the accepted marker replaces the whole
    // opener instead of leaving a stray '{' behind.
    DocPos const docEnd = SciCall_GetLength();
    char const left2 = (start >= 2) ? (char)SciCall_GetCharAt(start - 2) : '\0';
    char const left1 = (start >= 1) ? (char)SciCall_GetCharAt(start - 1) : '\0';
    char const first = (start < docEnd) ? (char)SciCall_GetCharAt(start) : '\0';
    int const width = CeLspPos_SectionOpenerWidth(left2, left1, first);
    if ((width > 0) && (start >= (DocPos)width) &&
        ((pos - (start - (DocPos)width)) < (DocPos)(cap - 1u))) {
        start -= (DocPos)width;
    }
    *pWordStart = start;
    const DocPos len = pos - start;
    if (len > 0) {
        const size_t copy = ((size_t)len < (cap - 1u)) ? (size_t)len : (cap - 1u);
        memcpy(root, SciCall_GetRangePointer(start, (DocPos)copy), copy);
        root[copy] = '\0';
    }
}

static bool _RequestList(HWND hwnd, int ch)
{
    (void)hwnd;
    if (!CeLspFeatures_IsEnabled()) {
        return false;
    }
    if (CeCompletion_OnCharAddedForLsp(ch)) {
        if (SciCall_AutoCActive()) {
            SciCall_AutoCCancel();
        }
        CeLspClient_OnDocumentChanged(); // re-sync after the text rewrite
        return true;
    }
    CeLspClient_OnDocumentChanged();
    if (!CeLspClient_IsReady()) {
        CeLspClient_Start(hwnd);
        return !Settings2.CeLspFallback; // use the legacy list until the handshake finishes
    }
    if (ch == '(') {
        SciCall_AutoCCancel();
        s_lspList = false;
        CeCompletion_HideTip();
        CeLspClient_CancelPending(CELSP_REQ_COMPLETION);
        CeLspClient_CancelCompletionDebounce();
        CeLspClient_RequestSignatureHelp();
        return true;
    }
    char root[128];
    DocPos wordStart = 0;
    _RootAtCaret(root, COUNTOF(root), &wordStart);
    if (ch == '.') {
        // "foo." has no prefix the catalogue could match, and the built-in
        // server has no receiver analysis.  Ask for the broad member list
        // instead, which is what the editor showed before LSP.
        root[0] = '\0';
        wordStart = SciCall_GetCurrentPos();
    }
    if ((root[0] == '\0') && (ch != '.')) {
        // Space, newline, ')', ... with no identifier before the caret: asking
        // the server would return the whole catalogue (up to 1200 items) for a
        // list the user did not ask for.
        if (SciCall_AutoCActive()) {
            SciCall_AutoCCancel();
        }
        s_lspList = false;
        CeCompletion_HideTip();
        return true;
    }
    const int triggerKind = (ch == '.') ? 2 : 1;  // TriggerCharacter / Invoked
    CeLspClient_RequestCompletion(root, wordStart, triggerKind, (ch == '.') ? '.' : 0);
    return true;
}

bool CeLspFeatures_OnCharAdded(HWND hwnd, int ch)
{
    return _RequestList(hwnd, ch);
}

bool CeLspFeatures_OnAutoCCharDeleted(HWND hwnd)
{
    return _RequestList(hwnd, 0);
}

void CeLspFeatures_OnAutoCSelectionChange(const char* text)
{
    if (!CeLspFeatures_IsActive() || !SciCall_AutoCActive() || !text || !*text) {
        CeCompletion_HideTip();
        CeCompletion_SetCurrentItem(NULL);
        return;
    }
    const CELSP_ITEM* const item = _FindItem(text);
    if (!item) {
        CeCompletion_HideTip();
        CeCompletion_SetCurrentItem(NULL);
        return;
    }
    // The reference pane command and F1 resolve the highlighted item through
    // CeCompletion_CurrentItem(); without this sync they would fall back to the
    // caret word (often empty or a partial prefix on a watch line) and show no
    // content for the item the user is actually looking at.
    CeCompletion_SetCurrentItem(item->label);
    // The pane under the list is a single line: the keyword followed by its
    // signature (blue).  The note is not repeated below the list; it lives in
    // the reference pane (Ctrl+Alt+Shift+H).
    if (item->detail[0] == '\0') {
        CeCompletion_HideTip();
        return;
    }
    CeCompletion_ShowTipWithSyntax(item->label, item->detail, false, "", item->kind);
    // Keep the reference pane on the item the user is looking at.
    CeCompletion_FollowDocWindow(item->label);
}

void CeLspFeatures_ShowCurrentItemTip(void)
{
    if (!CeLspFeatures_IsActive() || !SciCall_AutoCActive()) {
        return;
    }
    char name[256] = { 0 };
    if (SciCall_AutoCGetCurrentText(name) > 0) {
        CeLspFeatures_OnAutoCSelectionChange(name);
    }
}

void CeLspFeatures_OnAutoCCancelled(void)
{
    CeCompletion_HideTip();
    CeCompletion_SetCurrentItem(NULL); // never let a stale LSP item leak into the reference pane
    s_lspList = false;
    s_pending = false; // a cancelled list must never apply a snippet
    CeLspClient_CancelPending(CELSP_REQ_COMPLETION);
}

void CeLspFeatures_OnAutoCSelection(const char* text, int fillUpChar)
{
    UNREFERENCED_PARAMETER(fillUpChar);
    CeLspClient_CancelCompletionDebounce();
    CeCompletion_HideTip();
    Sci_CallTipCancelEx();
    if (text && *text) {
        _Copy(s_pendingName, sizeof(s_pendingName), text);
        s_pending = true;
    }
    else {
        s_pending = false;
    }
}

static void _ApplyInsertText(void)
{
    const CELSP_ITEM* const item = _FindItem(s_pendingName);
    if (!item) {
        return;
    }
    char expanded[1024];
    int caret = -1;
    _ExpandSnippet(item->insert, expanded, sizeof(expanded), &caret);
    if (strcmp(expanded, item->label) == 0) {
        return; // Scintilla already inserted exactly this text
    }
    const DocPos start = s_wordStart;
    const DocPos end = start + (DocPos)strlen(item->label);
    const DocPos docEnd = Sci_GetDocEndPosition();
    if ((start < 0) || (end > docEnd) || (start > end)) {
        return;
    }
    SciCall_BeginUndoAction();
    SciCall_SetTargetRange(start, end);
    SciCall_ReplaceTarget(-1, expanded);
    SciCall_EndUndoAction();
    const DocPos newCaret = start + (DocPos)caret;
    SciCall_SetSel(newCaret, newCaret);
    CeLspClient_OnDocumentChanged();
}

void CeLspFeatures_OnAutoCCompleted(const char* text, int fillUpChar)
{
    UNREFERENCED_PARAMETER(fillUpChar);
    CeLspClient_CancelCompletionDebounce();
    // Code blocks are merged into the LSP list as local extra words, but their
    // multi-line body is owned by CeCompletion.  Expand it here instead of
    // applying the LSP item's (identical, single-word) insertText.  This must
    // run even without a pending selection: a single-item list can complete
    // without SCN_AUTOCSELECTION.
    if ((text && *text) && CeCompletion_IsCodeBlockName(text) &&
        CeCompletion_InsertCodeBlock(text)) {
        s_pending = false;
        CeLspClient_OnDocumentChanged(); // the skeleton changed the document
        return;
    }
    if (!s_pending) {
        return;
    }
    s_pending = false;
    _ApplyInsertText();
}

void CeLspFeatures_FlushPendingCompletion(void)
{
    if (!s_pending || SciCall_AutoCActive()) {
        return;
    }
    s_pending = false;
    if (CeCompletion_IsCodeBlockName(s_pendingName) &&
        CeCompletion_InsertCodeBlock(s_pendingName)) {
        CeLspClient_OnDocumentChanged();
        return;
    }
    _ApplyInsertText();
}

void CeLspFeatures_OnUpdateUI(void)
{
    if (!SciCall_AutoCActive()) {
        CeCompletion_HideTip();
        if (s_lspList) {
            s_lspList = false;
            CeLspClient_CancelPending(CELSP_REQ_COMPLETION);
        CeLspClient_CancelCompletionDebounce();
        }
    }
    CeLspFeatures_FlushPendingCompletion();

    // Caret-debounced documentHighlight: request 300 ms after the caret settles.
    if (!SciCall_AutoCActive() && CeLspFeatures_IsActive() &&
        CeLspClient_HasCap(CELSP_CAP_DOCHIGHLIGHT)) {
        const DocPos pos = SciCall_GetCurrentPos();
        if (pos != s_lastHlPos) {
            s_lastHlPos = pos;
            if (Globals.hwndMain) {
                SetTimer(Globals.hwndMain, IDT_TIMER_CELSP_HILITE, 300, NULL);
            }
        }
    }
}

void CeLspFeatures_OnHighlightTimer(void)
{
    if (Globals.hwndMain) {
        KillTimer(Globals.hwndMain, IDT_TIMER_CELSP_HILITE);
    }
    if (CeLspFeatures_IsActive() && !SciCall_AutoCActive() &&
        CeLspClient_HasCap(CELSP_CAP_DOCHIGHLIGHT)) {
        CeLspClient_RequestPositional("textDocument/documentHighlight", CELSP_REQ_DOCHIGHLIGHT);
    }
}

bool CeLspFeatures_OnDwellStart(DocPos position)
{
    if (!CeLspFeatures_IsActive() || (position < 0) || SciCall_AutoCActive()) {
        return false;
    }
    // SCN_DWELLSTART reports the position between two characters.  When the
    // mouse sits on the right half of a word's last character the position is
    // already past the word, the server resolves an empty name and answers
    // null - the tip then silently never appears.  Step back onto the word.
    DocPos pos = position;
    if ((pos > 0) && !CeLspPos_IsIdentChar((unsigned char)SciCall_GetCharAt(pos)) &&
        CeLspPos_IsIdentChar((unsigned char)SciCall_GetCharAt(pos - 1))) {
        --pos;
    }
    s_hoverPos = pos;
    return CeLspClient_RequestHover(pos);
}

void CeLspFeatures_OnDwellEnd(void)
{
    CeCompletion_HideTip();
}

void CeLspFeatures_OnDocumentOpened(void)
{
    CeLint_ClearAnnotations(); // hints belong to the previous document
    _ClearDecorations();
    if (CeLspFeatures_IsEnabled()) {
        CeLspClient_OnDocumentOpened();
    }
}

void CeLspFeatures_OnDocumentChanged(void)
{
    if (CeLspFeatures_IsEnabled()) {
        CeLspClient_OnDocumentChanged();
    }
}

void CeLspFeatures_OnDocumentSaved(void)
{
    if (CeLspFeatures_IsEnabled()) {
        CeLspClient_OnDocumentSaved();
    }
}

void CeLspFeatures_OnDocumentClosed(void)
{
    _ClearDecorations();
    if (CeLspFeatures_IsEnabled()) {
        CeLspClient_OnDocumentClosed();
    }
}

// ----------------------------------------------------------------------------
// Server message handlers
// ----------------------------------------------------------------------------

// True when the caret line is an np3watch directive comment.  Completion in
// that context is dominated by the watch vocabulary, so the LSP list gets the
// same boost the local list applies through CeCompletion_WatchRank().
static bool _CaretOnWatchLine(void)
{
    const DocPos pos = SciCall_GetCurrentPos();
    const DocLn line = SciCall_LineFromPosition(pos);
    const DocPos start = SciCall_PositionFromLine(line);
    const DocPos end = SciCall_GetLineEndPosition(line);
    if (end <= start) {
        return false;
    }
    const size_t len = (size_t)(end - start);
    const char* const text = SciCall_GetRangePointer(start, (DocPos)len);
    if (!text) {
        return false;
    }
    size_t i = 0u;
    while ((i < len) && ((text[i] == ' ') || (text[i] == '\t'))) {
        ++i;
    }
    if (((i + 1u) < len) && (text[i] == '/') && (text[i + 1u] == '/')) {
        i += 2u;
    }
    else if ((i < len) && (text[i] == ';')) {
        ++i;
    }
    else {
        return false;
    }
    while ((i < len) && ((text[i] == ' ') || (text[i] == '\t'))) {
        ++i;
    }
    const size_t s = i;
    while ((i < len) && (((text[i] >= 'a') && (text[i] <= 'z')) ||
                         ((text[i] >= 'A') && (text[i] <= 'Z')) ||
                         ((text[i] >= '0') && (text[i] <= '9')) || (text[i] == '_'))) {
        ++i;
    }
    char word[16];
    const size_t wl = i - s;
    if ((wl == 0u) || (wl >= sizeof(word))) {
        return false;
    }
    memcpy(word, text + s, wl);
    word[wl] = '\0';
    return (_stricmp(word, "np3watch") == 0) ||
           (_stricmp(word, "np3unwatch") == 0) ||
           (_stricmp(word, "np3nowatch") == 0);
}

void CeLspFeatures_OnCompletion(const CELSP_JSON* result, const CELSP_PENDING* req)
{
    if (!CeLspFeatures_IsActive() || !result || !req) {
        return;
    }
    const CELSP_JSON* items = CeLspJson_Get(result, "items");
    if (!items && (CeLspJson_Type(result) == CELSP_JSON_ARRAY)) {
        items = result;
    }
    // The server only serves the generated catalogue.  The bundled common
    // variable dictionary and an external dictionary live on the client side,
    // so merge them here - otherwise enabling the language server silently
    // removes playerMoney & Co. from the list.
    CE_WORD_HINT extra[64];
    int const extraCount = CeCompletion_CollectExtraWords(req->root, extra, (int)COUNTOF(extra));

    const size_t count = CeLspJson_Count(items);
    const int limit = (count > CELSP_ITEM_LIMIT) ? CELSP_ITEM_LIMIT : (int)count;
    int const needed = limit + extraCount;
    if (needed > s_itemCap) {
        CELSP_ITEM* const grown = (CELSP_ITEM*)realloc(s_items, (size_t)needed * sizeof(CELSP_ITEM));
        if (!grown) {
            return;
        }
        s_items = grown;
        s_itemCap = needed;
    }
    s_itemCount = 0;
    s_listLen = 0u;
    if (s_list) {
        s_list[0] = '\0';
    }

    for (int i = 0; i < limit; ++i) {
        const CELSP_JSON* const item = CeLspJson_At(items, (size_t)i);
        if (!item) {
            continue;
        }
        const char* const label = CeLspJson_GetString(item, "label");
        if (!label || !*label || strchr(label, ' ') || strchr(label, '?')) {
            continue; // AutoC separator / type separator
        }
        CELSP_ITEM* const dst = &s_items[s_itemCount];
        // Every field is written below: _Copy() NUL-terminates each buffer and
        // kind/snippet are assigned, so a 3 KB-per-item memset is pure waste on
        // a hot path that can reach 1200 items.
        _Copy(dst->label, sizeof(dst->label), label);
        const char* const insert = CeLspJson_GetString(item, "insertText");
        _Copy(dst->insert, sizeof(dst->insert), insert ? insert : label);
        _Copy(dst->detail, sizeof(dst->detail), CeLspJson_GetString(item, "detail"));
        _Copy(dst->doc, sizeof(dst->doc), _ItemText(CeLspJson_Get(item, "documentation")));
        dst->kind = _LspKindToCe(CeLspJson_GetInt(item, "kind"));
        dst->snippet = (CeLspJson_GetInt(item, "insertTextFormat") == 2);
        dst->order = i; // server order, the tie breaker of the ranking below
        ++s_itemCount;
    }

    for (int i = 0; (i < extraCount) && (s_itemCount < needed); ++i) {
        const CE_WORD_HINT* const word = &extra[i];
        bool duplicate = false;
        for (int j = 0; j < s_itemCount; ++j) {
            if (_stricmp(s_items[j].label, word->name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        CELSP_ITEM* const dst = &s_items[s_itemCount];
        _Copy(dst->label, sizeof(dst->label), word->name);
        _Copy(dst->insert, sizeof(dst->insert), word->name);
        _Copy(dst->detail, sizeof(dst->detail), word->detail);
        dst->doc[0] = '\0';
        dst->kind = word->kind;
        dst->snippet = false;
        dst->order = s_itemCount;
        ++s_itemCount;
    }

    if (s_itemCount == 0) {
        CeCompletion_HideTip();
        SciCall_AutoCCancel();
        s_lspList = false;
        return;
    }

    // The server answers in generated-catalogue order, so "mo" inside an
    // Auto Assembler section listed mono_* Lua API calls before "mov".  Rank by
    // the language at the caret and keep the server order for equal weights.
    int const mode = CeCompletion_CurrentMode();
    const bool watchLine = (mode == CE_LANG_AA) && _CaretOnWatchLine();
    for (int i = 0; i < s_itemCount; ++i) {
        s_items[i].weight = CeCompletion_ContextRank(s_items[i].label, s_items[i].kind, mode);
        if (watchLine) {
            s_items[i].weight += CeCompletion_WatchRank(s_items[i].label);
        }
    }
    if (s_itemCount > 1) {
        qsort(s_items, (size_t)s_itemCount, sizeof(CELSP_ITEM), _CompareItem);
    }

    if (!_ListEnsure(2u)) {
        CeCompletion_HideTip();
        SciCall_AutoCCancel();
        s_lspList = false;
        return;
    }
    s_list[0] = ' ';
    s_list[1] = '\0';
    s_listLen = 1u;
    int emitted = 0;
    for (int i = 0; i < s_itemCount; ++i) {
        const CELSP_ITEM* const item = &s_items[i];
        const int kindId = ((item->kind > CE_KIND_NONE) && (item->kind < CE_KIND_COUNT)) ?
                           item->kind : CE_KIND_NONE;
        char suffix[8];
        const int n = snprintf(suffix, sizeof(suffix), "?%d", kindId);
        if (n <= 0) {
            break;
        }
        // Separator goes *between* items: a suffix like "?N " used to leave a
        // trailing space at the end of the list, which Scintilla renders as an
        // empty final row.
        if ((i > 0) && !_ListAdd(" ", 1u)) {
            break;
        }
        if (!_ListAdd(item->label, strlen(item->label)) || !_ListAdd(suffix, (size_t)n)) {
            break;
        }
        ++emitted;
    }
    s_itemCount = emitted;
    s_wordStart = req->wordStart;
    _Copy(s_root, sizeof(s_root), req->root);
    SciCall_AutoCCancel();
    SciCall_AutoCSetSeparator(' ');
    SciCall_AutoCSetTypeSeparator('?');
    SciCall_AutoCSetOrder(SC_ORDER_CUSTOM);
    SciCall_AutoCSetChooseSingle(false);
    SciCall_AutoCSetIgnoreCase(true);
    SciCall_AutoCSetAutoHide(false);
    SciCall_AutoCSetDropRestOfWord(true);
    SciCall_AutoCShow((DocPos)strlen(s_root), &s_list[1]);
    s_lspList = SciCall_AutoCActive();
    if (s_lspList && (SciCall_AutoCGetCurrent() < 0)) {
        SciCall_AutoCSelect("");
    }
    CeLspFeatures_ShowCurrentItemTip();
}

void CeLspFeatures_OnSignatureHelp(const CELSP_JSON* result)
{
    if (!CeLspFeatures_IsActive() || !result) {
        return;
    }
    const CELSP_JSON* const signatures = CeLspJson_Get(result, "signatures");
    const CELSP_JSON* const first = CeLspJson_At(signatures, 0u);
    const char* const label = CeLspJson_GetString(first, "label");
    if (!label || !*label) {
        SciCall_CallTipCancel();
        return;
    }
    SciCall_AutoCCancel();
    s_lspList = false;
    CeCompletion_HideTip();
    const DocPos pos = SciCall_GetCurrentPos();
    SciCall_CallTipShow((pos > 0) ? (pos - 1) : 0, label);
}

// The server answers hover with "### name\n\n`sig`\n\n<note>".  The pane shows
// the name in the head line, paints the signature right after it (blue), and
// keeps the note as the body.
static void _SplitHoverText(const char* value, char* head, size_t cchHead,
                            char* syntax, size_t cchSyntax, char* body, size_t cchBody)
{
    head[0] = '\0';
    syntax[0] = '\0';
    body[0] = '\0';
    const char* p = value;
    while ((p && (*p == '\n'))) { ++p; }
    if (!p) {
        return;
    }
    // Head: the first line, without the markdown heading marks.
    if ((p[0] == '#') && (p[1] == '#')) {
        p += 2;
        while (*p == '#') { ++p; }
        while ((*p == ' ') || (*p == '\t')) { ++p; }
    }
    const char* eol = strchr(p, '\n');
    size_t const headLen = eol ? (size_t)(eol - p) : strlen(p);
    StringCchCopyNA(head, cchHead, p, headLen);
    // The head is short: "CE API · name" reads better than a bare name.
    char name[256];
    StringCchCopyA(name, COUNTOF(name), head);
    StringCchPrintfA(head, cchHead, "CE API  \xC2\xB7  %s", name);

    p = eol ? (eol + 1) : (p + headLen);
    // Syntax: the first backticked line.
    while (*p) {
        const char* line = p;
        const char* lineEnd = strchr(line, '\n');
        size_t const lineLen = lineEnd ? (size_t)(lineEnd - line) : strlen(line);
        if ((lineLen > 2) && (line[0] == '`') && (line[lineLen - 1] == '`')) {
            StringCchCopyNA(syntax, cchSyntax, line + 1, lineLen - 2);
            p = lineEnd ? (lineEnd + 1) : (line + lineLen);
            break;
        }
        if (!lineEnd) {
            p = line + lineLen;
            break;
        }
        p = lineEnd + 1;
    }
    while ((*p == '\n') || (*p == '\r')) { ++p; }
    StringCchCopyA(body, cchBody, p);
}

void CeLspFeatures_OnHover(const CELSP_JSON* result)
{
    if (!CeLspFeatures_IsActive()) {
        return;
    }
    const char* const value = result ? _ItemText(CeLspJson_Get(result, "contents")) : NULL;
    if (value && *value) {
        char head[512];
        char syntax[512];
        char body[4096];
        _SplitHoverText(value, head, COUNTOF(head), syntax, COUNTOF(syntax), body, COUNTOF(body));
        CeCompletion_ShowTipWithSyntax(head, syntax, false, body, CE_KIND_FUNCTION);
        return;
    }
    // The server returns null for names it does not know (AA labels, Lua
    // locals, user defines).  Fall back to the local catalogue / notes so that
    // hovering a keyword always produces a hint; an entry without a catalogue
    // signature is painted red to show it is not part of the CE API.
    char word[128];
    if (_WordAtPosition(s_hoverPos, word, COUNTOF(word))) {
        const CE_COMPLETION_ENTRY* const pEntry = CeCompletion_Find(word);
        const char* const pszDoc = CeCompletion_FindDoc(word);
        if (pEntry || !StrIsEmptyA(pszDoc)) {
            char head[512];
            char syntax[512];
            char body[2048];
            if (pEntry) {
                StringCchPrintfA(head, COUNTOF(head), "%s  \xC2\xB7  %s",
                                 pEntry->category, pEntry->name);
                StringCchCopyA(syntax, COUNTOF(syntax), pEntry->signature);
                char* const sep = strstr(syntax, " -- ");
                if (sep) {
                    *sep = '\0';
                }
                StringCchCopyA(body, COUNTOF(body), StrIsEmptyA(pszDoc) ? "" : pszDoc);
            }
            else {
                StringCchPrintfA(head, COUNTOF(head), "\xE8\xAF\x8D\xE8\xA1\xA8  \xC2\xB7  %s", word);
                StringCchCopyA(syntax, COUNTOF(syntax), word);
                StringCchCopyA(body, COUNTOF(body), pszDoc);
            }
            CeCompletion_ShowTipWithSyntax(head, syntax, (pEntry == NULL), body,
                                           pEntry ? pEntry->kind : CE_KIND_VARIABLE);
            return;
        }
    }
    CeCompletion_HideTip();
}

#define LSP_DIAG_LIMIT 512
#define LSP_DIAG_MSG_MAX 256

typedef struct LSP_DIAG
{
    int  line;
    int  character;
    int  endLine;
    int  endCharacter;
    int  severity;
    char source[64];
    char message[LSP_DIAG_MSG_MAX];
} LSP_DIAG;

static LSP_DIAG s_diags[LSP_DIAG_LIMIT];
static int     s_diagCount = 0;
static HWND    s_hDiagWnd = NULL;

static void _DiagRefresh(HWND hwnd);

// Server diagnostics (syntax / semantic / style findings from the built-in
// server, including the Lua and asm checkers) render as margin markers plus a
// squiggle indicator.  CeLint keeps its own EOL annotations: the two producers
// use disjoint Scintilla resources, so they never clear each other.
void CeLspFeatures_OnDiagnostics(const CELSP_JSON* params)
{
    _EnsureLspDecorInit();
    SciCall_MarkerDeleteAll(MARKER_LSP_ERROR);
    SciCall_MarkerDeleteAll(MARKER_LSP_WARNING);
    SciCall_SetIndicatorCurrent(INDIC_NP3_LSP_DIAG);
    SciCall_IndicatorClearRange(0, SciCall_GetLength());
    s_diagCount = 0;

    if (!params) {
        if (s_hDiagWnd && IsWindow(s_hDiagWnd)) {
            _DiagRefresh(s_hDiagWnd);
        }
        return;
    }
    const CELSP_JSON* const diags = CeLspJson_Get(params, "diagnostics");
    const size_t n = CeLspJson_Count(diags);
    for (size_t i = 0u; (i < n) && (s_diagCount < LSP_DIAG_LIMIT); ++i) {
        const CELSP_JSON* const d = CeLspJson_At(diags, i);
        const CELSP_JSON* const range = CeLspJson_Get(d, "range");
        const CELSP_JSON* const start = CeLspJson_Get(range, "start");
        const CELSP_JSON* const end = CeLspJson_Get(range, "end");
        if (!start || !end) {
            continue;
        }
        LSP_DIAG* const item = &s_diags[s_diagCount++];
        item->line = CeLspJson_GetInt(start, "line");
        item->character = CeLspJson_GetInt(start, "character");
        item->endLine = CeLspJson_GetInt(end, "line");
        item->endCharacter = CeLspJson_GetInt(end, "character");
        item->severity = CeLspJson_GetInt(d, "severity");
        _Copy(item->source, sizeof(item->source), CeLspJson_GetString(d, "source"));
        _Copy(item->message, sizeof(item->message), CeLspJson_GetString(d, "message"));
        if (!Settings2.CeLspDocDiagnostics ||
            !CeLspClient_DiagnosticSeverityVisible(item->severity)) {
            continue;
        }
        DocPos a = 0;
        DocPos b = 0;
        if (!_ParseRange(range, &a, &b)) {
            continue;
        }
        if (b <= a) {
            b = a + 1;
        }
        const DocLn line = SciCall_LineFromPosition(a);
        SciCall_MarkerAdd(line, (item->severity == 1) ? MARKER_LSP_ERROR : MARKER_LSP_WARNING);
        SciCall_SetIndicatorCurrent(INDIC_NP3_LSP_DIAG);
        SciCall_IndicatorFillRange(a, b - a);
    }
    if (s_hDiagWnd && IsWindow(s_hDiagWnd)) {
        _DiagRefresh(s_hDiagWnd);
    }
}

void CeLspFeatures_OnLogMessage(const char* message, int level)
{
    _Log(message, level);
}

void CeLspFeatures_OnServerStatus(int status, const char* message)
{
    switch (status) {
    case CELSP_STATUS_READY:
        _Log("LSP: server ready (CE code hints active)", 3);
        break;
    case CELSP_STATUS_EXITED:
        _Log("LSP: server stopped; restarting or falling back to local completion", 2);
        CeCompletion_HideTip();
        s_lspList = false;
        s_lastHlPos = -1;
        _ClearDecorations();
        break;
    case CELSP_STATUS_ERROR:
        _Log(message ? message : "LSP: error", 1);
        break;
    default:
        break;
    }
}

// ============================================================================
// Navigation / symbols / formatting / decorations
// ============================================================================

#define LSP_SYM_LIMIT   1024
#define LSP_ANN_LIMIT   256
#define LSP_LINK_LIMIT  128
#define LSP_LOC_LIMIT   512

typedef struct LSP_SYM
{
    char name[192];
    char kindText[32];
    int  line;
    int  character;
    int  endLine;
    int  endChar;
} LSP_SYM;

typedef struct LSP_ANN
{
    DocLn line;
    char  text[200];
} LSP_ANN;

typedef struct LSP_LINK
{
    DocPos start;
    DocPos end;
    char   target[256];
} LSP_LINK;

static LSP_SYM  s_symbols[LSP_SYM_LIMIT];
static int      s_symbolCount = 0;
static LSP_ANN  s_hints[LSP_ANN_LIMIT];
static int      s_hintCount = 0;
static LSP_ANN  s_lenses[LSP_ANN_LIMIT];
static int      s_lensCount = 0;
static LSP_LINK s_links[LSP_LINK_LIMIT];
static int      s_linkCount = 0;

static char* _DupStr(const char* text)
{
    if (!text) {
        return NULL;
    }
    const size_t n = strlen(text);
    char* const out = (char*)malloc(n + 1u);
    if (out) {
        memcpy(out, text, n);
        out[n] = '\0';
    }
    return out;
}

static void _Utf8ToWide(const char* utf8, WCHAR* dst, size_t cap)
{
    dst[0] = L'\0';
    if (!utf8 || (cap == 0u)) {
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, dst, (int)cap);
    dst[cap - 1u] = L'\0';
}

// LSP Range -> document byte offsets through the client's synced snapshot.
static bool _ParseRange(const CELSP_JSON* range, DocPos* pStart, DocPos* pEnd)
{
    if (!range || !pStart || !pEnd) {
        return false;
    }
    const CELSP_JSON* const st = CeLspJson_Get(range, "start");
    if (!st) {
        return false;
    }
    DocPos a = 0;
    DocPos b = 0;
    if (!CeLspClient_PosToOffset(CeLspJson_GetInt(st, "line"), CeLspJson_GetInt(st, "character"), &a)) {
        return false;
    }
    const CELSP_JSON* const en = CeLspJson_Get(range, "end");
    if (en) {
        CeLspClient_PosToOffset(CeLspJson_GetInt(en, "line"), CeLspJson_GetInt(en, "character"), &b);
    }
    if (b < a) {
        b = a;
    }
    *pStart = a;
    *pEnd = b;
    return true;
}

// Location {uri,range} or LocationLink {targetUri,targetSelectionRange,targetRange}
static const CELSP_JSON* _LocationRange(const CELSP_JSON* loc)
{
    const CELSP_JSON* const r = CeLspJson_Get(loc, "range");
    if (r) {
        return r;
    }
    const CELSP_JSON* const sel = CeLspJson_Get(loc, "targetSelectionRange");
    return sel ? sel : CeLspJson_Get(loc, "targetRange");
}

static void _EnsureLspDecorInit(void)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    // The bookmark margin shows every marker except fold/history ones, so the
    // LSP markers are visible without touching the margin mask.
    SciCall_MarkerDefine(MARKER_LSP_ERROR, SC_MARK_CIRCLE);
    SciCall_MarkerSetFore(MARKER_LSP_ERROR, RGB(0xD0, 0x30, 0x30));
    SciCall_MarkerDefine(MARKER_LSP_WARNING, SC_MARK_SMALLRECT);
    SciCall_MarkerSetFore(MARKER_LSP_WARNING, RGB(0xD0, 0x90, 0x20));
    SciCall_IndicSetStyle(INDIC_NP3_LSP_DIAG, INDIC_SQUIGGLE);
    SciCall_IndicSetFore(INDIC_NP3_LSP_DIAG, RGB(0xE0, 0x60, 0x40));
    SciCall_IndicSetStyle(INDIC_NP3_LSP_HILITE, INDIC_ROUNDBOX);
    SciCall_IndicSetFore(INDIC_NP3_LSP_HILITE, RGB(0x80, 0xA8, 0xD0));
    SciCall_IndicSetAlpha(INDIC_NP3_LSP_HILITE, 60);
}

static void _ClearDecorations(void)
{
    s_hintCount = 0;
    s_lensCount = 0;
    s_linkCount = 0;
    SciCall_AnnotationClearAll();
    SciCall_SetIndicatorCurrent(INDIC_NP3_LSP_HILITE);
    SciCall_IndicatorClearRange(0, SciCall_GetLength());
    SciCall_SetIndicatorCurrent(INDIC_NP3_LSP_DIAG);
    SciCall_IndicatorClearRange(0, SciCall_GetLength());
    SciCall_MarkerDeleteAll(MARKER_LSP_ERROR);
    SciCall_MarkerDeleteAll(MARKER_LSP_WARNING);
    s_diagCount = 0;
    if (s_hDiagWnd && IsWindow(s_hDiagWnd)) {
        _DiagRefresh(s_hDiagWnd);
    }
}

// ----------------------------------------------------------------------------
// definition / declaration / references
// ----------------------------------------------------------------------------

void CeLspFeatures_GotoDefinition(void)
{
    if (!CeLspFeatures_IsActive()) {
        _Log("LSP: server not ready", 2);
        return;
    }
    if (!CeLspClient_HasCap(CELSP_CAP_DEFINITION)) {
        _Log("LSP: definition not supported", 2);
        return;
    }
    CeLspClient_RequestPositional("textDocument/definition", CELSP_REQ_DEFINITION);
}

void CeLspFeatures_GotoDeclaration(void)
{
    if (!CeLspFeatures_IsActive()) {
        _Log("LSP: server not ready", 2);
        return;
    }
    if (!CeLspClient_HasCap(CELSP_CAP_DECLARATION)) {
        _Log("LSP: declaration not supported", 2);
        return;
    }
    CeLspClient_RequestPositional("textDocument/declaration", CELSP_REQ_DECLARATION);
}

void CeLspFeatures_FindReferences(void)
{
    if (!CeLspFeatures_IsActive()) {
        _Log("LSP: server not ready", 2);
        return;
    }
    if (!CeLspClient_HasCap(CELSP_CAP_REFERENCES)) {
        _Log("LSP: references not supported", 2);
        return;
    }
    CeLspClient_RequestReferences();
}

void CeLspFeatures_OnLocations(const CELSP_JSON* result, int kind)
{
    if (!CeLspFeatures_IsActive() || !result) {
        return;
    }
    static DocPos locStart[LSP_LOC_LIMIT];
    static DocPos locEnd[LSP_LOC_LIMIT];
    int count = 0;
    const size_t n = CeLspJson_Count(result);
    for (size_t i = 0u; (i < n) && (count < LSP_LOC_LIMIT); ++i) {
        DocPos a = 0;
        DocPos b = 0;
        if (_ParseRange(_LocationRange(CeLspJson_At(result, i)), &a, &b)) {
            locStart[count] = a;
            locEnd[count] = b;
            ++count;
        }
    }
    if (count == 0) {
        _Log("LSP: no location found", 2);
        return;
    }
    char msg[96];
    if (kind == CELSP_REQ_REFERENCES) {
        // SETSEL resets the selection set; ADDSELECTION appends the rest.
        SciCall_SetSel(locStart[0], locEnd[0]);
        for (int i = 1; i < count; ++i) {
            SciCall_AddSelection(locStart[i], locEnd[i]);
        }
        snprintf(msg, sizeof(msg), "LSP: %d reference%s", count, (count == 1) ? "" : "s");
    }
    else {
        SciCall_GotoPos(locStart[0]);
        SciCall_SetSel(locStart[0], locEnd[0]);
        snprintf(msg, sizeof(msg), "LSP: jumped to %s (%d result%s)",
                 (kind == CELSP_REQ_DEFINITION) ? "definition" : "declaration",
                 count, (count == 1) ? "" : "s");
    }
    _Log(msg, 3);
}

// ----------------------------------------------------------------------------
// documentSymbol / workspaceSymbol: non-modal list window
// ----------------------------------------------------------------------------

#define LSP_SYM_CLASS      L"CeLspSymbolList"
#define IDC_LSP_SYM_FILTER 901
#define IDC_LSP_SYM_LIST   902
#define LSP_DIAG_CLASS     L"CeLspDiagnosticList"
#define IDC_LSP_DIAG_LIST  903

static HWND s_hSymWnd = NULL;

// The built-in server emits its own SymbolKind table for both documentSymbol
// and workspace/symbol: 12=Function, 13=Variable, 14=Constant, 5=Type,
// 20=everything else (keyword / AA / ASM / SECTION).
static const char* _SymKindText(int kind)
{
    switch (kind) {
    case 5: return "Type";
    case 12: return "Function";
    case 13: return "Variable";
    case 14: return "Constant";
    case 1: case 2: return "Module";
    default: return "Symbol";
    }
}

static void _AddSymbol(const CELSP_JSON* sym)
{
    if (!sym || (s_symbolCount >= LSP_SYM_LIMIT)) {
        return;
    }
    const char* const name = CeLspJson_GetString(sym, "name");
    if (!name || !*name) {
        return;
    }
    const CELSP_JSON* rng = CeLspJson_Get(sym, "selectionRange");
    if (!rng) {
        rng = CeLspJson_Get(sym, "range");
    }
    if (!rng) {
        rng = CeLspJson_Get(CeLspJson_Get(sym, "location"), "range");
    }
    const CELSP_JSON* const st = CeLspJson_Get(rng, "start");
    if (!st) {
        return;
    }
    LSP_SYM* const s = &s_symbols[s_symbolCount];
    _Copy(s->name, sizeof(s->name), name);
    _Copy(s->kindText, sizeof(s->kindText), _SymKindText(CeLspJson_GetInt(sym, "kind")));
    s->line = CeLspJson_GetInt(st, "line");
    s->character = CeLspJson_GetInt(st, "character");
    const CELSP_JSON* const en = CeLspJson_Get(rng, "end");
    s->endLine = en ? CeLspJson_GetInt(en, "line") : s->line;
    s->endChar = en ? CeLspJson_GetInt(en, "character") : s->character;
    ++s_symbolCount;
}

static void _SymRefresh(HWND hwnd)
{
    HWND const hList = GetDlgItem(hwnd, IDC_LSP_SYM_LIST);
    if (!hList) {
        return;
    }
    wchar_t wfilter[256] = { 0 };
    GetDlgItemTextW(hwnd, IDC_LSP_SYM_FILTER, wfilter, (int)COUNTOF(wfilter));
    CharUpperW(wfilter);
    SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hList);
    int shown = 0;
    for (int i = 0; i < s_symbolCount; ++i) {
        wchar_t wname[256] = { 0 };
        _Utf8ToWide(s_symbols[i].name, wname, COUNTOF(wname));
        if (wfilter[0]) {
            wchar_t up[256] = { 0 };
            StringCchCopyW(up, COUNTOF(up), wname);
            CharUpperW(up);
            if (!wcsstr(up, wfilter)) {
                continue;
            }
        }
        LVITEMW li = { 0 };
        li.mask = LVIF_TEXT | LVIF_PARAM;
        li.iItem = shown;
        li.pszText = wname;
        li.lParam = (LPARAM)i;
        if (ListView_InsertItem(hList, &li) >= 0) {
            wchar_t wkind[64] = { 0 };
            _Utf8ToWide(s_symbols[i].kindText, wkind, COUNTOF(wkind));
            ListView_SetItemText(hList, shown, 1, wkind);
            wchar_t wline[24];
            StringCchPrintfW(wline, COUNTOF(wline), L"%d", s_symbols[i].line + 1);
            ListView_SetItemText(hList, shown, 2, wline);
            ++shown;
        }
    }
    wchar_t wtitle[80];
    StringCchPrintfW(wtitle, COUNTOF(wtitle), L"CE 符号 (%d/%d)", shown, s_symbolCount);
    SetWindowTextW(hwnd, wtitle);
    SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, TRUE);
}

static void _SymJump(HWND hwnd)
{
    HWND const hList = GetDlgItem(hwnd, IDC_LSP_SYM_LIST);
    int const item = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (item < 0) {
        return;
    }
    LVITEMW li = { 0 };
    li.mask = LVIF_PARAM;
    li.iItem = item;
    if (!ListView_GetItem(hList, &li)) {
        return;
    }
    int const idx = (int)li.lParam;
    if ((idx < 0) || (idx >= s_symbolCount)) {
        return;
    }
    DocPos a = 0;
    DocPos b = 0;
    if (CeLspClient_PosToOffset(s_symbols[idx].line, s_symbols[idx].character, &a)) {
        if (!CeLspClient_PosToOffset(s_symbols[idx].endLine, s_symbols[idx].endChar, &b) || (b < a)) {
            b = a;
        }
        SciCall_GotoPos(a);
        SciCall_SetSel(a, b);
        if (Globals.hwndMain) {
            SetFocus(Globals.hwndEdit);
        }
    }
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK _SymWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        InitWindowCommon(hwnd, true);
        HINSTANCE const hInst = GetModuleHandleW(NULL);
        HFONT const hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND const hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                           6, 6, 556, 24, hwnd,
                                           (HMENU)IDC_LSP_SYM_FILTER, hInst, NULL);
        HWND const hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                           WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                           6, 36, 556, 286, hwnd,
                                           (HMENU)IDC_LSP_SYM_LIST, hInst, NULL);
        if (hEdit) {
            SendMessageW(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        }
        if (hList) {
            ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW col = { 0 };
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPWSTR)L"名称";
            col.cx = 340;
            ListView_InsertColumn(hList, 0, &col);
            col.pszText = (LPWSTR)L"类型";
            col.cx = 110;
            ListView_InsertColumn(hList, 1, &col);
            col.pszText = (LPWSTR)L"行号";
            col.cx = 70;
            ListView_InsertColumn(hList, 2, &col);
            SendMessageW(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        }
        return 0;
    }
    case WM_COMMAND:
        if ((HIWORD(wp) == EN_CHANGE) && (LOWORD(wp) == IDC_LSP_SYM_FILTER)) {
            _SymRefresh(hwnd);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        const NMHDR* const nh = (const NMHDR*)lp;
        if (nh && (nh->idFrom == IDC_LSP_SYM_LIST)) {
            if (nh->code == NM_DBLCLK) {
                _SymJump(hwnd);
                return 0;
            }
            if (nh->code == LVN_KEYDOWN) {
                const NMLVKEYDOWN* const kd = (const NMLVKEYDOWN*)lp;
                if (kd->wVKey == VK_RETURN) {
                    _SymJump(hwnd);
                    return 0;
                }
            }
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (s_hSymWnd == hwnd) {
            s_hSymWnd = NULL;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void _ShowSymbolList(void)
{
    static bool s_classDone = false;
    if (!s_classDone) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = _SymWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = LSP_SYM_CLASS;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        if (!RegisterClassW(&wc)) {
            return;
        }
        s_classDone = true;
    }
    if (s_hSymWnd && IsWindow(s_hSymWnd)) {
        _SymRefresh(s_hSymWnd);
        SetForegroundWindow(s_hSymWnd);
        return;
    }
    s_hSymWnd = CreateWindowExW(WS_EX_TOOLWINDOW, LSP_SYM_CLASS, L"CE 符号",
                                WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 588, 360,
                                Globals.hwndMain, NULL, GetModuleHandleW(NULL), NULL);
    if (s_hSymWnd) {
        ShowWindow(s_hSymWnd, SW_SHOWNOACTIVATE);
        _SymRefresh(s_hSymWnd);
    }
}

static const WCHAR* _DiagUiText(const WCHAR* chinese, const WCHAR* english)
{
    const WCHAR* const locale = Globals.CurrentLngLocaleName;
    return (locale[0] == L'z' && locale[1] == L'h') ? chinese : english;
}

static void _DiagRefresh(HWND hwnd)
{
    HWND const hList = GetDlgItem(hwnd, IDC_LSP_DIAG_LIST);
    if (!hList) {
        return;
    }
    SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hList);
    for (int i = 0; i < s_diagCount; ++i) {
        const LSP_DIAG* const d = &s_diags[i];
        LVITEMW li = { 0 };
        li.mask = LVIF_TEXT | LVIF_PARAM;
        li.iItem = i;
        li.lParam = (LPARAM)i;
        wchar_t wseverity[24];
        wchar_t wsource[64];
        wchar_t wmessage[LSP_DIAG_MSG_MAX];
        wchar_t wpos[48];
        StringCchPrintfW(wseverity, COUNTOF(wseverity), L"%d", d->severity);
        _Utf8ToWide(d->source, wsource, COUNTOF(wsource));
        _Utf8ToWide(d->message, wmessage, COUNTOF(wmessage));
        StringCchPrintfW(wpos, COUNTOF(wpos), L"%d:%d", d->line + 1, d->character + 1);
        li.pszText = wseverity;
        if (ListView_InsertItem(hList, &li) >= 0) {
            ListView_SetItemText(hList, i, 1, wsource);
            ListView_SetItemText(hList, i, 2, wmessage);
            ListView_SetItemText(hList, i, 3, wpos);
        }
    }
    wchar_t wtitle[80];
    StringCchPrintfW(wtitle, COUNTOF(wtitle), L"%s (%d)",
                     _DiagUiText(L"CE 诊断", L"CE Diagnostics"), s_diagCount);
    SetWindowTextW(hwnd, wtitle);
    SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, TRUE);
}

static void _DiagJump(HWND hwnd)
{
    HWND const hList = GetDlgItem(hwnd, IDC_LSP_DIAG_LIST);
    int const item = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (item < 0) {
        return;
    }
    LVITEMW li = { 0 };
    li.mask = LVIF_PARAM;
    li.iItem = item;
    if (!ListView_GetItem(hList, &li)) {
        return;
    }
    const int idx = (int)li.lParam;
    if ((idx < 0) || (idx >= s_diagCount)) {
        return;
    }
    const LSP_DIAG* const d = &s_diags[idx];
    DocPos const a = SciCall_FindColumn((DocLn)d->line, (DocPos)d->character);
    DocPos b = SciCall_FindColumn((DocLn)d->endLine, (DocPos)d->endCharacter);
    if (b < a) {
        b = a;
    }
    SciCall_GotoPos(a);
    SciCall_SetSel(a, b);
    if (Globals.hwndMain) {
        SetFocus(Globals.hwndEdit);
    }
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK _DiagWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        InitWindowCommon(hwnd, true);
        HWND const hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                           WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                           6, 6, 760, 360, hwnd,
                                           (HMENU)IDC_LSP_DIAG_LIST, GetModuleHandleW(NULL), NULL);
        if (hList) {
            ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW col = { 0 };
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPWSTR)_DiagUiText(L"级别", L"Severity");
            col.cx = 60;
            ListView_InsertColumn(hList, 0, &col);
            col.pszText = (LPWSTR)_DiagUiText(L"来源", L"Source");
            col.cx = 130;
            ListView_InsertColumn(hList, 1, &col);
            col.pszText = (LPWSTR)_DiagUiText(L"说明", L"Message");
            col.cx = 500;
            ListView_InsertColumn(hList, 2, &col);
            col.pszText = (LPWSTR)_DiagUiText(L"行:列", L"Line:Char");
            col.cx = 70;
            ListView_InsertColumn(hList, 3, &col);
            SendMessageW(hList, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        }
        _DiagRefresh(hwnd);
        return 0;
    }
    case WM_NOTIFY: {
        const NMHDR* const nh = (const NMHDR*)lp;
        if (nh && (nh->idFrom == IDC_LSP_DIAG_LIST)) {
            if (nh->code == NM_DBLCLK) {
                _DiagJump(hwnd);
                return 0;
            }
            if (nh->code == LVN_KEYDOWN) {
                const NMLVKEYDOWN* const kd = (const NMLVKEYDOWN*)lp;
                if (kd->wVKey == VK_RETURN) {
                    _DiagJump(hwnd);
                    return 0;
                }
            }
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (s_hDiagWnd == hwnd) {
            s_hDiagWnd = NULL;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CeLspFeatures_ShowDiagnostics(void)
{
    static bool s_classDone = false;
    if (!s_classDone) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = _DiagWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = LSP_DIAG_CLASS;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        if (!RegisterClassW(&wc)) {
            return;
        }
        s_classDone = true;
    }
    if (s_hDiagWnd && IsWindow(s_hDiagWnd)) {
        _DiagRefresh(s_hDiagWnd);
        SetForegroundWindow(s_hDiagWnd);
        return;
    }
    s_hDiagWnd = CreateWindowExW(WS_EX_TOOLWINDOW, LSP_DIAG_CLASS,
                                 _DiagUiText(L"CE 诊断", L"CE Diagnostics"),
                                 WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 788, 420,
                                 Globals.hwndMain, NULL, GetModuleHandleW(NULL), NULL);
    if (s_hDiagWnd) {
        ShowWindow(s_hDiagWnd, SW_SHOWNOACTIVATE);
        _DiagRefresh(s_hDiagWnd);
    }
}

void CeLspFeatures_ShowDocumentSymbols(void)
{
    if (!CeLspFeatures_IsActive()) {
        _Log("LSP: server not ready", 2);
        return;
    }
    if (!CeLspClient_HasCap(CELSP_CAP_DOCSYMBOLS)) {
        _Log("LSP: document symbols not supported", 2);
        return;
    }
    CeLspClient_RequestPositional("textDocument/documentSymbol", CELSP_REQ_DOCSYMBOLS);
}

void CeLspFeatures_ShowWorkspaceSymbols(void)
{
    if (!CeLspFeatures_IsActive()) {
        _Log("LSP: server not ready", 2);
        return;
    }
    if (!CeLspClient_HasCap(CELSP_CAP_WSSYMBOLS)) {
        _Log("LSP: workspace symbols not supported", 2);
        return;
    }
    char root[128];
    DocPos wordStart = 0;
    _RootAtCaret(root, COUNTOF(root), &wordStart);
    CeLspClient_RequestWorkspaceSymbols(root);
}

void CeLspFeatures_OnDocumentSymbols(const CELSP_JSON* result)
{
    if (!result || (CeLspJson_Type(result) != CELSP_JSON_ARRAY)) {
        return;
    }
    s_symbolCount = 0;
    const size_t n = CeLspJson_Count(result);
    for (size_t i = 0u; i < n; ++i) {
        const CELSP_JSON* const sym = CeLspJson_At(result, i);
        _AddSymbol(sym);
        // flatten one level of DocumentSymbol children
        const CELSP_JSON* const kids = CeLspJson_Get(sym, "children");
        const size_t m = CeLspJson_Count(kids);
        for (size_t j = 0u; j < m; ++j) {
            _AddSymbol(CeLspJson_At(kids, j));
        }
    }
    if (s_symbolCount == 0) {
        _Log("LSP: no symbols in document", 2);
        return;
    }
    _ShowSymbolList();
}

void CeLspFeatures_OnWorkspaceSymbols(const CELSP_JSON* result)
{
    if (!result || (CeLspJson_Type(result) != CELSP_JSON_ARRAY)) {
        return;
    }
    s_symbolCount = 0;
    const size_t n = CeLspJson_Count(result);
    for (size_t i = 0u; i < n; ++i) {
        _AddSymbol(CeLspJson_At(result, i));
    }
    if (s_symbolCount == 0) {
        _Log("LSP: no symbols found", 2);
        return;
    }
    _ShowSymbolList();
}

// ----------------------------------------------------------------------------
// formatting
// ----------------------------------------------------------------------------

void CeLspFeatures_FormatDocument(void)
{
    if (CeLspFeatures_IsActive() && CeLspClient_HasCap(CELSP_CAP_FORMATTING)) {
        if (CeLspClient_RequestFormatting()) {
            return; // applied asynchronously by CeLspFeatures_OnFormatting
        }
    }
    CeFormat_Document(); // local fallback keeps the menu item always useful
}

void CeLspFeatures_OnFormatting(const CELSP_JSON* result)
{
    if (!result || (CeLspJson_Type(result) != CELSP_JSON_ARRAY)) {
        return;
    }
    typedef struct { DocPos start; DocPos end; char* text; } LSP_TEDIT;
    LSP_TEDIT edits[256];
    int count = 0;
    const size_t n = CeLspJson_Count(result);
    for (size_t i = 0u; (i < n) && (count < 256); ++i) {
        const CELSP_JSON* const e = CeLspJson_At(result, i);
        DocPos a = 0;
        DocPos b = 0;
        if (!_ParseRange(CeLspJson_Get(e, "range"), &a, &b)) {
            continue;
        }
        char* const text = _DupStr(CeLspJson_GetString(e, "newText"));
        if (!text) {
            continue;
        }
        edits[count].start = a;
        edits[count].end = b;
        edits[count].text = text;
        ++count;
    }
    // insertion sort by ascending start, then apply in reverse so that the
    // offsets of the not-yet-applied edits stay valid.
    for (int i = 1; i < count; ++i) {
        LSP_TEDIT const tmp = edits[i];
        int j = i - 1;
        while ((j >= 0) && (edits[j].start > tmp.start)) {
            edits[j + 1] = edits[j];
            --j;
        }
        edits[j + 1] = tmp;
    }
    if (count > 0) {
        SciCall_BeginUndoAction();
        for (int i = count - 1; i >= 0; --i) {
            SciCall_SetTargetRange(edits[i].start, edits[i].end);
            SciCall_ReplaceTarget(-1, edits[i].text);
            free(edits[i].text);
        }
        SciCall_EndUndoAction();
        CeLspClient_OnDocumentChanged();
        char msg[64];
        snprintf(msg, sizeof(msg), "LSP: formatting applied (%d edits)", count);
        _Log(msg, 3);
    }
}

// ----------------------------------------------------------------------------
// semantic tokens
// ----------------------------------------------------------------------------

static int _TokenStyle(const char* type)
{
    if (!type) {
        return -1;
    }
    // Names match the server's g_semTokenTypes legend.
    if (!_stricmp(type, "keyword"))   { return SCE_CE_WORD; }
    if (!_stricmp(type, "function"))  { return SCE_CE_WORD8; }
    if (!_stricmp(type, "variable") || !_stricmp(type, "parameter")) { return SCE_CE_IDENTIFIER; }
    if (!_stricmp(type, "string"))    { return SCE_CE_STRING; }
    if (!_stricmp(type, "number"))    { return SCE_CE_NUMBER; }
    if (!_stricmp(type, "comment"))   { return SCE_CE_COMMENT; }
    if (!_stricmp(type, "operator"))  { return SCE_CE_OPERATOR; }
    if (!_stricmp(type, "macro"))     { return SCE_CE_PREPROCESSOR; }
    if (!_stricmp(type, "label"))     { return SCE_CE_WORD7; }
    if (!_stricmp(type, "register") || !_stricmp(type, "instruction")) { return SCE_CE_WORD4; }
    if (!_stricmp(type, "type") || !_stricmp(type, "constant")) { return SCE_CE_WORD5; }
    if (!_stricmp(type, "assembly"))  { return SCE_CE_WORD4; }
    if (!_stricmp(type, "aa") || !_stricmp(type, "directive")) { return SCE_CE_WORD3; }
    if (!_stricmp(type, "section"))   { return SCE_CE_WORD6; }
    if (!_stricmp(type, "namespace") || !_stricmp(type, "module")) { return SCE_CE_MODULE; }
    if (!_stricmp(type, "api"))       { return SCE_CE_WORD2; }
    return -1;
}

void CeLspFeatures_OnSemanticTokens(const CELSP_JSON* result)
{
    if (!Settings2.CeLspSemanticTokens || !CeLspFeatures_IsActive() || !result) {
        return;
    }
    const CELSP_JSON* const data = CeLspJson_Get(result, "data");
    const size_t n = CeLspJson_Count(data);
    int line = 0;
    int character = 0;
    for (size_t i = 0u; (i + 4u) < n; i += 5u) {
        const int deltaLine = CeLspJson_Int(CeLspJson_At(data, i));
        const int deltaStart = CeLspJson_Int(CeLspJson_At(data, i + 1u));
        line += deltaLine;
        // LSP: startChar is absolute when the token starts a new line and only
        // relative to the previous token on the same line.  Adding it
        // unconditionally accumulated a drift after every line break, which
        // skewed the semantic styles after the first edit.
        character = (deltaLine == 0) ? (character + deltaStart) : deltaStart;
        const int len = CeLspJson_Int(CeLspJson_At(data, i + 2u));
        const int type = CeLspJson_Int(CeLspJson_At(data, i + 3u));
        if (len <= 0) {
            continue;
        }
        const int style = _TokenStyle(CeLspClient_TokenType(type));
        if (style < 0) {
            continue;
        }
        DocPos a = 0;
        DocPos b = 0;
        if (!CeLspClient_PosToOffset(line, character, &a)) {
            continue;
        }
        if (!CeLspClient_PosToOffset(line, character + len, &b) || (b <= a)) {
            continue;
        }
        // LexCe already colours everything it recognises: [ENABLE]/[DISABLE]
        // (WORD6), {$...} directives (PREPROCESSOR), CE API names (WORD2),
        // keywords, strings, ... .  Writing semantic styles over those bytes
        // split one token into two colours (purple "ENABLE" inside a red
        // "[ENABLE]") and repainted green CE API calls purple.  Semantic tokens
        // may only refine words the lexer left unclassified.
        int const existing = SciCall_GetStyleAt(a) & 0xff;
        if ((existing != SCE_CE_DEFAULT) && (existing != SCE_CE_IDENTIFIER)) {
            continue;
        }
        SciCall_StartStyling(a);
        SciCall_SetStyling(b - a, style);
    }
}

// ----------------------------------------------------------------------------
// inlay hints + code lens (one combined annotation per line)
// ----------------------------------------------------------------------------

static void _AppendText(char* buf, size_t cap, size_t* used, const char* s)
{
    if (!s) {
        return;
    }
    const size_t len = strlen(s);
    if ((*used + len + 2u) >= cap) {
        return;
    }
    if (*used > 0u) {
        buf[*used] = ' ';
        buf[*used + 1] = ' ';
        *used += 2u;
    }
    memcpy(buf + *used, s, len);
    *used += len;
    buf[*used] = '\0';
}

void CeLspFeatures_OnInlayHints(const CELSP_JSON* result)
{
    s_hintCount = 0;
    if (result && (CeLspJson_Type(result) == CELSP_JSON_ARRAY)) {
        const size_t n = CeLspJson_Count(result);
        for (size_t i = 0u; (i < n) && (s_hintCount < LSP_ANN_LIMIT); ++i) {
            const CELSP_JSON* const h = CeLspJson_At(result, i);
            const CELSP_JSON* const pos = CeLspJson_Get(h, "position");
            if (!pos) {
                continue;
            }
            char text[192] = { 0 };
            size_t used = 0u;
            const CELSP_JSON* const label = CeLspJson_Get(h, "label");
            if (CeLspJson_Type(label) == CELSP_JSON_STRING) {
                _AppendText(text, sizeof(text), &used, CeLspJson_String(label));
            }
            else if (CeLspJson_Type(label) == CELSP_JSON_ARRAY) {
                const size_t m = CeLspJson_Count(label);
                for (size_t j = 0u; j < m; ++j) {
                    _AppendText(text, sizeof(text), &used, _ItemText(CeLspJson_At(label, j)));
                }
            }
            if (!text[0]) {
                continue;
            }
            s_hints[s_hintCount].line = CeLspJson_GetInt(pos, "line");
            _Copy(s_hints[s_hintCount].text, sizeof(s_hints[0].text), text);
            ++s_hintCount;
        }
    }
    _RenderAnnotations();
}

void CeLspFeatures_OnCodeLens(const CELSP_JSON* result)
{
    s_lensCount = 0;
    if (result && (CeLspJson_Type(result) == CELSP_JSON_ARRAY)) {
        const size_t n = CeLspJson_Count(result);
        for (size_t i = 0u; (i < n) && (s_lensCount < LSP_ANN_LIMIT); ++i) {
            const CELSP_JSON* const cl = CeLspJson_At(result, i);
            const CELSP_JSON* const st = CeLspJson_Get(CeLspJson_Get(cl, "range"), "start");
            const char* const title = CeLspJson_GetString(CeLspJson_Get(cl, "command"), "title");
            if (!st || !title || !*title) {
                continue;
            }
            s_lenses[s_lensCount].line = CeLspJson_GetInt(st, "line");
            _Copy(s_lenses[s_lensCount].text, sizeof(s_lenses[0].text), title);
            ++s_lensCount;
        }
    }
    _RenderAnnotations();
}

static void _RenderAnnotations(void)
{
    _EnsureLspDecorInit();
    SciCall_AnnotationClearAll();
    const bool showHints = Settings2.CeLspInlayHints;
    const bool showLens = Settings2.CeLspCodeLens;
    if ((!showHints || (s_hintCount == 0)) && (!showLens || (s_lensCount == 0))) {
        SciCall_AnnotationSetVisible(ANNOTATION_HIDDEN);
        return;
    }
    SciCall_AnnotationSetVisible(ANNOTATION_BOXED);
    const DocLn lines = SciCall_GetLineCount();
    for (int i = 0; i < s_lensCount; ++i) {
        if (!showLens) {
            break;
        }
        const DocLn line = s_lenses[i].line;
        if ((line < 0) || (line >= lines)) {
            continue;
        }
        char buf[400];
        size_t used = 0u;
        _AppendText(buf, sizeof(buf), &used, s_lenses[i].text);
        if (showHints) {
            for (int j = 0; j < s_hintCount; ++j) {
                if (s_hints[j].line == line) {
                    _AppendText(buf, sizeof(buf), &used, s_hints[j].text);
                }
            }
        }
        SciCall_AnnotationSetText(line, buf);
        SciCall_AnnotationSetStyle(line, SCE_CE_COMMENT);
    }
    if (showHints) {
        for (int j = 0; j < s_hintCount; ++j) {
            const DocLn line = s_hints[j].line;
            if ((line < 0) || (line >= lines)) {
                continue;
            }
            if (SciCall_AnnotationGetLines(line) > 0) {
                continue; // already merged into the code-lens annotation
            }
            char buf[400];
            size_t used = 0u;
            _AppendText(buf, sizeof(buf), &used, s_hints[j].text);
            for (int k = j + 1; k < s_hintCount; ++k) {
                if (s_hints[k].line == line) {
                    _AppendText(buf, sizeof(buf), &used, s_hints[k].text);
                }
            }
            SciCall_AnnotationSetText(line, buf);
            SciCall_AnnotationSetStyle(line, SCE_CE_COMMENT);
        }
    }
}

// ----------------------------------------------------------------------------
// document links (Ctrl+click opens the target file)
// ----------------------------------------------------------------------------

void CeLspFeatures_OnDocumentLinks(const CELSP_JSON* result)
{
    s_linkCount = 0;
    if (!result || (CeLspJson_Type(result) != CELSP_JSON_ARRAY)) {
        return;
    }
    const size_t n = CeLspJson_Count(result);
    for (size_t i = 0u; (i < n) && (s_linkCount < LSP_LINK_LIMIT); ++i) {
        const CELSP_JSON* const lk = CeLspJson_At(result, i);
        const char* const target = CeLspJson_GetString(lk, "target");
        if (!target || !*target) {
            continue;
        }
        DocPos a = 0;
        DocPos b = 0;
        if (!_ParseRange(CeLspJson_Get(lk, "range"), &a, &b)) {
            continue;
        }
        s_links[s_linkCount].start = a;
        s_links[s_linkCount].end = b;
        _Copy(s_links[s_linkCount].target, sizeof(s_links[0].target), target);
        ++s_linkCount;
    }
}

static void _OpenLink(const char* target)
{
    wchar_t wpath[1024] = { 0 };
    if (!_strnicmp(target, "file://", 7)) {
        char utf8[2048];
        if (!CeLspPos_PathFromUri(target, utf8, sizeof(utf8))) {
            _Log("LSP: bad document link", 1);
            return;
        }
        _Utf8ToWide(utf8, wpath, COUNTOF(wpath));
    }
    else {
        _Utf8ToWide(target, wpath, COUNTOF(wpath));
        if (PathIsRelativeW(wpath)) {
            wchar_t dir[512] = { 0 };
            LPCWSTR const cur = Path_Get(Paths.CurrentFile);
            if (cur && *cur) {
                StringCchCopyW(dir, COUNTOF(dir), cur);
                wchar_t* const slash = wcsrchr(dir, L'\\');
                if (slash) {
                    *slash = L'\0';
                }
            }
            if (!dir[0]) {
                LPCWSTR const wd = Path_Get(Paths.WorkingDirectory);
                if (wd) {
                    StringCchCopyW(dir, COUNTOF(dir), wd);
                }
            }
            wchar_t full[1024];
            StringCchPrintfW(full, COUNTOF(full), L"%s\\%s", dir, wpath);
            StringCchCopyW(wpath, COUNTOF(wpath), full);
        }
    }
    if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) {
        _Log("LSP: link target not found", 2);
        return;
    }
    HPATHL const hPath = Path_Allocate(wpath);
    if (!hPath) {
        return;
    }
    FileLoadFlags fLoadFlags = FLF_None;
    fLoadFlags |= Settings.SkipUnicodeDetection ? FLF_SkipUnicodeDetect : 0;
    fLoadFlags |= Settings.SkipANSICodePageDetection ? FLF_SkipANSICPDetection : 0;
    FileLoad(hPath, fLoadFlags, 0, 0);
    Path_Release(hPath);
}

bool CeLspFeatures_OnClickNotify(DocPos position)
{
    // The vendored Scintilla has no click-notify API: a Ctrl+double-click on a
    // linked range is the gesture that opens the target file.
    if ((s_linkCount == 0) || ((GetKeyState(VK_CONTROL) & 0x8000) == 0)) {
        return false;
    }
    for (int i = 0; i < s_linkCount; ++i) {
        if ((position >= s_links[i].start) && (position <= s_links[i].end)) {
            _OpenLink(s_links[i].target);
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// document highlight
// ----------------------------------------------------------------------------

void CeLspFeatures_OnDocumentHighlight(const CELSP_JSON* result)
{
    _EnsureLspDecorInit();
    SciCall_SetIndicatorCurrent(INDIC_NP3_LSP_HILITE);
    SciCall_IndicatorClearRange(0, SciCall_GetLength());
    if (!result || !CeLspFeatures_IsActive()) {
        return;
    }
    const size_t n = CeLspJson_Count(result);
    for (size_t i = 0u; i < n; ++i) {
        DocPos a = 0;
        DocPos b = 0;
        if (!_ParseRange(CeLspJson_Get(CeLspJson_At(result, i), "range"), &a, &b)) {
            continue;
        }
        if (b <= a) {
            b = a + 1;
        }
        SciCall_SetIndicatorCurrent(INDIC_NP3_LSP_HILITE);
        SciCall_IndicatorFillRange(a, b - a);
    }
}

// ----------------------------------------------------------------------------
// decoration refresh after every synchronised change
// ----------------------------------------------------------------------------

void CeLspFeatures_RefreshDecorations(void)
{
    if (!Settings2.CeLspInlayHints) {
        s_hintCount = 0;
    }
    if (!Settings2.CeLspCodeLens) {
        s_lensCount = 0;
    }
    _RenderAnnotations();
    if (!CeLspFeatures_IsActive()) {
        return;
    }
    if (CeLspClient_HasCap(CELSP_CAP_SEMTOKENS) && Settings2.CeLspSemanticTokens) {
        CeLspClient_RequestPositional("textDocument/semanticTokens/full", CELSP_REQ_SEMTOKENS);
    }
    if (CeLspClient_HasCap(CELSP_CAP_INLAY) && Settings2.CeLspInlayHints) {
        CeLspClient_RequestPositional("textDocument/inlayHint", CELSP_REQ_INLAY);
    }
    if (CeLspClient_HasCap(CELSP_CAP_CODELENS) && Settings2.CeLspCodeLens) {
        CeLspClient_RequestPositional("textDocument/codeLens", CELSP_REQ_CODELENS);
    }
    if (CeLspClient_HasCap(CELSP_CAP_DOCLINKS)) {
        CeLspClient_RequestPositional("textDocument/documentLink", CELSP_REQ_DOCLINKS);
    }
}
