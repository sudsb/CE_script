// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeScript.c                                                                  *
*   Shared document model for the Cheat Engine script tooling.                *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <assert.h>
#include <ctype.h>
#include <string.h>

#include "Styles.h"
#include "SciCall.h"
#include "CeScript.h"

// ----------------------------------------------------------------------------

static char _LowerCh(char ch) {
    return (char)((ch >= 'A' && ch <= 'Z') ? (ch + ('a' - 'A')) : ch);
}

static bool _IsIdentStart(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static bool _IsIdentChar(char ch) {
    return _IsIdentStart(ch) || (ch >= '0' && ch <= '9');
}

static bool _IsBlank(char ch) {
    return ch == ' ' || ch == '\t';
}

// ----------------------------------------------------------------------------

bool CeScript_IsActive(void) {
    return (Style_GetCurrentLexerPtr() == &lexCE);
}

bool CeScript_InitDoc(CE_DOC* doc) {
    if (!doc) {
        return false;
    }
    doc->text = NULL;
    doc->len = 0;
    doc->lines = 0;
    doc->starts = NULL;

    DocPos const len = Sci_GetDocEndPosition();
    if (len <= 0) {
        return false;
    }
    const char* const text = SciCall_GetRangePointer(0, len);
    if (!text) {
        return false;
    }

    DocLn lines = 1;
    for (DocPos i = 0; i < len; ++i) {
        if (text[i] == '\n') {
            ++lines;
        }
    }
    DocPos* const starts = AllocMem(sizeof(DocPos) * (size_t)(lines + 1), HEAP_ZERO_MEMORY);
    if (!starts) {
        return false;
    }
    DocLn line = 0;
    starts[0] = 0;
    for (DocPos i = 0; i < len; ++i) {
        if (text[i] == '\n') {
            ++line;
            starts[line] = i + 1;
        }
    }
    starts[lines] = len;

    // Borrowed pointer: the caller must not mutate the document until
    // CeScript_FreeDoc(); the debug check there catches violations.
    doc->text = text;
    doc->len = len;
    doc->lines = lines;
    doc->starts = starts;
    return true;
}

void CeScript_FreeDoc(CE_DOC* doc) {
    // A changed document length means a SciCall mutation happened while the
    // borrowed text pointer/starts table were live — a use-after-move bug.
    assert(!doc || !doc->text || (Sci_GetDocEndPosition() == doc->len));
    if (doc && doc->starts) {
        FreeMem(doc->starts);
        doc->starts = NULL;
    }
}

DocLn CeScript_LineFromPos(const CE_DOC* doc, DocPos pos) {
    if (!doc || !doc->starts) {
        return 0;
    }
    DocLn low = 0;
    DocLn high = doc->lines;
    while (low < high) {
        DocLn const mid = (low + high + 1) / 2;
        if (doc->starts[mid] <= pos) {
            low = mid;
        }
        else {
            high = mid - 1;
        }
    }
    return low;
}

const char* CeScript_LinePtr(const CE_DOC* doc, DocLn line, DocPos* len) {
    if (!doc || !doc->starts || line < 0 || line >= doc->lines) {
        if (len) {
            *len = 0;
        }
        return "";
    }
    DocPos const start = doc->starts[line];
    DocPos const end = doc->starts[line + 1];
    DocPos n = end - start;
    const char* const p = doc->text + start;
    while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == '\n')) {
        --n;
    }
    if (len) {
        *len = n;
    }
    return p;
}

const char* CeScript_Trimmed(const CE_DOC* doc, DocLn line, DocPos* len) {
    DocPos n = 0;
    const char* p = CeScript_LinePtr(doc, line, &n);
    while (n > 0 && _IsBlank(*p)) {
        ++p;
        --n;
    }
    if (len) {
        *len = n;
    }
    return p;
}

const char* CeScript_SkipBlanks(const char* p, const char* end) {
    while (p < end && _IsBlank(*p)) {
        ++p;
    }
    return p;
}

bool CeScript_StartsWithCI(const char* text, DocPos len, const char* prefix) {
    DocPos i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= len || _LowerCh(text[i]) != _LowerCh(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool CeScript_EqualsCI(const char* text, DocPos len, const char* word) {
    DocPos const wordLen = (DocPos)strlen(word);
    return (len == wordLen) && CeScript_StartsWithCI(text, len, word);
}

const char* CeScript_ReadIdent(const char* p, const char* end, char* out, size_t outCch) {
    if (!out || outCch == 0) {
        return p;
    }
    out[0] = '\0';
    if (p >= end || !_IsIdentStart(*p)) {
        return p;
    }
    const char* start = p;
    while (p < end && _IsIdentChar(*p)) {
        ++p;
    }
    size_t const n = (size_t)(p - start);
    size_t const copy = (n < (outCch - 1)) ? n : (outCch - 1);
    memcpy(out, start, copy);
    out[copy] = '\0';
    return p;
}

// ----------------------------------------------------------------------------

// [ENABLE] / [DISABLE] at the beginning of |p|.
static bool _SectionAt(const char* p, const char* end, bool* enable_out) {
    if (p >= end || *p != '[') {
        return false;
    }
    const char* q = CeScript_SkipBlanks(p + 1, end);
    DocPos const rest = (DocPos)(end - q);
    bool enable = false;
    if (CeScript_StartsWithCI(q, rest, "enable")) {
        q += 6;
        enable = true;
    }
    else if (CeScript_StartsWithCI(q, rest, "disable")) {
        q += 7;
        enable = false;
    }
    else {
        return false;
    }
    q = CeScript_SkipBlanks(q, end);
    if (q >= end || *q != ']') {
        return false;
    }
    if (enable_out) {
        *enable_out = enable;
    }
    return true;
}

// {$lua} / {$asm} / {$c} / {$ccode}: returns the new mode when matched.
static bool _DirectiveAt(const char* p, const char* end, CE_LANG* mode_out) {
    if ((end - p) < 5 || p[0] != '{' || p[1] != '$') {
        return false;
    }
    const char* q = p + 2;
    char name[16] = { '\0' };
    const char* const ident = CeScript_ReadIdent(q, end, name, COUNTOF(name));
    if (ident == q) {
        return false;
    }
    q = CeScript_SkipBlanks(ident, end);
    if (q >= end || *q != '}') {
        return false;
    }
    char lower[16] = { '\0' };
    for (int i = 0; name[i] != '\0' && i < (int)COUNTOF(lower) - 1; ++i) {
        lower[i] = _LowerCh(name[i]);
    }
    CE_LANG mode = CE_LANG_AA;
    if (strcmp(lower, "lua") == 0) {
        mode = CE_LANG_LUA;
    }
    else {
        static const char* const cNames[] = {
            "c", "ccode", "code", "stdc", "stdcall", "fastcode", NULL
        };
        for (int i = 0; cNames[i]; ++i) {
            if (strcmp(lower, cNames[i]) == 0) {
                mode = CE_LANG_C;
                break;
            }
        }
    }
    if (mode_out) {
        *mode_out = mode;
    }
    return true;
}

// Auto Assembler accepts "{ ... }" as a comment block.  CE emits one for the
// table header at the top of a script and for the disassembled original code
// after [DISABLE].  Directives such as "{$lua}" are code, and in C blocks a
// leading "{" is an ordinary brace, so those never start a comment.
static bool _BraceCommentAt(const char* p, const char* end, int mode) {
    if (mode == (int)CE_LANG_C) {
        return false;
    }
    const char* const b = CeScript_SkipBlanks(p, end);
    if ((b >= end) || (*b != '{')) {
        return false;
    }
    CE_LANG directiveMode = CE_LANG_AA;
    return !_DirectiveAt(b, end, &directiveMode);
}

// Track multi-line comments while scanning a line.  |block| is 0 (none),
// 1 (Lua --[[ ... ]]), 2 (C /* ... */) or 3 (AA "{ ... }").  Returns nothing;
// the caller uses |block| for the next line.
void CeScript_UpdateBlockComment(const char* p, const char* end, int mode, int* block) {
    if ((*block == 0) && _BraceCommentAt(p, end, mode)) {
        *block = 3;
        p = CeScript_SkipBlanks(p, end) + 1;
    }
    while (p < end) {
        if (*block == 3) {
            const char* hit = (const char*)memchr(p, '}', (size_t)(end - p));
            if (!hit) {
                return;
            }
            *block = 0;
            p = hit + 1;
            continue;
        }
        if (*block == 1) {
            const char* hit = (const char*)memchr(p, ']', (size_t)(end - p));
            if (!hit) {
                return;
            }
            p = hit + 1;
            if (p < end && *p == ']') {
                *block = 0;
                ++p;
            }
            continue;
        }
        if (*block == 2) {
            const char* hit = (const char*)memchr(p, '*', (size_t)(end - p));
            if (!hit) {
                return;
            }
            p = hit + 1;
            if (p < end && *p == '/') {
                *block = 0;
                ++p;
            }
            continue;
        }
        if ((end - p) >= 2) {
            if (p[0] == '-' && p[1] == '-' && (end - p) >= 4 && p[2] == '[' && p[3] == '[') {
                *block = 1;
                p += 4;
                continue;
            }
            if ((mode == CE_LANG_C) && p[0] == '/' && p[1] == '*') {
                *block = 2;
                p += 2;
                continue;
            }
            if (p[0] == '-' && p[1] == '-') {
                return; // line comment
            }
            if (p[0] == '/' && p[1] == '/') {
                return;
            }
        }
        ++p;
    }
}

void CeScript_LineModes(const CE_DOC* doc, unsigned char* modes) {
    if (!doc || !modes) {
        return;
    }
    // The lexer receives the file-extension default through a property; keep
    // the host analysis in sync so .lua stays Lua and .cea/.aa start as AA.
    CE_LANG mode = (SciCall_GetPropertyInt("lexer.ce.initial", 0) != 0) ? CE_LANG_AA : CE_LANG_LUA;
    int block = 0;
    for (DocLn line = 0; line < doc->lines; ++line) {
        DocPos n = 0;
        const char* const start = CeScript_LinePtr(doc, line, &n);
        const char* const end = start + n;

        if (block != 0) {
            // Inside a multi-line comment nothing changes the language mode.
            int const startedIn = block;
            CeScript_UpdateBlockComment(start, end, (block == 2) ? CE_LANG_C : CE_LANG_LUA, &block);
            modes[line] = (startedIn == 3)
                              ? (unsigned char)CE_LANG_COMMENT
                              : (unsigned char)((block == 2) ? CE_LANG_C : mode);
            continue;
        }

        const char* p = start;
        bool first = true;
        while (p < end) {
            if (*p == '-' && (p + 1) < end && p[1] == '-') {
                break; // Lua/AA line comment; markers inside are inert
            }
            if (*p == ';' && mode == CE_LANG_AA) {
                break;
            }
            if (*p == '/' && (p + 1) < end && p[1] == '/') {
                break;
            }
            if (first && _IsBlank(*p)) {
                ++p;
                continue;
            }
            if (first && (mode != CE_LANG_C)) {
                bool enable = false;
                if (_SectionAt(p, end, &enable)) {
                    mode = CE_LANG_AA;
                    p += 1;
                }
            }
            CE_LANG newMode = mode;
            if (*p == '{' && _DirectiveAt(p, end, &newMode)) {
                mode = newMode;
                ++p;
            }
            first = false;
            ++p;
        }
        bool const braceComment = _BraceCommentAt(start, end, mode);
        CeScript_UpdateBlockComment(start, end, mode, &block);
        modes[line] = (braceComment || (block == 3))
                          ? (unsigned char)CE_LANG_COMMENT : (unsigned char)mode;
    }
}

// Language mode in effect on the caret line.  Returns CE_LANG_LUA when the CE
// scheme is not active or the document is empty, so callers fall back to the
// Lua comment marker.
CE_LANG CeScript_LangAtCaret(void)
{
    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc) || (doc.lines <= 0)) {
        CeScript_FreeDoc(&doc);
        return CE_LANG_LUA;
    }
    unsigned char* const modes = AllocMem((size_t)doc.lines, HEAP_ZERO_MEMORY);
    if (!modes) {
        CeScript_FreeDoc(&doc);
        return CE_LANG_LUA;
    }
    CeScript_LineModes(&doc, modes);
    DocLn const line = SciCall_LineFromPosition(SciCall_GetCurrentPos());
    CE_LANG const mode = ((line >= 0) && (line < doc.lines))
                             ? (CE_LANG)modes[line] : CE_LANG_LUA;
    FreeMem(modes);
    CeScript_FreeDoc(&doc);
    return mode;
}

// ----------------------------------------------------------------------------

int CeScript_CollectSections(const CE_DOC* doc, CE_SECTION* out, int max) {
    if (!doc || !out || max <= 0) {
        return 0;
    }
    int count = 0;
    int block = 0;
    CE_LANG mode = CE_LANG_LUA;
    for (DocLn line = 0; line < doc->lines && count < max; ++line) {
        DocPos n = 0;
        const char* const start = CeScript_LinePtr(doc, line, &n);
        const char* const end = start + n;

        if (block != 0) {
            // [ENABLE] inside a multi-line comment is not a section marker.
            CeScript_UpdateBlockComment(start, end, (block == 2) ? CE_LANG_C : CE_LANG_LUA, &block);
            continue;
        }

        DocPos tn = 0;
        const char* const trimmed = CeScript_Trimmed(doc, line, &tn);
        bool isLineComment = (tn >= 2) &&
            ((trimmed[0] == '-' && trimmed[1] == '-') || (trimmed[0] == '/' && trimmed[1] == '/') || (trimmed[0] == ';'));

        bool enable = false;
        if (!isLineComment && _SectionAt(trimmed, trimmed + tn, &enable)) {
            DocPos const markerStart = (DocPos)(trimmed - doc->text);
            if (count > 0) {
                out[count - 1].end = markerStart;
            }
            CE_SECTION* const sec = &out[count++];
            sec->enable = enable;
            sec->line = line;
            sec->start = markerStart;
            sec->bodyStart = (line + 1 < doc->lines) ? doc->starts[line + 1] : doc->len;
            sec->end = doc->len;
            mode = CE_LANG_AA;
        }

        CeScript_UpdateBlockComment(start, end, mode, &block);
    }
    return count;
}

// ----------------------------------------------------------------------------

bool CeScript_IsDeclarationLine(const char* p, size_t len, bool enable)
{
    static const char* const s_enableWords[] = {
        "aobscan", "aobscanmodule", "aobscanregion", "aobscanpattern",
        "alloc", "globalalloc", "sharedalloc", "label", "registersymbol",
        "define", "readmem", "loadbinary", "createthread", "fullaccess", "assert",
    };
    static const char* const s_disableWords[] = {
        "dealloc", "unregistersymbol",
    };
    while ((len > 0) && ((*p == ' ') || (*p == '\t'))) {
        ++p;
        --len;
    }
    if ((len == 0) || (*p == '[') || (*p == '{') || (*p == ';') ||
        ((len >= 2) && (((*p == '/') && (p[1] == '/')) || ((*p == '-') && (p[1] == '-'))))) {
        return false;
    }
    size_t n = 0;
    while ((n < len) && (isalnum((unsigned char)p[n]) || (p[n] == '_'))) {
        ++n;
    }
    if (n == 0) {
        return false;
    }
    const char* const* const words = enable ? s_enableWords : s_disableWords;
    int const count = enable ? (int)COUNTOF(s_enableWords) : (int)COUNTOF(s_disableWords);
    for (int i = 0; i < count; ++i) {
        if ((strlen(words[i]) == n) && (_strnicmp(p, words[i], n) == 0)) {
            return true;
        }
    }
    return false;
}

DocPos CeScript_DeclarationAnchor(const CE_DOC* doc, const CE_SECTION* sec, bool enable)
{
    if (!doc || !sec || !doc->text || (sec->bodyStart < 0) || (sec->end > doc->len)) {
        return -1;
    }
    DocPos p = sec->bodyStart;
    DocPos anchor = -1;
    while (p < sec->end) {
        DocPos q = p;
        while ((q < sec->end) && (doc->text[q] != '\n') && (doc->text[q] != '\r')) {
            ++q;
        }
        if (CeScript_IsDeclarationLine(doc->text + p, (size_t)(q - p), enable)) {
            anchor = q;
        }
        p = q;
        while ((p < sec->end) && ((doc->text[p] == '\n') || (doc->text[p] == '\r'))) {
            ++p;
        }
    }
    return anchor;
}

// ----------------------------------------------------------------------------

static const char* const _CallKinds[] = {
    "alloc", "globalalloc", "sharedalloc", "aobscanmodule", "aobscanregion",
    "aobscan", "define", "label", "registersymbol", "unregistersymbol", "dealloc"
};

static void _AddDef(CE_DEF* out, int max, int* count, const char* name, const char* kind, DocLn line) {
    if (*count >= max || !name || name[0] == '\0') {
        return;
    }
    // A symbol can be both declared and defined: "label(foo)" (kind "label")
    // and "foo:" (kind "label:") must coexist.  Deduplicating by name alone
    // used to drop the definition whenever the declaration came first, which
    // made the linter report an existing [ENABLE] label as undefined.
    // Label definitions are kept even when the name repeats: the linter
    // reports a second "foo:" definition instead of silently dropping it.
    bool const keepDuplicates = (_stricmp(kind, "label:") == 0);
    if (!keepDuplicates) {
        for (int i = 0; i < *count; ++i) {
            if ((_stricmp(out[i].name, name) == 0) && (_stricmp(out[i].kind, kind) == 0)) {
                return;
            }
        }
    }
    CE_DEF* const def = &out[(*count)++];
    StringCchCopyA(def->name, COUNTOF(def->name), name);
    StringCchCopyA(def->kind, COUNTOF(def->kind), kind);
    def->line = line + 1;
}

int CeScript_CollectDefs(const CE_DOC* doc, CE_DEF* out, int max) {
    if (!doc || !out || max <= 0) {
        return 0;
    }
    int count = 0;
    int block = 0;
    char name[64] = { '\0' };

    // Brace comments ("{ ... }") are AA comments; their contents must not be
    // mistaken for symbols.  LineModes already classifies them.
    unsigned char* const modes = AllocMem((size_t)doc->lines, HEAP_ZERO_MEMORY);
    if (modes) {
        CeScript_LineModes(doc, modes);
    }

    for (DocLn line = 0; line < doc->lines; ++line) {
        DocPos n = 0;
        const char* const start = CeScript_LinePtr(doc, line, &n);
        const char* const end = start + n;
        if (modes && (modes[line] == (unsigned char)CE_LANG_COMMENT)) {
            continue;
        }
        // Multi-line comments are inert: a "label(x)" inside --[[ ]] or /* */
        // is documentation, not a symbol definition.
        int const wasInBlock = block;
        CeScript_UpdateBlockComment(start, end, (int)CE_LANG_C, &block);
        if (wasInBlock || block) {
            continue;
        }
        const char* p = CeScript_SkipBlanks(start, end);
        if (p >= end) {
            continue;
        }
        // Comments are inert.
        if ((end - p) >= 2 && ((p[0] == '-' && p[1] == '-') || (p[0] == '/' && p[1] == '/'))) {
            continue;
        }
        if (p[0] == ';' || p[0] == '#') {
            continue;
        }

        bool matched = false;
        if (CeScript_StartsWithCI(p, (DocPos)(end - p), "local")) {
            const char* q = CeScript_SkipBlanks(p + 5, end);
            if (CeScript_StartsWithCI(q, (DocPos)(end - q), "function")) {
                q = CeScript_SkipBlanks(q + 8, end);
            }
            const char* const after = CeScript_ReadIdent(q, end, name, COUNTOF(name));
            if (after != q) {
                _AddDef(out, max, &count, name, "local", line);
                matched = true;
            }
        }
        else if (CeScript_StartsWithCI(p, (DocPos)(end - p), "function")) {
            const char* q = CeScript_SkipBlanks(p + 8, end);
            const char* const after = CeScript_ReadIdent(q, end, name, COUNTOF(name));
            if (after != q) {
                _AddDef(out, max, &count, name, "function", line);
                matched = true;
            }
        }

        if (!matched) {
            for (int i = 0; i < (int)COUNTOF(_CallKinds); ++i) {
                if (!CeScript_StartsWithCI(p, (DocPos)(end - p), _CallKinds[i])) {
                    continue;
                }
                DocPos const klen = (DocPos)strlen(_CallKinds[i]);
                if (((DocPos)(end - p) > klen) && _IsIdentChar(p[klen])) {
                    continue; // "allocation" is not "alloc"
                }
                const char* q = CeScript_SkipBlanks(p + klen, end);
                if (q >= end || *q != '(') {
                    continue;
                }
                q = CeScript_SkipBlanks(q + 1, end);
                const char* const after = CeScript_ReadIdent(q, end, name, COUNTOF(name));
                if (after != q) {
                    _AddDef(out, max, &count, name, _CallKinds[i], line);
                    matched = true;
                }
                break;
            }
        }

        if (!matched) {
            // AA label definition: "name:" (but not "::name::").
            const char* const after = CeScript_ReadIdent(p, end, name, COUNTOF(name));
            if (after != p) {
                const char* q = CeScript_SkipBlanks(after, end);
                if (q < end && *q == ':' && ((q + 1) >= end || q[1] != ':')) {
                    _AddDef(out, max, &count, name, "label:", line);
                    matched = true;
                }
                else if (q < end && *q == '=' && ((q + 1) >= end || q[1] != '=')) {
                    _AddDef(out, max, &count, name, "assign", line);
                }
            }
        }
    }
    FreeMem(modes);
    return count;
}

const char* CeScript_EolStr(void) {
    switch (SciCall_GetEOLMode()) {
    case SC_EOL_CR:
        return "\r";
    case SC_EOL_LF:
        return "\n";
    default:
        return "\r\n";
    }
}

void CeScript_ReplaceAll(const char* text) {
    if (!text) {
        return;
    }
    DocPos const caret = SciCall_GetCurrentPos();
    SciCall_BeginUndoAction();
    SciCall_SetSel(0, Sci_GetDocEndPosition());
    SciCall_ReplaceSel(text);
    SciCall_EndUndoAction();

    DocPos const newLen = Sci_GetDocEndPosition();
    DocPos const target = (caret < newLen) ? caret : newLen;
    SciCall_SetEmptySelection(target);
    SciCall_ScrollCaret();
}

bool CeBuf_Init(CE_BUF* buf, size_t initialCap) {
    if (!buf) {
        return false;
    }
    size_t const cap = (initialCap < 256) ? 256 : initialCap;
    buf->data = AllocMem(cap, HEAP_ZERO_MEMORY);
    buf->len = 0;
    buf->cap = buf->data ? cap : 0;
    return (buf->data != NULL);
}

bool CeBuf_Cat(CE_BUF* buf, const char* text, size_t len) {
    if (!buf || !buf->data || !text) {
        return false;
    }
    if (buf->len + len + 1 > buf->cap) {
        size_t cap = buf->cap;
        while (cap < buf->len + len + 1) {
            cap *= 2;
        }
        char* const grown = AllocMem(cap, HEAP_ZERO_MEMORY);
        if (!grown) {
            return false;
        }
        memcpy(grown, buf->data, buf->len);
        FreeMem(buf->data);
        buf->data = grown;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return true;
}

bool CeBuf_CatZ(CE_BUF* buf, const char* text) {
    return text ? CeBuf_Cat(buf, text, strlen(text)) : false;
}

void CeBuf_Free(CE_BUF* buf) {
    if (buf && buf->data) {
        FreeMem(buf->data);
        buf->data = NULL;
        buf->len = 0;
        buf->cap = 0;
    }
}

int CeScript_FindDef(const CE_DEF* defs, int count, const char* name) {
    if (!defs || !name) {
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        if (_stricmp(defs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

// ----------------------------------------------------------------------------
// Strict section markers
// ----------------------------------------------------------------------------

bool CeScript_SectionMarker(const char* p, size_t len, char* name, size_t cchName) {
    if (name && (cchName > 0)) {
        name[0] = '\0';
    }
    if (!p || (len < 3)) {
        return false;
    }
    const char* const end = p + len;
    const char* q = CeScript_SkipBlanks(p, end);
    if ((q >= end) || (*q != '[')) {
        return false;
    }
    q = CeScript_SkipBlanks(q + 1, end);
    // "[1] = 'x'" is a Lua table constructor key, not a section name.
    if ((q >= end) || !_IsIdentStart(*q)) {
        return false;
    }
    const char* const ident = q;
    while ((q < end) && _IsIdentChar(*q)) {
        ++q;
    }
    size_t const nameLen = (size_t)(q - ident);
    if (nameLen >= 64) {
        return false;
    }
    q = CeScript_SkipBlanks(q, end);
    if ((q >= end) || (*q != ']')) {
        return false;
    }
    ++q;
    while ((q < end) && ((*q == ' ') || (*q == '\t') || (*q == '\r') || (*q == '\n'))) {
        ++q; // tolerate a CRLF when callers pass a raw line
    }
    if (q < end) {
        // Only a trailing comment may follow; "[FOO] = 1" is a table entry.
        bool const comment =
            ((q[0] == '-') && ((q + 1) < end) && (q[1] == '-')) ||
            ((q[0] == '/') && ((q + 1) < end) && (q[1] == '/')) ||
            (q[0] == ';');
        if (!comment) {
            return false;
        }
    }
    if (name && (cchName > 0)) {
        size_t const n = (nameLen < (cchName - 1)) ? nameLen : (cchName - 1);
        memcpy(name, ident, n);
        name[n] = '\0';
    }
    return true;
}

// ----------------------------------------------------------------------------
// Lua lexical scope model
// ----------------------------------------------------------------------------

typedef enum _ce_tok_kind {
    CE_TOK_IDENT = 0,
    CE_TOK_NUMBER,
    CE_TOK_STRING,
    CE_TOK_PUNCT,
} CE_TOK_KIND;

typedef struct _ce_tok {
    int         kind;
    const char* text;
    size_t      len;
    DocPos      pos;
} CE_TOK;

static bool _LuaTokIs(const CE_TOK* t, const char* s) {
    size_t const n = strlen(s);
    return (t->len == n) && (memcmp(t->text, s, n) == 0);
}

static bool _LuaWordIs(const CE_TOK* t, const char* word) {
    return (t->kind == CE_TOK_IDENT) && _LuaTokIs(t, word);
}

// "[==" / "]==" at |p|: returns the number of '=' and the consumed length, or
// -1 when this is not a long bracket.
static int _LuaLongBracket(const char* p, const char* end, size_t* consumed) {
    if ((p >= end) || ((*p != '[') && (*p != ']'))) {
        return -1;
    }
    char const bracket = *p;
    const char* q = p + 1;
    int level = 0;
    while ((q < end) && (*q == '=')) {
        ++level;
        ++q;
    }
    if ((q >= end) || (*q != bracket)) {
        return -1;
    }
    if (consumed) {
        *consumed = (size_t)(q + 1 - p);
    }
    return level;
}

// Position just past the "]===]" closing a long string/comment of |level|, or
// NULL when the terminator is on a later line.
static const char* _LuaLongEnd(const char* p, const char* end, int level) {
    while (p < end) {
        const char* const hit = (const char*)memchr(p, ']', (size_t)(end - p));
        if (!hit) {
            return NULL;
        }
        const char* q = hit + 1;
        int n = 0;
        while ((q < end) && (*q == '=') && (n < level)) {
            ++n;
            ++q;
        }
        if ((n == level) && (q < end) && (*q == ']')) {
            return q + 1;
        }
        p = hit + 1;
    }
    return NULL;
}

static int _LuaTokenize(CE_LUA_MODEL* model, const char* p, const char* end,
                        DocPos lineBase, CE_TOK* toks, int maxToks) {
    static const char* const s_ops[] = { "==", "~=", "<=", ">=", "..", "//", "::", "<<", ">>" };
    int n = 0;
    const char* r = p;
    while ((r < end) && (n < maxToks)) {
        if (_IsBlank(*r) || (*r == '\r')) {
            ++r;
            continue;
        }
        // Both a long comment (--[=[ ... ]=]) and a long string ([[ ... ]])
        // are skipped until their matching "]===]".
        if (model->longLevel >= 0) {
            const char* const after = _LuaLongEnd(r, end, model->longLevel);
            if (!after) {
                return n;
            }
            model->longLevel = -1;
            r = after;
            continue;
        }
        if ((r[0] == '-') && ((r + 1) < end) && (r[1] == '-')) {
            size_t consumed = 0;
            int const level = _LuaLongBracket(r + 2, end, &consumed);
            if (level >= 0) {
                model->longLevel = level;
                r = r + 2 + consumed;
                continue;
            }
            break; // ordinary line comment
        }
        if ((r[0] == '{') && ((r + 1) < end) && (r[1] == '$')) {
            // Skip the directive itself but keep lexing the rest of the line:
            // "{$lua} local x = 1" is valid and must declare x.  A line whose
            // directive switches to AA/C is never fed to this scanner at all.
            const char* close = NULL;
            if ((r + 2) < end) {
                close = (const char*)memchr(r + 2, '}', (size_t)(end - r - 2));
            }
            if (!close) {
                break;
            }
            r = close + 1;
            continue;
        }
        const char* const start = r;
        DocPos const pos = lineBase + (start - p);
        if (r[0] == '[') {
            size_t consumed = 0;
            int const level = _LuaLongBracket(r, end, &consumed);
            if (level >= 0) {
                const char* const body = r + consumed;
                const char* const after = _LuaLongEnd(body, end, level);
                if (after) {
                    r = after;
                }
                else {
                    model->longLevel = level;
                    r = end;
                }
                toks[n].kind = CE_TOK_STRING;
                toks[n].text = start;
                toks[n].len = (size_t)(r - start);
                toks[n].pos = pos;
                ++n;
                continue;
            }
        }
        if ((r[0] == '"') || (r[0] == '\'')) {
            char const quote = *r++;
            while ((r < end) && (*r != quote)) {
                if ((*r == '\\') && ((r + 1) < end)) {
                    ++r;
                }
                ++r;
            }
            if (r < end) {
                ++r;
            }
            toks[n].kind = CE_TOK_STRING;
            toks[n].text = start;
            toks[n].len = (size_t)(r - start);
            toks[n].pos = pos;
            ++n;
            continue;
        }
        if ((r[0] >= '0') && (r[0] <= '9')) {
            ++r;
            while ((r < end) && (_IsIdentChar(*r) || (*r == '.'))) {
                ++r;
            }
            toks[n].kind = CE_TOK_NUMBER;
            toks[n].text = start;
            toks[n].len = (size_t)(r - start);
            toks[n].pos = pos;
            ++n;
            continue;
        }
        if (_IsIdentStart(*r)) {
            while ((r < end) && _IsIdentChar(*r)) {
                ++r;
            }
            toks[n].kind = CE_TOK_IDENT;
            toks[n].text = start;
            toks[n].len = (size_t)(r - start);
            toks[n].pos = pos;
            ++n;
            continue;
        }
        size_t plen = 1;
        for (int i = 0; i < (int)COUNTOF(s_ops); ++i) {
            if (((size_t)(end - r) >= 2) && (memcmp(r, s_ops[i], 2) == 0)) {
                plen = 2;
                break;
            }
        }
        toks[n].kind = CE_TOK_PUNCT;
        toks[n].text = start;
        toks[n].len = plen;
        toks[n].pos = pos;
        ++n;
        r += plen;
    }
    return n;
}

static void _LuaAddSym(CE_LUA_MODEL* model, const CE_TOK* tok, int kind, DocLn line) {
    if ((tok->len == 0) || (tok->len >= COUNTOF(model->syms[0].name))) {
        return;
    }
    if (model->count >= CE_LUA_MAX_SYMS) {
        model->truncated = true;
        return;
    }
    CE_LUA_SYMBOL* const sym = &model->syms[model->count++];
    memcpy(sym->name, tok->text, tok->len);
    sym->name[tok->len] = '\0';
    sym->kind = kind;
    sym->pos = tok->pos;
    sym->scopeStart = (model->blockTop > 0) ? model->blockStart[model->blockTop - 1] : 0;
    sym->scopeEnd = -1;
    sym->line = line;
}

static void _LuaOpenBlock(CE_LUA_MODEL* model, DocPos pos) {
    if (model->blockTop >= CE_LUA_MAX_BLOCKS) {
        model->truncated = true;
        return;
    }
    model->blockStart[model->blockTop] = pos;
    model->blockEnd[model->blockTop] = -1;
    model->blockFirstSym[model->blockTop] = model->count;
    ++model->blockTop;
    if (model->blockTop > model->blockUsed) {
        model->blockUsed = model->blockTop;
    }
}

static void _LuaCloseBlock(CE_LUA_MODEL* model, DocPos pos) {
    if (model->blockTop <= 0) {
        return; // the block checker reports the orphaned end/until
    }
    --model->blockTop;
    model->blockEnd[model->blockTop] = pos;
    int const first = model->blockFirstSym[model->blockTop];
    for (int i = first; i < model->count; ++i) {
        CE_LUA_SYMBOL* const sym = &model->syms[i];
        if ((sym->scopeEnd < 0) &&
            ((sym->kind == CE_LUA_SYM_LOCAL) || (sym->kind == CE_LUA_SYM_PARAM) ||
             (sym->kind == CE_LUA_SYM_LOCALFN))) {
            sym->scopeEnd = pos;
        }
    }
}

static void _LuaCommitFuncName(CE_LUA_MODEL* model, DocLn line) {
    if (model->funcNameText && (model->funcNameLen > 0)) {
        CE_TOK tok;
        tok.kind = CE_TOK_IDENT;
        tok.text = model->funcNameText;
        tok.len = model->funcNameLen;
        tok.pos = model->funcNamePos;
        _LuaAddSym(model, &tok, model->pendingKind, line);
    }
    model->funcNameText = NULL;
    model->funcNameLen = 0;
    model->funcNamePos = 0;
}

// "a =", "a, b =" are assignment targets; "f(a, b)" and "x == 1" are not.
static bool _LuaAssignTarget(const CE_TOK* toks, int count, int i) {
    if ((i > 0) && (toks[i - 1].kind == CE_TOK_PUNCT) &&
        (_LuaTokIs(&toks[i - 1], ".") || _LuaTokIs(&toks[i - 1], ":") ||
         _LuaTokIs(&toks[i - 1], "]"))) {
        return false;
    }
    int j = i + 1;
    while ((j < count) && _LuaTokIs(&toks[j], ",")) {
        ++j;
        if ((j < count) && (toks[j].kind == CE_TOK_IDENT)) {
            ++j;
        }
        else {
            return false;
        }
    }
    return (j < count) && _LuaTokIs(&toks[j], "=");
}

void CeLuaScope_Init(CE_LUA_MODEL* model) {
    if (!model) {
        return;
    }
    memset(model, 0, sizeof(CE_LUA_MODEL));
    model->longLevel = -1;
}

void CeLuaScope_Line(CE_LUA_MODEL* model, const char* p, const char* end,
                     DocPos lineBase, DocLn line) {
    if (!model || !p || (end < p)) {
        return;
    }
    CE_TOK toks[CE_LUA_MAX_TOKENS];
    int const n = _LuaTokenize(model, p, end, lineBase, toks, CE_LUA_MAX_TOKENS);
    if (n >= CE_LUA_MAX_TOKENS) {
        model->truncated = true;
    }
    for (int i = 0; i < n; ++i) {
        const CE_TOK* const t = &toks[i];
        DocPos const pos = t->pos;

        if (model->pending == CE_LUA_P_PARAMS) {
            if ((t->kind == CE_TOK_PUNCT) && _LuaTokIs(t, "(")) {
                ++model->paramDepth;
            }
            else if ((t->kind == CE_TOK_PUNCT) && _LuaTokIs(t, ")")) {
                if (--model->paramDepth <= 0) {
                    model->pending = CE_LUA_P_NONE;
                }
            }
            else if (t->kind == CE_TOK_IDENT) {
                _LuaAddSym(model, t, CE_LUA_SYM_PARAM, line);
            }
            continue;
        }

        if ((model->pending == CE_LUA_P_LOCAL) || (model->pending == CE_LUA_P_FOR)) {
            bool const inFor = (model->pending == CE_LUA_P_FOR);
            if ((t->kind == CE_TOK_PUNCT) && _LuaTokIs(t, ",")) {
                continue;
            }
            if ((t->kind == CE_TOK_PUNCT) && _LuaTokIs(t, "=")) {
                model->pending = CE_LUA_P_NONE;
                continue;
            }
            if (inFor && _LuaWordIs(t, "in")) {
                model->pending = CE_LUA_P_NONE;
                continue;
            }
            if (t->kind == CE_TOK_IDENT) {
                if (!inFor && _LuaWordIs(t, "function")) {
                    // "local function f(...)": the name belongs to the outer
                    // scope, the parameters and body to the new block.
                    _LuaOpenBlock(model, pos);
                    model->pending = CE_LUA_P_FUNCNAME;
                    model->pendingKind = CE_LUA_SYM_LOCALFN;
                    model->funcNameText = NULL;
                    model->funcNameLen = 0;
                }
                else {
                    _LuaAddSym(model, t, CE_LUA_SYM_LOCAL, line);
                }
                continue;
            }
            model->pending = CE_LUA_P_NONE;
        }

        if (model->pending == CE_LUA_P_FUNCNAME) {
            if (t->kind == CE_TOK_IDENT) {
                // Only the last segment of "a.b:c" is the function name.
                model->funcNameText = t->text;
                model->funcNameLen = t->len;
                model->funcNamePos = pos;
                continue;
            }
            if ((t->kind == CE_TOK_PUNCT) && (_LuaTokIs(t, ".") || _LuaTokIs(t, ":"))) {
                continue;
            }
            if ((t->kind == CE_TOK_PUNCT) && _LuaTokIs(t, "(")) {
                _LuaCommitFuncName(model, line);
                model->pending = CE_LUA_P_PARAMS;
                model->paramDepth = 1;
                continue;
            }
            _LuaCommitFuncName(model, line);
            model->pending = CE_LUA_P_NONE;
        }

        if (t->kind == CE_TOK_PUNCT) {
            if (_LuaTokIs(t, "{")) {
                ++model->tableDepth;
            }
            else if (_LuaTokIs(t, "}") && (model->tableDepth > 0)) {
                --model->tableDepth;
            }
            continue;
        }
        if (t->kind != CE_TOK_IDENT) {
            continue;
        }

        if (_LuaWordIs(t, "local")) {
            model->pending = CE_LUA_P_LOCAL;
            continue;
        }
        if (_LuaWordIs(t, "function")) {
            _LuaOpenBlock(model, pos);
            model->pending = CE_LUA_P_FUNCNAME;
            model->pendingKind = (model->tableDepth > 0) ? CE_LUA_SYM_FIELD : CE_LUA_SYM_FUNCTION;
            model->funcNameText = NULL;
            model->funcNameLen = 0;
            continue;
        }
        if (_LuaWordIs(t, "for") || _LuaWordIs(t, "while")) {
            _LuaOpenBlock(model, pos);
            model->skipDo = true;
            if (_LuaWordIs(t, "for")) {
                model->pending = CE_LUA_P_FOR;
            }
            continue;
        }
        if (_LuaWordIs(t, "if") || _LuaWordIs(t, "repeat")) {
            _LuaOpenBlock(model, pos);
            continue;
        }
        if (_LuaWordIs(t, "do")) {
            if (model->skipDo) {
                model->skipDo = false;
            }
            else {
                _LuaOpenBlock(model, pos);
            }
            continue;
        }
        if (_LuaWordIs(t, "end") || _LuaWordIs(t, "until")) {
            _LuaCloseBlock(model, pos);
            model->skipDo = false;
            continue;
        }
        if (_LuaWordIs(t, "else") || _LuaWordIs(t, "elseif") || _LuaWordIs(t, "then") ||
            _LuaWordIs(t, "in") || _LuaWordIs(t, "return") || _LuaWordIs(t, "break") ||
            _LuaWordIs(t, "goto") || _LuaWordIs(t, "true") || _LuaWordIs(t, "false") ||
            _LuaWordIs(t, "nil") || _LuaWordIs(t, "and") || _LuaWordIs(t, "or") ||
            _LuaWordIs(t, "not")) {
            continue; // keywords are not variable references
        }
        // "[key] = value" inside a table constructor: the key is a field, and
        // never an undeclared variable in the enclosing scope.
        if ((i > 0) && _LuaTokIs(&toks[i - 1], "[") &&
            ((i + 1) < n) && _LuaTokIs(&toks[i + 1], "]") &&
            ((i + 2) < n) && _LuaTokIs(&toks[i + 2], "=")) {
            _LuaAddSym(model, t, CE_LUA_SYM_FIELD, line);
            continue;
        }
        if (_LuaAssignTarget(toks, n, i)) {
            int const kind = (model->tableDepth > 0) ? CE_LUA_SYM_FIELD : CE_LUA_SYM_GLOBAL;
            _LuaAddSym(model, t, kind, line);
        }
    }
}

void CeLuaScope_Finish(CE_LUA_MODEL* model, DocPos docEnd) {
    if (!model) {
        return;
    }
    for (int i = 0; i < model->count; ++i) {
        if (model->syms[i].scopeEnd < 0) {
            model->syms[i].scopeEnd = docEnd;
        }
    }
    for (int i = 0; i < model->blockUsed; ++i) {
        if (model->blockEnd[i] < 0) {
            model->blockEnd[i] = docEnd;
        }
    }
    // The pending function name points into the caller's text buffer; the
    // model may outlive that buffer, so never keep the pointer.
    model->funcNameText = NULL;
    model->funcNameLen = 0;
}

int CeLuaScope_BlockDepthAt(const CE_LUA_MODEL* model, DocPos pos) {
    if (!model) {
        return 0;
    }
    int depth = 0;
    for (int i = 0; i < model->blockUsed; ++i) {
        if ((model->blockStart[i] <= pos) && (pos < model->blockEnd[i])) {
            ++depth;
        }
    }
    return depth;
}

int CeLuaScope_Resolve(const CE_LUA_MODEL* model, const char* name, DocPos pos) {
    if (!model || !name || (name[0] == '\0')) {
        return CE_LUA_SYM_NONE;
    }
    for (int i = 0; i < model->count; ++i) {
        const CE_LUA_SYMBOL* const sym = &model->syms[i];
        if (_stricmp(sym->name, name) != 0) {
            continue;
        }
        if (sym->kind == CE_LUA_SYM_FIELD) {
            continue; // table fields are not resolved as variables
        }
        if ((sym->kind == CE_LUA_SYM_GLOBAL) || (sym->kind == CE_LUA_SYM_FUNCTION)) {
            return sym->kind;
        }
        if ((sym->pos < pos) && (sym->scopeStart <= pos) && (pos < sym->scopeEnd)) {
            return sym->kind;
        }
    }
    return CE_LUA_SYM_NONE;
}

bool CeLuaScope_DeclaredAnywhere(const CE_LUA_MODEL* model, const char* name) {
    if (!model || !name || (name[0] == '\0')) {
        return false;
    }
    for (int i = 0; i < model->count; ++i) {
        int const kind = model->syms[i].kind;
        if (((kind == CE_LUA_SYM_LOCAL) || (kind == CE_LUA_SYM_PARAM) ||
             (kind == CE_LUA_SYM_LOCALFN)) &&
            (_stricmp(model->syms[i].name, name) == 0)) {
            return true;
        }
    }
    return false;
}
