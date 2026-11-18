// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeHighlight.c                                                               *
*   Highlight user-defined variables and label()/label: names in CE scripts.  *
*                                                                             *
*   The bundled lexer only knows static keyword lists, so host-side            *
*   indicators are used instead.  The marks are owned by the UI thread and     *
*   are dropped as soon as the text changes: Scintilla shifts decorations      *
*   along with inserted/deleted text, so keeping them until the throttled      *
*   rebuild lands them on unrelated words ("highlight does not match the      *
*   keyword").  A rebuild only happens when something actually changed.       *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <stdlib.h>
#include <string.h>

#include "SciCall.h"
#include "CeHighlight.h"
#include "CeScript.h"
#include "CeSettings.h"

// ----------------------------------------------------------------------------

#define CE_HL_THROTTLE_MS 250

static bool        s_initialized = false;
static bool        s_dirty = true;
static bool        s_marksStale = true;  // on-screen ranges no longer match the text
static bool        s_wasActive = false;
static bool        s_wasEnabled = false;
static ULONGLONG   s_lastScan = 0;
static DocPos      s_lastLen = -1;

static void _ConfigureIndicator(void)
{
    // Kept on every rebuild: a fresh Scintilla document or a future
    // StyleClearAll() may otherwise leave the indicator unstyled.
    SciCall_IndicSetStyle(INDIC_NP3_CE_SYMBOL, INDIC_TEXTFORE);
    SciCall_IndicSetFore(INDIC_NP3_CE_SYMBOL, RGB(0x9C, 0x36, 0xAC));
}

// These two MUST agree with IsIdentStart/IsIdentChar in LexCe.cxx.  The host
// used to accept '.' inside a word (the lexer treats it as an operator) and
// rejected '@' (the lexer accepts it), which made the marks cover a different
// token than the one the keyword classifier saw.
static bool _IsIdentStart(unsigned char ch)
{
    return ((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z')) || (ch == '_');
}

static bool _IsIdentChar(unsigned char ch)
{
    return _IsIdentStart(ch) || ((ch >= '0') && (ch <= '9')) || (ch == '$') || (ch == '@');
}

static unsigned char _Lower(unsigned char ch)
{
    return ((ch >= 'A') && (ch <= 'Z')) ? (unsigned char)(ch + ('a' - 'A')) : ch;
}

// Three-way, case-insensitive comparison of a NUL-terminated name against a
// length-delimited document span.  Enables a binary search over the defs
// without copying each token into a temporary buffer.
static int _CompareNameToSpan(const char* name, const char* text, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        unsigned char const a = (unsigned char)name[i];
        unsigned char const b = (unsigned char)text[i];
        if (a == '\0') {
            return -1;
        }
        unsigned char const la = _Lower(a);
        unsigned char const lb = _Lower(b);
        if (la != lb) {
            return (la < lb) ? -1 : 1;
        }
    }
    return (name[len] == '\0') ? 0 : 1;
}

static int __cdecl _CompareDefByName(const void* pA, const void* pB)
{
    const CE_DEF* const a = (const CE_DEF*)pA;
    const CE_DEF* const b = (const CE_DEF*)pB;
    return _stricmp(a->name, b->name);
}

static bool _IsDefName(const CE_DEF* defs, int count, const char* text, size_t len)
{
    int lo = 0;
    int hi = count - 1;
    while (lo <= hi) {
        int const mid = lo + ((hi - lo) / 2);
        int const cmp = _CompareNameToSpan(defs[mid].name, text, len);
        if (cmp == 0) {
            return true;
        }
        if (cmp < 0) {
            lo = mid + 1;
        }
        else {
            hi = mid - 1;
        }
    }
    return false;
}

// Skip a multi-line comment starting at |p|.  |open| is the opener length
// (4 for "--[[", 5 for "--[=[", 2 for "/*").  Returns the offset just past
// the terminator, or |len| when the comment is unterminated.
static DocPos _BlockCommentEnd(const char* text, DocPos len, DocPos p, DocPos open, bool lua)
{
    p += open;
    if (lua) {
        while ((p + 1) < len) {
            if (text[p] == ']') {
                if (text[p + 1] == ']') {
                    return p + 2;
                }
                if ((text[p + 1] == '=') && ((p + 2) < len) && (text[p + 2] == ']')) {
                    return p + 3;
                }
            }
            ++p;
        }
    }
    else {
        while ((p + 1) < len) {
            if ((text[p] == '*') && (text[p + 1] == '/')) {
                return p + 2;
            }
            ++p;
        }
    }
    return len;
}

static void _ClearMarks(DocPos docEnd)
{
    SciCall_SetIndicatorCurrent(INDIC_NP3_CE_SYMBOL);
    SciCall_IndicatorClearRange(0, docEnd);
}

// ----------------------------------------------------------------------------

void CeHighlight_Init(void)
{
    _ConfigureIndicator();
    s_initialized = true;
    s_dirty = true;
    s_marksStale = true;
    s_wasActive = false;
    s_wasEnabled = false;
    s_lastScan = 0;
    s_lastLen = -1;
}

void CeHighlight_Invalidate(void)
{
    s_dirty = true;
    s_marksStale = true;
}

void CeHighlight_Refresh(bool force)
{
    if (!s_initialized) {
        return;
    }
    bool const active = CeScript_IsActive();
    bool const enabled = CeSettings_HighlightUserSymbols();
    if ((active != s_wasActive) || (enabled != s_wasEnabled)) {
        s_wasActive = active;
        s_wasEnabled = enabled;
        s_dirty = true;
        s_marksStale = true;
    }

    DocPos const docEnd = Sci_GetDocEndPosition();
    if (!active || !enabled) {
        if (s_lastLen >= 0) {
            _ClearMarks(docEnd);
            s_lastLen = -1;
        }
        s_dirty = false;
        s_marksStale = false;
        return;
    }
    if (docEnd != s_lastLen) {
        s_dirty = true;
    }

    // During the throttle window the marks must be gone, not merely stale:
    // Scintilla moves indicator ranges together with edited text.
    if (s_marksStale) {
        _ClearMarks(docEnd);
        s_marksStale = false;
        s_lastLen = -1;
    }
    if (!s_dirty) {
        return; // nothing changed; no periodic full-document rebuild
    }
    ULONGLONG const now = GetTickCount64();
    if (!force && (s_lastScan != 0) && ((now - s_lastScan) < CE_HL_THROTTLE_MS)) {
        return;
    }
    s_dirty = false;
    s_lastScan = now;
    s_lastLen = docEnd;

    _ConfigureIndicator();
    // Always clear before filling: _ClearMarks() also restores the indicator
    // current, which CeDiagnostics_End() leaves pointing at the error/warning
    // squiggle.  Filling without it paints the wrong indicator entirely.
    _ClearMarks(docEnd);

    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc)) {
        return;
    }
    CE_DEF* const defs = AllocMem(sizeof(CE_DEF) * CE_MAX_DEFS, HEAP_ZERO_MEMORY);
    if (!defs) {
        CeScript_FreeDoc(&doc);
        return;
    }
    int const defCount = CeScript_CollectDefs(&doc, defs, CE_MAX_DEFS);
    if (defCount > 1) {
        qsort(defs, (size_t)defCount, sizeof(CE_DEF), _CompareDefByName);
    }

    DocPos p = 0;
    while (p < doc.len) {
        unsigned char const ch = (unsigned char)doc.text[p];

        // Multi-line comments are inert, exactly like in CeScript_CollectDefs().
        if ((ch == '-') && ((p + 3) < doc.len) && (doc.text[p + 1] == '-') &&
            (doc.text[p + 2] == '[') &&
            ((doc.text[p + 3] == '[') || (doc.text[p + 3] == '='))) {
            p = _BlockCommentEnd(doc.text, doc.len, p, (doc.text[p + 3] == '=') ? 5 : 4, true);
            continue;
        }
        if ((ch == '/') && ((p + 1) < doc.len) && (doc.text[p + 1] == '*')) {
            p = _BlockCommentEnd(doc.text, doc.len, p, 2, false);
            continue;
        }
        // Line comments and quoted strings; only real identifiers count.
        if ((ch == ';') ||
            ((ch == '/') && (p + 1 < doc.len) && (doc.text[p + 1] == '/')) ||
            ((ch == '-') && (p + 1 < doc.len) && (doc.text[p + 1] == '-'))) {
            while ((p < doc.len) && (doc.text[p] != '\n') && (doc.text[p] != '\r')) {
                ++p;
            }
            continue;
        }
        if ((ch == '"') || (ch == '\'')) {
            char const quote = (char)ch;
            ++p;
            while ((p < doc.len) && (doc.text[p] != quote)) {
                if ((doc.text[p] == '\\') && (p + 1 < doc.len)) {
                    ++p;
                }
                ++p;
            }
            if (p < doc.len) {
                ++p;
            }
            continue;
        }
        if (!_IsIdentStart(ch)) {
            ++p;
            continue;
        }

        DocPos const start = p;
        while ((p < doc.len) && _IsIdentChar((unsigned char)doc.text[p])) {
            ++p;
        }
        size_t const len = (size_t)(p - start);
        if ((len == 0) || (len >= COUNTOF(defs[0].name))) {
            continue;
        }
        if (_IsDefName(defs, defCount, doc.text + start, len)) {
            SciCall_IndicatorFillRange(start, (DocPos)len);
        }
    }

    FreeMem(defs);
    CeScript_FreeDoc(&doc);
}
