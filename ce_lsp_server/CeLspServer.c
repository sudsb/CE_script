// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspServer.c                                                                *
*   Built-in Language Server for Cheat Engine Lua / Auto Assembler scripts.    *
*                                                                             *
*   Standalone console process speaking LSP base protocol (Content-Length      *
*   framed JSON-RPC 2.0) over stdin/stdout.  It deliberately has no Win32,    *
*   Scintilla or editor dependency: the completion catalogue, the keyword     *
*   documentation and the script checks are served from the same generated    *
*   data that used to be compiled into Notepad3.exe.                           *
*                                                                             *
*   Supported: initialize / initialized / shutdown / exit,                     *
*              textDocument/didOpen|didChange|didSave|didClose,               *
*              textDocument/completion, completionItem/resolve,              *
*              textDocument/signatureHelp, textDocument/hover,                *
*              textDocument/definition, textDocument/declaration,             *
*              textDocument/references, textDocument/documentSymbol,          *
*              workspace/symbol, textDocument/formatting,                     *
*              textDocument/semanticTokens/full, textDocument/inlayHint,      *
*              textDocument/codeLens, textDocument/documentLink,              *
*              textDocument/documentHighlight,                                *
*              textDocument/publishDiagnostics (server -> client),            *
*              $/cancelRequest (accepted, ignored).                            *
*                                                                             *
*******************************************************************************/

#define _CRT_SECURE_NO_WARNINGS

#include "../src/CeLspJson.h"
#include "../src/CeLspFraming.h"
#include "../src/CeLspPos.h"
#include "../src/StyleLexers/ce_completions_data.h"
#include "ce_api_types.h"   // generated CE API type registry (tools/gen_ce_api_types.py)

#include <fcntl.h>
#include <limits.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CELSP_SERVER_NAME    "np3-ce-lsp"
#define CELSP_SERVER_VERSION "1.0"
#define CELSP_MAX_ITEMS      1200
#define CELSP_MAX_SYMBOLS    256
#define CELSP_MAX_DIAGS      400
#define CELSP_MAX_MSG        256

// ----------------------------------------------------------------------------
// Document state
// ----------------------------------------------------------------------------

static char*  g_docText = NULL;
static size_t g_docLen = 0u;
static char*  g_docUri = NULL;
static int    g_docVersion = 0;
static bool   g_docOpen = false;
static int    g_diagDirtyFirst = INT_MAX;
static int    g_diagDirtyLast = -1;
static int    g_tokenCacheVersion = -1;
static char*  g_tokenCacheData = NULL;
static bool   g_utf16 = false;   // position encoding negotiated in initialize
static bool   g_shutdown = false;
static bool   g_exit = false;

static char*  g_inBuf = NULL;
static size_t g_inLen = 0u;
static size_t g_inCap = 0u;

static void _SetText(const char* text)
{
    g_tokenCacheVersion = -1;
    free(g_tokenCacheData); g_tokenCacheData = NULL;
    free(g_docText);
    g_docText = NULL;
    g_docLen = 0u;
    if (text) {
        g_docLen = strlen(text);
        g_docText = (char*)malloc(g_docLen + 1u);
        if (g_docText) {
            memcpy(g_docText, text, g_docLen);
            g_docText[g_docLen] = '\0';
        }
        else {
            g_docLen = 0u;
        }
    }
}

static bool _ApplyRangeChange(const char* text, int startLine, int startChar,
                              int endLine, int endChar)
{
    if (!g_docText || (startLine < 0) || (endLine < 0) || (startChar < 0) || (endChar < 0) ||
        (endLine < startLine)) {
        return false;
    }
    int maxLine = 0;
    for (size_t i = 0u; i < g_docLen; ++i) {
        if (g_docText[i] == '\n') { ++maxLine; }
    }
    if ((startLine > maxLine) || (endLine > maxLine)) {
        return false;
    }
    const size_t start = CeLspPos_ToOffset(g_docText, g_docLen, startLine, startChar, g_utf16);
    const size_t end = CeLspPos_ToOffset(g_docText, g_docLen, endLine, endChar, g_utf16);
    if (end < start || end > g_docLen) {
        return false;
    }
    const size_t added = text ? strlen(text) : 0u;
    const size_t newLen = g_docLen - (end - start) + added;
    char* const next = (char*)malloc(newLen + 1u);
    if (!next) {
        return false;
    }
    memcpy(next, g_docText, start);
    if (added) {
        memcpy(next + start, text, added);
    }
    memcpy(next + start + added, g_docText + end, g_docLen - end);
    next[newLen] = '\0';
    free(g_docText);
    g_docText = next;
    g_docLen = newLen;
    g_tokenCacheVersion = -1;
    free(g_tokenCacheData); g_tokenCacheData = NULL;
    return true;
}

static void _SetUri(const char* uri)
{
    free(g_docUri);
    g_docUri = NULL;
    if (uri) {
        const size_t n = strlen(uri);
        g_docUri = (char*)malloc(n + 1u);
        if (g_docUri) {
            memcpy(g_docUri, uri, n);
            g_docUri[n] = '\0';
        }
    }
}

// ----------------------------------------------------------------------------
// Transport
// ----------------------------------------------------------------------------

static bool _InReserve(size_t extra)
{
    const size_t needed = g_inLen + extra + 1u;
    if (needed <= g_inCap) {
        return true;
    }
    size_t cap = (g_inCap == 0u) ? 16384u : g_inCap;
    while (cap < needed) {
        cap *= 2u;
    }
    char* const grown = (char*)realloc(g_inBuf, cap);
    if (!grown) {
        return false;
    }
    g_inBuf = grown;
    g_inCap = cap;
    return true;
}

static bool _Fill(void)
{
    char chunk[8192];
    // fread() keeps reading until it fills the requested count, which blocks on
    // a pipe until 8 KB arrive (or EOF).  _read() returns as soon as some bytes
    // are available, which is what an interactive LSP session needs.
    const int n = _read(_fileno(stdin), chunk, (unsigned int)sizeof(chunk));
    if ((n <= 0) || !_InReserve((size_t)n)) {
        return false;
    }
    memcpy(g_inBuf + g_inLen, chunk, (size_t)n);
    g_inLen += (size_t)n;
    g_inBuf[g_inLen] = '\0';
    return true;
}

static char* _ReadFrame(void)
{
    for (;;) {
        const int hdr = CeLspFraming_HeaderEnd(g_inBuf, g_inLen);
        if (hdr < 0) {
            if (!_Fill()) { return NULL; }
            continue;
        }
        size_t contentLen = 0u;
        if (!CeLspFraming_ContentLength(g_inBuf, (size_t)hdr, &contentLen)) {
            return NULL;
        }
        const size_t frameLen = (size_t)hdr + contentLen;
        if (g_inLen < frameLen) {
            if (!_Fill()) { return NULL; }
            continue;
        }
        char* const msg = (char*)malloc(contentLen + 1u);
        if (!msg) {
            return NULL;
        }
        memcpy(msg, g_inBuf + hdr, contentLen);
        msg[contentLen] = '\0';
        const size_t rest = g_inLen - frameLen;
        if (rest > 0u) {
            memmove(g_inBuf, g_inBuf + frameLen, rest);
        }
        g_inLen = rest;
        if (g_inBuf) {
            g_inBuf[g_inLen] = '\0';
        }
        return msg;
    }
}

static void _Send(const char* json)
{
    if (!json) {
        return;
    }
    size_t frameLen = 0u;
    char* const frame = CeLspFraming_Encode(json, strlen(json), &frameLen);
    if (frame) {
        if (fwrite(frame, 1u, frameLen, stdout) != frameLen) {
            // The client went away; exit quietly on the next loop.
            g_exit = true;
        }
        fflush(stdout);
        free(frame);
    }
}

static void _SendResult(int id, const CELSP_JSON* result)
{
    char* const json = CeLspFraming_Result(id, result);
    _Send(json);
    free(json);
}

static void _SendError(int id, int code, const char* message)
{
    char* const json = CeLspFraming_Error(id, code, message);
    _Send(json);
    free(json);
}

static void _SendNotification(const char* method, const CELSP_JSON* params)
{
    char* const json = CeLspFraming_Notification(method, params);
    _Send(json);
    free(json);
}

static void _Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[np3-ce-lsp] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

// ----------------------------------------------------------------------------
// Catalogue lookup
// ----------------------------------------------------------------------------

static const CE_COMPLETION_ENTRY* _FindEntry(const char* name)
{
    if (!name || !*name) {
        return NULL;
    }
    for (int i = 0; i < g_ceCompletionCount; ++i) {
        if (strcmp(g_ceCompletions[i].name, name) == 0) {
            return &g_ceCompletions[i];
        }
    }
    return NULL;
}

static const char* _FindDoc(const char* name)
{
    if (!name || !*name) {
        return NULL;
    }
    int lo = 0;
    int hi = g_ceCompletionDocCount - 1;
    while (lo <= hi) {
        const int mid = lo + ((hi - lo) / 2);
        const int cmp = strcmp(g_ceCompletionDocs[mid].name, name);
        if (cmp == 0) {
            return g_ceCompletionDocs[mid].doc;
        }
        if (cmp < 0) { lo = mid + 1; }
        else { hi = mid - 1; }
    }
    return NULL;
}

static int _KindToLsp(int kind)
{
    // LSP CompletionItemKind: Text 1, Method 2, Function 3, Field 5,
    // Variable 6, Class 7, Interface 8, Module 9, Property 10, Value 12,
    // Enum 13, Keyword 14, Snippet 15, Constant 21, Operator 25.
    switch (kind) {
    case CE_KIND_KEYWORD:  return 14;
    case CE_KIND_FUNCTION: return 3;
    case CE_KIND_VARIABLE: return 6;
    case CE_KIND_AA:       return 15;
    case CE_KIND_ASM:      return 25;
    case CE_KIND_TYPE:     return 7;
    case CE_KIND_CONSTANT: return 21;
    case CE_KIND_SECTION:  return 9;
    default:               return 1;
    }
}

static int _CaseInsensitivePrefix(const char* text, const char* prefix)
{
    if (!prefix || !*prefix) {
        return 1;
    }
    for (const char* a = text, *b = prefix; *b; ++a, ++b) {
        if (*a == '\0') {
            return 0;
        }
        char ca = *a;
        char cb = *b;
        if ((ca >= 'A') && (ca <= 'Z')) { ca = (char)(ca + ('a' - 'A')); }
        if ((cb >= 'A') && (cb <= 'Z')) { cb = (char)(cb + ('a' - 'A')); }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

// Document-defined symbols with their definition position (alloc/label/
// define/registersymbol, `name:` labels and Lua definitions).  The first
// definition of a name wins; name/kind is what completion still relies on.
typedef struct CE_SYMBOL
{
    char name[128];
    int  kind;      // CE_KIND_* for the AutoC image id
    int  symKind;   // LSP SymbolKind (12 function, 13 variable, 14 constant, 20 key)
    int  offset;    // byte offset of the name in the document
    int  nameLen;   // byte length of the name
    char detail[48]; // short description of the definition site
} CE_SYMBOL;

static CE_SYMBOL g_symbols[CELSP_MAX_SYMBOLS];
static int       g_symbolCount = 0;

// Identifier classes without the '.' that CeLspPos_IsIdentChar() accepts;
// whole-word scanning must not glue `a.b` into one token.
static bool _IsWordStart(unsigned char ch)
{
    return ((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z')) ||
           (ch == '_') || (ch == '$') || (ch >= 0x80u);
}

static bool _IsWordBody(unsigned char ch)
{
    return _IsWordStart(ch) || ((ch >= '0') && (ch <= '9'));
}

static bool _MemContains(const char* hay, size_t len, const char* needle)
{
    const size_t n = strlen(needle);
    if ((n == 0u) || (len < n)) {
        return false;
    }
    for (size_t i = 0u; (i + n) <= len; ++i) {
        if ((hay[i] == needle[0]) && (memcmp(hay + i, needle, n) == 0)) {
            return true;
        }
    }
    return false;
}

static void _AddSymbol(const char* name, size_t len, int kind, int symKind,
                       size_t nameOffset, const char* detail)
{
    if (!name || (len == 0u) || (len >= sizeof(g_symbols[0].name)) ||
        (g_symbolCount >= CELSP_MAX_SYMBOLS)) {
        return;
    }
    for (int i = 0; i < g_symbolCount; ++i) {
        if ((strlen(g_symbols[i].name) == len) && (strncmp(g_symbols[i].name, name, len) == 0)) {
            return; // keep the first definition of a name
        }
    }
    memcpy(g_symbols[g_symbolCount].name, name, len);
    g_symbols[g_symbolCount].name[len] = '\0';
    g_symbols[g_symbolCount].kind = kind;
    g_symbols[g_symbolCount].symKind = symKind;
    g_symbols[g_symbolCount].offset = (int)nameOffset;
    g_symbols[g_symbolCount].nameLen = (int)len;
    snprintf(g_symbols[g_symbolCount].detail, sizeof(g_symbols[g_symbolCount].detail),
             "%s", detail ? detail : "");
    ++g_symbolCount;
}

static void _ScanSymbols(void)
{
    g_symbolCount = 0;
    if (!g_docText) {
        return;
    }
    static const char* const directives[] = { "alloc(", "label(", "define(", "registersymbol(" };
    static const int dirSymKind[] = { 13, 13, 14, 13 };
    static const char* const dirDetail[] = { "alloc", "label", "define", "registersymbol" };
    bool inLua = false;
    const char* p = g_docText;
    while (*p) {
        const char* eol = strchr(p, '\n');
        const size_t lineLen = eol ? (size_t)(eol - p) : strlen(p);
        const size_t lineOff = (size_t)(p - g_docText);
        const bool luaRegion = inLua;
        if (_MemContains(p, lineLen, "{$lua}")) {
            inLua = !inLua;
        }
        if (!luaRegion) {
            for (size_t d = 0; d < sizeof(directives) / sizeof(directives[0]); ++d) {
                const char* hit = p;
                const char* const lineEnd = p + lineLen;
                while (hit < lineEnd) {
                    const size_t left = (size_t)(lineEnd - hit);
                    const char* const candidate = (const char*)memchr(hit, directives[d][0], left);
                    if (!candidate) {
                        break;
                    }
                    const size_t needleLen = strlen(directives[d]);
                    if ((left >= needleLen) && (strncmp(candidate, directives[d], needleLen) == 0)) {
                        const char* q = candidate + needleLen;
                        while ((q < lineEnd) && ((*q == ' ') || (*q == '\t'))) { ++q; }
                        const char* const start = q;
                        while ((q < lineEnd) && CeLspPos_IsIdentChar((unsigned char)*q)) { ++q; }
                        _AddSymbol(start, (size_t)(q - start), CE_KIND_VARIABLE, dirSymKind[d],
                                   lineOff + (size_t)(start - p), dirDetail[d]);
                        hit = q;
                    }
                    else {
                        hit = candidate + 1;
                    }
                }
            }
            // AA label definition: `name:` at the start of a line, colon ends the line.
            const char* s = p;
            while ((s < p + lineLen) && ((*s == ' ') || (*s == '\t'))) { ++s; }
            if ((s < p + lineLen) && _IsWordStart((unsigned char)*s)) {
                const char* w = s;
                while ((w < p + lineLen) && _IsWordBody((unsigned char)*w)) { ++w; }
                if ((w < p + lineLen) && (*w == ':')) {
                    const char* k = w + 1u;
                    while ((k < p + lineLen) && ((*k == ' ') || (*k == '\t') || (*k == '\r'))) { ++k; }
                    if ((k >= p + lineLen) || (*k == ';') ||
                        ((*k == '/') && (k + 1u < p + lineLen) && (k[1] == '/')) ||
                        ((*k == '-') && (k + 1u < p + lineLen) && (k[1] == '-'))) {
                        _AddSymbol(s, (size_t)(w - s), CE_KIND_VARIABLE, 20,
                                   lineOff + (size_t)(s - p), "label");
                    }
                }
            }
        }
        // Lua: "local name", "function name(" and "local function name("
        const char* q = p;
        while ((q < p + lineLen) && ((*q == ' ') || (*q == '\t'))) { ++q; }
        const char* word = q;
        if ((size_t)((p + lineLen) - q) >= 5u && (strncmp(q, "local", 5u) == 0)) {
            q += 5;
            while ((q < p + lineLen) && ((*q == ' ') || (*q == '\t'))) { ++q; }
            if ((size_t)((p + lineLen) - q) >= 8u && (strncmp(q, "function", 8u) == 0)) {
                q += 8;
                while ((q < p + lineLen) && ((*q == ' ') || (*q == '\t'))) { ++q; }
                word = q;
                while ((word < p + lineLen) && CeLspPos_IsIdentChar((unsigned char)*word)) { ++word; }
                _AddSymbol(q, (size_t)(word - q), CE_KIND_FUNCTION, 12,
                           lineOff + (size_t)(q - p), "function");
            }
            else {
                word = q;
                while ((word < p + lineLen) && CeLspPos_IsIdentChar((unsigned char)*word)) { ++word; }
                _AddSymbol(q, (size_t)(word - q), CE_KIND_VARIABLE, 13,
                           lineOff + (size_t)(q - p), "local");
            }
        }
        else if ((size_t)((p + lineLen) - q) >= 8u && (strncmp(q, "function", 8u) == 0)) {
            q += 8;
            while ((q < p + lineLen) && ((*q == ' ') || (*q == '\t'))) { ++q; }
            word = q;
            while ((word < p + lineLen) && CeLspPos_IsIdentChar((unsigned char)*word)) { ++word; }
            _AddSymbol(q, (size_t)(word - q), CE_KIND_FUNCTION, 12,
                       lineOff + (size_t)(q - p), "function");
        }
        // Lua: `name = function`
        {
            const char* w = p;
            while (w < p + lineLen) {
                if (_IsWordStart((unsigned char)*w)) {
                    const char* we = w;
                    while ((we < p + lineLen) && _IsWordBody((unsigned char)*we)) { ++we; }
                    const char* t = we;
                    while ((t < p + lineLen) && ((*t == ' ') || (*t == '\t'))) { ++t; }
                    if ((t < p + lineLen) && (*t == '=') &&
                        ((t + 1u >= p + lineLen) || (t[1] != '='))) {
                        ++t;
                        while ((t < p + lineLen) && ((*t == ' ') || (*t == '\t'))) { ++t; }
                        if (((size_t)((p + lineLen) - t) >= 8u) && (strncmp(t, "function", 8u) == 0) &&
                            ((t + 8u >= p + lineLen) || !_IsWordBody((unsigned char)t[8u]))) {
                            _AddSymbol(w, (size_t)(we - w), CE_KIND_FUNCTION, 12,
                                       lineOff + (size_t)(w - p), "function");
                        }
                    }
                    w = (we > w) ? (we + 1u) : (w + 1u);
                }
                else {
                    ++w;
                }
            }
        }
        if (!eol) { break; }
        p = eol + 1;
    }
}

// ----------------------------------------------------------------------------
// Diagnostics (the CE script rules, now living on the server side)
// ----------------------------------------------------------------------------

typedef struct CE_LSP_DIAG
{
    int  line;
    int  character;
    int  endCharacter;
    int  severity;   // 1 error, 2 warning
    char code[32];
    char message[CELSP_MAX_MSG];
} CE_LSP_DIAG;

static CE_LSP_DIAG g_diags[CELSP_MAX_DIAGS];
static int         g_diagCount = 0;

static void _AddDiag(size_t offset, size_t endOffset, int severity,
                     const char* code, const char* fmt, ...)
{
    if (g_diagCount >= CELSP_MAX_DIAGS) {
        return;
    }
    CE_LSP_DIAG* const d = &g_diags[g_diagCount++];
    CeLspPos_FromOffset(g_docText ? g_docText : "", g_docLen, offset,
                        g_utf16, &d->line, &d->character);
    int endLine = 0;
    CeLspPos_FromOffset(g_docText ? g_docText : "", g_docLen, endOffset,
                        g_utf16, &endLine, &d->endCharacter);
    d->severity = severity;
    snprintf(d->code, sizeof(d->code), "%s", code ? code : "ce");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->message, sizeof(d->message), fmt, ap);
    va_end(ap);
    d->message[sizeof(d->message) - 1u] = '\0';
}

static bool _LineIsComment(const char* line, size_t len)
{
    size_t i = 0u;
    while ((i < len) && ((line[i] == ' ') || (line[i] == '\t'))) { ++i; }
    return ((i + 1u < len) && (((line[i] == '/') && (line[i + 1u] == '/')) ||
                               ((line[i] == '-') && (line[i + 1u] == '-')) ||
                               (line[i] == ';')));
}

static void _RunExtraDiagnostics(void);   // implemented in the positional-features section

static void _RunDiagnostics(void)
{
    g_diagCount = 0;
    if (!g_docText) {
        return;
    }
    bool hasEnable = false;
    bool hasDisable = false;
    int blockOpen = 0; // 0 none, 1 {$lua}, 2 {$ccode}
    int longStringOpen = 0;
    size_t firstEnable = 0u;
    size_t blockOpenAt = 0u;
    size_t longStringAt = 0u;

    // Per-line bracket balance, section markers, alloc/label collection.
    char allocNames[CELSP_MAX_SYMBOLS][128];
    char labelNames[CELSP_MAX_SYMBOLS][128];
    char deallocNames[CELSP_MAX_SYMBOLS][128];
    int allocCount = 0, labelCount = 0, deallocCount = 0;

    const char* p = g_docText;
    while (p <= g_docText + g_docLen) {
        const char* const eol = strchr(p, '\n');
        const size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const size_t lineOff = (size_t)(p - g_docText);
        char line[512];
        const size_t copyLen = (len < (sizeof(line) - 1u)) ? len : (sizeof(line) - 1u);
        memcpy(line, p, copyLen);
        line[copyLen] = '\0';

        if (strstr(line, "[ENABLE]")) {
            hasEnable = true;
            if (firstEnable == 0u) { firstEnable = lineOff; }
        }
        if (strstr(line, "[DISABLE]")) { hasDisable = true; }
        {
            const char* const pLua = strstr(line, "{$lua}");
            const char* const pCcode = strstr(line, "{$ccode}");
            const char* const pCcode2 = strstr(line, "{$CODE}");
            if ((pCcode != NULL) || (pCcode2 != NULL)) {
                const char* const pOpen = (pCcode != NULL) ? pCcode : pCcode2;
                blockOpen = 2;
                blockOpenAt = lineOff + (size_t)(pOpen - line);
            }
            else if (pLua != NULL) {
                blockOpen = 1;
                blockOpenAt = lineOff + (size_t)(pLua - line);
            }
            if (strstr(line, "{$asm}") != NULL) {
                blockOpen = 0;
            }
        }

        if (!_LineIsComment(line, len)) {
            // Brackets outside quoted strings.
            int round = 0, square = 0, curly = 0;
            char quote = '\0';
            for (size_t i = 0u; i < len; ++i) {
                const char ch = p[i];
                if (quote) {
                    if (ch == quote) { quote = '\0'; }
                    continue;
                }
                if ((ch == '"') || (ch == '\'')) { quote = ch; continue; }
                if ((ch == '/') && (i + 1u < len) && (p[i + 1u] == '/')) { break; }
                if ((ch == '-') && (i + 1u < len) && (p[i + 1u] == '-')) { break; }
                switch (ch) {
                case '(': ++round; break;
                case ')': --round; break;
                case '[': ++square; break;
                case ']': --square; break;
                case '{': ++curly; break;
                case '}': --curly; break;
                default: break;
                }
            }
            if (round != 0) {
                _AddDiag(lineOff, lineOff + len, 1, "ce.brackets",
                         "Unbalanced parentheses (%d)", round);
            }
            if (square != 0) {
                _AddDiag(lineOff, lineOff + len, 1, "ce.brackets",
                         "Unbalanced square brackets (%d)", square);
            }
            if (curly != 0) {
                _AddDiag(lineOff, lineOff + len, 1, "ce.brackets",
                         "Unbalanced braces (%d)", curly);
            }
        }

        // Long Lua comments / strings.
        const char* q = p;
        while ((q < p + len) && (*q != '\0')) {
            if ((q + 3u < p + len) && (strncmp(q, "--[[", 4u) == 0)) {
                if (longStringOpen == 0) { longStringAt = lineOff + (size_t)(q - p); }
                ++longStringOpen;
                q += 4;
            }
            else if ((q + 1u < p + len) && (strncmp(q, "]]", 2u) == 0)) {
                if (longStringOpen > 0) { --longStringOpen; }
                q += 2;
            }
            else {
                ++q;
            }
        }

        // Directives.
        for (int k = 0; k < 3; ++k) {
            const char* const needle = (k == 0) ? "alloc(" : (k == 1) ? "label(" : "dealloc(";
            const char* hit = strstr(line, needle);
            while (hit) {
                const char* n = hit + strlen(needle);
                while ((*n == ' ') || (*n == '\t')) { ++n; }
                const char* const start = n;
                while (CeLspPos_IsIdentChar((unsigned char)*n)) { ++n; }
                const size_t nameLen = (size_t)(n - start);
                if ((nameLen > 0u) && (nameLen < 128u)) {
                    char name[128];
                    memcpy(name, start, nameLen);
                    name[nameLen] = '\0';
                    if (k == 0 && allocCount < CELSP_MAX_SYMBOLS) {
                        memcpy(allocNames[allocCount++], name, nameLen + 1u);
                    }
                    else if (k == 1) {
                        for (int i = 0; i < labelCount; ++i) {
                            if (strcmp(labelNames[i], name) == 0) {
                                _AddDiag(lineOff, lineOff + len, 2, "ce.duplicate-label",
                                         "Duplicate label '%s'", name);
                                break;
                            }
                        }
                        if (labelCount < CELSP_MAX_SYMBOLS) {
                            memcpy(labelNames[labelCount++], name, nameLen + 1u);
                        }
                    }
                    else if (k == 2 && deallocCount < CELSP_MAX_SYMBOLS) {
                        memcpy(deallocNames[deallocCount++], name, nameLen + 1u);
                    }
                }
                hit = strstr(hit + 1, needle);
            }
        }
        if (!eol) { break; }
        p = eol + 1;
        if (p > g_docText + g_docLen) { break; }
    }

    if (hasEnable && !hasDisable) {
        _AddDiag(firstEnable, firstEnable + 8u, 2, "ce.missing-disable",
                 "Script has [ENABLE] but no [DISABLE] section");
    }
    // {$lua}/{$ccode} open a block that the next {$asm} closes, so a single
    // "{$lua} ... {$asm}" pair is valid -- counting markers by parity flagged
    // the standard idiom as unmatched.  Track the open block instead.
    if (blockOpen != 0) {
        _AddDiag(blockOpenAt, blockOpenAt + 6u, 1, (blockOpen == 1) ? "ce.lua-block" : "ce.ccode-block",
                 (blockOpen == 1) ? "Unclosed {$lua} block (missing {$asm})"
                                  : "Unclosed {$ccode} block (missing {$asm})");
    }
    if (longStringOpen != 0) {
        _AddDiag(longStringAt, longStringAt + 4u, 1, "ce.long-string",
                 "Unclosed '--[[' block comment");
    }
    for (int i = 0; i < allocCount && (g_diagCount < CELSP_MAX_DIAGS); ++i) {
        bool freed = false;
        for (int j = 0; j < deallocCount; ++j) {
            if (strcmp(allocNames[i], deallocNames[j]) == 0) { freed = true; break; }
        }
        if (!freed) {
            _AddDiag(0u, 0u, 2, "ce.alloc-leak",
                     "alloc(%s) has no matching dealloc()", allocNames[i]);
        }
    }
    for (int i = 0; i < labelCount && (g_diagCount < CELSP_MAX_DIAGS); ++i) {
        // A label is only defined when it appears as "labelname:" too.
        char pattern[160];
        const int n = snprintf(pattern, sizeof(pattern), "%s:", labelNames[i]);
        if ((n > 0) && !strstr(g_docText, pattern)) {
            _AddDiag(0u, 0u, 2, "ce.undefined-label",
                     "label(%s) has no matching label definition", labelNames[i]);
        }
    }
    _ScanSymbols();
    _RunExtraDiagnostics();
}

static void _PublishCurrentDiagnostics(void)
{
    CELSP_JSON* const diags = CeLspJson_NewArray();
    if (diags) {
        for (int i = 0; i < g_diagCount; ++i) {
            const CE_LSP_DIAG* const d = &g_diags[i];
            CELSP_JSON* const start = CeLspJson_NewObject();
            CELSP_JSON* const end = CeLspJson_NewObject();
            CELSP_JSON* const range = CeLspJson_NewObject();
            CELSP_JSON* const item = CeLspJson_NewObject();
            if (!start || !end || !range || !item) {
                CeLspJson_Free(start); CeLspJson_Free(end);
                CeLspJson_Free(range); CeLspJson_Free(item);
                break;
            }
            CeLspJson_Set(start, "line", CeLspJson_NewNumber((double)d->line));
            CeLspJson_Set(start, "character", CeLspJson_NewNumber((double)d->character));
            CeLspJson_Set(end, "line", CeLspJson_NewNumber((double)d->line));
            CeLspJson_Set(end, "character", CeLspJson_NewNumber((double)d->endCharacter));
            CeLspJson_Set(range, "start", start);
            CeLspJson_Set(range, "end", end);
            CeLspJson_Set(item, "range", range);
            CeLspJson_Set(item, "severity", CeLspJson_NewNumber((double)d->severity));
            CeLspJson_Set(item, "code", CeLspJson_NewString(d->code));
            CeLspJson_Set(item, "source", CeLspJson_NewString(CELSP_SERVER_NAME));
            CeLspJson_Set(item, "message", CeLspJson_NewString(d->message));
            if (!CeLspJson_Add(diags, item)) {
                CeLspJson_Free(item);
                break;
            }
        }
    }
    CELSP_JSON* const params = CeLspJson_NewObject();
    if (params) {
        CeLspJson_Set(params, "uri", CeLspJson_NewString(g_docUri ? g_docUri : "file:///untitled.cea"));
        CeLspJson_Set(params, "version", CeLspJson_NewNumber((double)g_docVersion));
        CeLspJson_Set(params, "diagnostics", diags);
    }
    else {
        CeLspJson_Free(diags);
    }
    _SendNotification("textDocument/publishDiagnostics", params);
    CeLspJson_Free(params);
}

static void _PublishDiagnostics(void)
{
    _RunDiagnostics();
    _PublishCurrentDiagnostics();
}

static void _PublishDiagnosticsForLines(int firstLine, int lastLine)
{
    if (firstLine > lastLine) {
        _PublishDiagnostics();
        return;
    }
    static CE_LSP_DIAG oldDiags[CELSP_MAX_DIAGS];
    static CE_LSP_DIAG merged[CELSP_MAX_DIAGS];
    const int oldCount = g_diagCount;
    if (oldCount > 0) {
        memcpy(oldDiags, g_diags, (size_t)oldCount * sizeof(oldDiags[0]));
    }
    _RunDiagnostics();
    int mergedCount = 0;
    for (int i = 0; (i < g_diagCount) && (mergedCount < CELSP_MAX_DIAGS); ++i) {
        if ((g_diags[i].line >= firstLine) && (g_diags[i].line <= lastLine)) {
            merged[mergedCount++] = g_diags[i];
        }
    }
    for (int i = 0; (i < oldCount) && (mergedCount < CELSP_MAX_DIAGS); ++i) {
        if ((oldDiags[i].line < firstLine) || (oldDiags[i].line > lastLine)) {
            merged[mergedCount++] = oldDiags[i];
        }
    }
    for (int i = 0; i < mergedCount; ++i) {
        g_diags[i] = merged[i];
    }
    g_diagCount = mergedCount;
    _PublishCurrentDiagnostics();
}

// ----------------------------------------------------------------------------
// Document analysis: locals, their lexical scope and inferred types, plus
// fields collected from `owner.field = ...` assignments.  This drives
// receiver-aware member completion (e.g. `al.` for a local holding the
// Addresslist) instead of dumping the whole static catalogue.
// ----------------------------------------------------------------------------

#define CELSP_MAX_LOCALS   1024
#define CELSP_MAX_FIELDS   2048
#define CELSP_MAX_LINES    8192

typedef struct CE_LOCAL
{
    char name[64];
    int  declLine;   // 0-based line where declared
    int  declDepth;  // block nesting depth at declaration
    char type[40];   // inferred type name, "" when unknown
} CE_LOCAL;

typedef struct CE_FIELD
{
    char owner[64];
    char field[64];
} CE_FIELD;

static CE_LOCAL g_locals[CELSP_MAX_LOCALS];
static int      g_localCount = 0;
static CE_FIELD g_fields[CELSP_MAX_FIELDS];
static int      g_fieldCount = 0;
static int      g_depthAt[CELSP_MAX_LINES];
static int      g_lineCount = 0;
static char     g_typeBuf[40];   // scratch for inferred type names

// Count whole-word occurrences of `kw` in `s` (case-sensitive; Lua keywords).
static int _CountKeyword(const char* s, const char* kw)
{
    if (!s || !kw) {
        return 0;
    }
    const size_t kwLen = strlen(kw);
    if (kwLen == 0u) {
        return 0;
    }
    int n = 0;
    for (const char* p = s; *p; ++p) {
        if (*p != *kw) {
            continue;
        }
        if (memcmp(p, kw, kwLen) != 0) {
            continue;
        }
        const char before = (p > s) ? p[-1] : ' ';
        const char after = p[kwLen];
        const bool bWord = !CeLspPos_IsIdentChar((unsigned char)before);
        const bool aWord = !CeLspPos_IsIdentChar((unsigned char)after);
        if (bWord && aWord) {
            ++n;
            p += kwLen - 1u;
        }
    }
    return n;
}

static void _AddLocal(const char* name, size_t len, int line, int depth, const char* type)
{
    if ((len == 0u) || (len >= sizeof(g_locals[0].name)) ||
        (g_localCount >= CELSP_MAX_LOCALS)) {
        return;
    }
    // Update the most recent same-name local in the same/outer scope instead
    // of shadowing with a duplicate entry we would never query.
    for (int i = g_localCount - 1; i >= 0; --i) {
        if ((strlen(g_locals[i].name) == len) &&
            (strncmp(g_locals[i].name, name, len) == 0) &&
            (g_locals[i].declDepth <= depth)) {
            memcpy(g_locals[i].name, name, len);
            g_locals[i].name[len] = '\0';
            g_locals[i].declLine = line;
            g_locals[i].declDepth = depth;
            snprintf(g_locals[i].type, sizeof(g_locals[i].type), "%s",
                     type ? type : "");
            return;
        }
        if (g_locals[i].declDepth < depth) {
            break; // already past the declaring scope
        }
    }
    memcpy(g_locals[g_localCount].name, name, len);
    g_locals[g_localCount].name[len] = '\0';
    g_locals[g_localCount].declLine = line;
    g_locals[g_localCount].declDepth = depth;
    snprintf(g_locals[g_localCount].type, sizeof(g_locals[g_localCount].type),
             "%s", type ? type : "");
    ++g_localCount;
}

static void _AddField(const char* owner, size_t olen, const char* field, size_t flen)
{
    if ((olen == 0u) || (flen == 0u) ||
        (olen >= sizeof(g_fields[0].owner)) || (flen >= sizeof(g_fields[0].field)) ||
        (g_fieldCount >= CELSP_MAX_FIELDS)) {
        return;
    }
    for (int i = 0; i < g_fieldCount; ++i) {
        if ((strncmp(g_fields[i].owner, owner, olen) == 0) &&
            (strncmp(g_fields[i].field, field, flen) == 0)) {
            return; // already known
        }
    }
    memcpy(g_fields[g_fieldCount].owner, owner, olen);
    g_fields[g_fieldCount].owner[olen] = '\0';
    memcpy(g_fields[g_fieldCount].field, field, flen);
    g_fields[g_fieldCount].field[flen] = '\0';
    ++g_fieldCount;
}

// Infer a type name from the RHS of an assignment (after the '=').
static void _InferType(const char* rhs, char* outType, size_t cap)
{
    outType[0] = '\0';
    if (!rhs) {
        return;
    }
    while (*rhs == ' ' || *rhs == '\t') { ++rhs; }
    if (*rhs == '\0') {
        return;
    }
    if (*rhs == '{') {
        snprintf(outType, cap, "table");
        return;
    }
    if (*rhs == '"' || *rhs == '\'') {
        snprintf(outType, cap, "string");
        return;
    }
    if ((*rhs >= '0' && *rhs <= '9') || (*rhs == '-' && rhs[1] >= '0' && rhs[1] <= '9')) {
        snprintf(outType, cap, "number");
        return;
    }
    if (strncmp(rhs, "true", 4) == 0 || strncmp(rhs, "false", 5) == 0) {
        snprintf(outType, cap, "boolean");
        return;
    }
    // function call: callee name before the first '(' -> return-type map.
    const char* p = rhs;
    while (*p && *p != '(' && *p != ' ' && *p != '\t' && *p != '"' && *p != '\'') {
        ++p;
    }
    if (*p == '(' && (p > rhs)) {
        char fn[64];
        const size_t n = (size_t)(p - rhs);
        if (n < sizeof(fn)) {
            memcpy(fn, rhs, n);
            fn[n] = '\0';
            const char* const ret = CeApi_FunctionReturnType(fn);
            if (ret) {
                snprintf(outType, cap, "%s", ret);
            }
        }
    }
}

// Parse a single line for `owner.field =` and `local name = ...` / `name = ...`.
static void _AnalyzeLine(const char* line, int lineNo, int depth)
{
    // 1) field assignment:  owner.field =  (also owner["field"] =)
    {
        const char* dot = line;
        while ((dot = strchr(dot, '.')) != NULL) {
            const char* const before = dot - 1;
            if ((before >= line) && CeLspPos_IsIdentChar((unsigned char)*before)) {
                // owner name ends at `before`
                const char* ostart = before;
                while ((ostart > line) && CeLspPos_IsIdentChar((unsigned char)ostart[-1])) {
                    --ostart;
                }
                const char* fstart = dot + 1;
                if (CeLspPos_IsIdentChar((unsigned char)*fstart)) {
                    const char* fend = fstart;
                    while (CeLspPos_IsIdentChar((unsigned char)*fend)) { ++fend; }
                    // ensure '=' follows on this line
                    const char* eq = fend;
                    while (*eq == ' ' || *eq == '\t') { ++eq; }
                    if (*eq == '=') {
                        _AddField(ostart, (size_t)(before - ostart + 1u),
                                  fstart, (size_t)(fend - fstart));
                    }
                }
            }
            ++dot;
        }
    }

    // 2) declaration / assignment: optional `local`, name, '=', rhs
    {
        const char* p = line;
        while (*p == ' ' || *p == '\t') { ++p; }
        bool isLocal = false;
        if (strncmp(p, "local", 5) == 0) {
            const char c = p[5];
            if (!CeLspPos_IsIdentChar((unsigned char)c)) {
                isLocal = true;
                p += 5;
                while (*p == ' ' || *p == '\t') { ++p; }
            }
        }
        else {
            // only treat top-level (depth 0) bare assignments as globals
            if (depth != 0) {
                return;
            }
            // ignore control keywords to avoid for/while/if condition vars
            if (strncmp(p, "if", 2) == 0 || strncmp(p, "for", 3) == 0 ||
                strncmp(p, "while", 5) == 0 || strncmp(p, "elseif", 6) == 0) {
                return;
            }
        }
        // name
        if (!CeLspPos_IsIdentChar((unsigned char)*p)) {
            return;
        }
        const char* const nstart = p;
        while (CeLspPos_IsIdentChar((unsigned char)*p)) { ++p; }
        const size_t nlen = (size_t)(p - nstart);
        // skip spaces and one '='
        while (*p == ' ' || *p == '\t') { ++p; }
        if (*p != '=') {
            return;
        }
        ++p;
        _InferType(p, g_typeBuf, sizeof(g_typeBuf));
        _AddLocal(nstart, nlen, lineNo, depth, isLocal ? g_typeBuf : g_typeBuf);
    }
}

static void _AnalyzeDocument(void)
{
    g_localCount = 0;
    g_fieldCount = 0;
    g_lineCount = 0;
    if (!g_docText) {
        return;
    }
    int depth = 0;
    const char* p = g_docText;
    int lineNo = 0;
    while (p <= g_docText + g_docLen) {
        const char* const eol = strchr(p, '\n');
        const size_t lineLen = eol ? (size_t)(eol - p) : strlen(p);
        if (lineNo < CELSP_MAX_LINES) {
            g_depthAt[lineNo] = depth;
        }
        g_lineCount = lineNo + 1;
        char buf[2048];
        const size_t cp = (lineLen < (sizeof(buf) - 1u)) ? lineLen : (sizeof(buf) - 1u);
        memcpy(buf, p, cp);
        buf[cp] = '\0';
        _AnalyzeLine(buf, lineNo, depth);
        // adjust depth after recording this line
        depth += _CountKeyword(buf, "function");
        depth += _CountKeyword(buf, "do");
        depth += _CountKeyword(buf, "then");
        depth += _CountKeyword(buf, "repeat");
        depth -= _CountKeyword(buf, "end");
        depth -= _CountKeyword(buf, "until");
        if (depth < 0) { depth = 0; }
        if (!eol) { break; }
        p = eol + 1;
        ++lineNo;
    }
}

// Find the local visible at `cursorLine`: the most recent (innermost) whose
// declaration precedes the cursor and whose block still encloses it.
static const CE_LOCAL* _FindLocal(const char* name, int cursorLine)
{
    const int dc = (cursorLine >= 0 && cursorLine < g_lineCount)
                       ? g_depthAt[cursorLine] : 0;
    const CE_LOCAL* best = NULL;
    for (int i = 0; i < g_localCount; ++i) {
        const CE_LOCAL* const l = &g_locals[i];
        if (strcmp(l->name, name) != 0) {
            continue;
        }
        if (l->declLine > cursorLine) {
            continue;
        }
        if (dc < l->declDepth) {
            continue; // declaring block already closed
        }
        if (!best || (l->declLine > best->declLine) ||
            (l->declLine == best->declLine && l->declDepth > best->declDepth)) {
            best = l;
        }
    }
    return best;
}

// ----------------------------------------------------------------------------
// np3watch completion context
// ----------------------------------------------------------------------------

// Words the np3watch AA comment accepts.  They mirror the lexer sets in
// ce_edit/ce_edit/lexer.py so completion, highlighting and parsing agree.
static const char* const g_watchDirectives[] = { "np3watch", "np3unwatch", "np3nowatch", NULL };
static const char* const g_watchTypes[] = {
    "byte", "word", "dword", "qword", "pointer", "float", "double",
    "string", "wstring", "array", "reg", "u8", "u16", "u32", "u64",
    "i8", "i16", "i32", "i64", "ptr", "f32", "f64", "str", "wstr",
    "arr", "auto", NULL,
};
static const char* const g_watchFormats[] = {
    "bin", "dec", "hex", "binary", "decimal", "hexadecimal", NULL,
};
static const char* const g_watchSwitches[] = { "on", "off", NULL };
static const char* const g_watchBlock[] = { "if", "begin", "end", NULL };
static const char* const g_watchRegisters[] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "eflags", "flags",
    "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip",
    "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
    "al", "bl", "cl", "dl", "ah", "bh", "ch", "dh",
    "sil", "dil", "bpl", "spl", NULL,
};

static bool _WatchWordIs(const char* const* table, const char* word, size_t len)
{
    for (; *table != NULL; ++table) {
        const char* const t = *table;
        size_t i = 0u;
        for (; t[i] != '\0'; ++i) {
            if (i >= len) {
                break;
            }
            char a = word[i];
            char b = t[i];
            if ((a >= 'A') && (a <= 'Z')) { a = (char)(a + ('a' - 'A')); }
            if ((b >= 'A') && (b <= 'Z')) { b = (char)(b + ('a' - 'A')); }
            if (a != b) {
                break;
            }
        }
        if ((t[i] == '\0') && (i == len)) {
            return true;
        }
    }
    return false;
}

static bool _IsWatchRegister(const char* name)
{
    if (!name) {
        return false;
    }
    const size_t len = strlen(name);
    if (_WatchWordIs(g_watchRegisters, name, len)) {
        return true;
    }
    if ((len >= 3) && ((name[0] == 'r') || (name[0] == 'R')) &&
        ((name[len - 1] == 'd') || (name[len - 1] == 'D') ||
         (name[len - 1] == 'b') || (name[len - 1] == 'B'))) {
        int idx = 0;
        for (size_t i = 1; (i + 1) < len; ++i) {
            if ((name[i] < '0') || (name[i] > '9')) {
                return false;
            }
            idx = (idx * 10) + (int)(name[i] - '0');
        }
        return (idx >= 8) && (idx <= 15);
    }
    return false;
}

static bool _IsWatchKeyword(const char* name)
{
    if (!name) {
        return false;
    }
    const size_t len = strlen(name);
    return _WatchWordIs(g_watchDirectives, name, len) ||
           _WatchWordIs(g_watchTypes, name, len) ||
           _WatchWordIs(g_watchFormats, name, len) ||
           _WatchWordIs(g_watchSwitches, name, len) ||
           _WatchWordIs(g_watchBlock, name, len) ||
           _IsWatchRegister(name);
}

static bool _IsWatchDirective(const char* name)
{
    return name && _WatchWordIs(g_watchDirectives, name, strlen(name));
}

// True when the caret line is an AA comment that starts with an np3watch
// directive.  Both comment forms the parser accepts (// and ;) are covered.
static bool _LineIsWatchDirective(size_t offset)
{
    if (!g_docText || (offset > g_docLen)) {
        return false;
    }
    size_t ls = offset;
    while ((ls > 0u) && (g_docText[ls - 1u] != '\n')) {
        --ls;
    }
    size_t i = ls;
    while ((i < g_docLen) && ((g_docText[i] == ' ') || (g_docText[i] == '\t'))) {
        ++i;
    }
    if (((i + 1u) < g_docLen) && (g_docText[i] == '/') && (g_docText[i + 1u] == '/')) {
        i += 2u;
    }
    else if ((i < g_docLen) && (g_docText[i] == ';')) {
        ++i;
    }
    else {
        return false;
    }
    while ((i < g_docLen) && ((g_docText[i] == ' ') || (g_docText[i] == '\t'))) {
        ++i;
    }
    const size_t s = i;
    while ((i < g_docLen) && _IsWordBody((unsigned char)g_docText[i])) {
        ++i;
    }
    return (i > s) && _WatchWordIs(g_watchDirectives, g_docText + s, i - s);
}

// Region scan copied from the diagnostics pass: 0 AA, 1 Lua, 2 asm, 3 ccode.
// np3watch comments are meaningful in the AA and {$asm} sections.
static bool _IsAaRegionAt(size_t offset)
{
    if (!g_docText) {
        return false;
    }
    int region = 0;
    size_t lineOff = 0u;
    while ((lineOff <= offset) && (lineOff <= g_docLen)) {
        const char* const p = g_docText + lineOff;
        const char* const eol = (const char*)memchr(p, '\n', g_docLen - lineOff);
        const size_t len = eol ? (size_t)(eol - p) : (g_docLen - lineOff);
        if (_MemContains(p, len, "{$lua}")) {
            region = (region == 1) ? 0 : 1;
        }
        else if (_MemContains(p, len, "{$asm}")) {
            region = (region == 2) ? 1 : 2;
        }
        else if (_MemContains(p, len, "{$ccode}") || _MemContains(p, len, "{$CODE}")) {
            region = (region == 3) ? 1 : 3;
        }
        if ((lineOff + len) >= offset) {
            break;
        }
        lineOff += len + 1u;
    }
    return (region == 0) || (region == 2);
}

// Lower rank sorts first in clients that honour sortText.  Only the items the
// current context cares about get a rank; everything else keeps the client's
// default label ordering.
static void _SetSortText(CELSP_JSON* item, int rank, const char* label)
{
    if (!item || !label) {
        return;
    }
    char buf[320];
    snprintf(buf, sizeof(buf), "%02d%s", rank, label);
    CeLspJson_Set(item, "sortText", CeLspJson_NewString(buf));
}

// ----------------------------------------------------------------------------
// Completion / resolve / signatureHelp / hover
// ----------------------------------------------------------------------------

static CELSP_JSON* _MakeItem(const char* label, const char* detail, const char* doc,
                             int kind, bool snippet, int dataIndex)
{
    CELSP_JSON* const item = CeLspJson_NewObject();
    if (!item) {
        return NULL;
    }
    CeLspJson_Set(item, "label", CeLspJson_NewString(label));
    CeLspJson_Set(item, "kind", CeLspJson_NewNumber((double)_KindToLsp(kind)));
    CeLspJson_Set(item, "detail", CeLspJson_NewString(detail ? detail : ""));
    if (doc && *doc) {
        CELSP_JSON* const documentation = CeLspJson_NewObject();
        if (documentation) {
            CeLspJson_Set(documentation, "kind", CeLspJson_NewString("markdown"));
            CeLspJson_Set(documentation, "value", CeLspJson_NewString(doc));
            CeLspJson_Set(item, "documentation", documentation);
        }
    }
    if (snippet) {
        const size_t n = strlen(label) + 8u;
        char* const text = (char*)malloc(n);
        if (text) {
            snprintf(text, n, "%s($0)", label);
            CeLspJson_Set(item, "insertText", CeLspJson_NewString(text));
            free(text);
        }
        CeLspJson_Set(item, "insertTextFormat", CeLspJson_NewNumber(2.0)); // Snippet
    }
    else {
        CeLspJson_Set(item, "insertText", CeLspJson_NewString(label));
        CeLspJson_Set(item, "insertTextFormat", CeLspJson_NewNumber(1.0)); // PlainText
    }
    if (dataIndex >= 0) {
        CELSP_JSON* const data = CeLspJson_NewObject();
        if (data) {
            CeLspJson_Set(data, "index", CeLspJson_NewNumber((double)dataIndex));
            CeLspJson_Set(item, "data", data);
        }
    }
    return item;
}

static void _HandleCompletion(int id, const CELSP_JSON* params)
{
    _ScanSymbols();
    const CELSP_JSON* const pos = CeLspJson_Get(params, "position");
    int line = CeLspJson_GetInt(pos, "line");
    int character = CeLspJson_GetInt(pos, "character");
    const size_t offset = CeLspPos_ToOffset(g_docText, g_docLen, line, character, g_utf16);
    size_t wordStart = CeLspPos_WordStart(g_docText, g_docLen, offset);
    // Section markers such as {$lua}/{$asm} are not identifier-shaped.  '$' is
    // an identifier character here, so WordStart() normally stops between '{'
    // and '$'.  Include the opener in the prefix so those catalogue entries
    // match and replace the whole marker opener instead of leaving a stray '{'.
    if (g_docText && (wordStart <= g_docLen)) {
        char const left2 = (wordStart >= 2u) ? g_docText[wordStart - 2u] : '\0';
        char const left1 = (wordStart >= 1u) ? g_docText[wordStart - 1u] : '\0';
        char const first = (wordStart < g_docLen) ? g_docText[wordStart] : '\0';
        int const width = CeLspPos_SectionOpenerWidth(left2, left1, first);
        if ((width > 0) && (wordStart >= (size_t)width)) {
            wordStart -= (size_t)width;
        }
    }

    // Member access detection.  CeLspPos_IsIdentChar() treats '.' as a word
    // character, so we scan back manually and stop at the separator: the
    // identifier immediately before the last '.' is the receiver, and the text
    // between the dot and the cursor is the (possibly empty) member prefix.
    char receiver[64];
    receiver[0] = '\0';
    size_t memberPrefixStart = wordStart;
    {
        size_t i = offset;
        while (i > 0u) {
            const char c = g_docText[i - 1u];
            if ((c == '.') || !CeLspPos_IsIdentChar((unsigned char)c)) {
                break;
            }
            --i;
        }
        if ((i > 0u) && (g_docText[i - 1u] == '.')) {
            size_t k = i - 1u;
            while ((k > 0u) && CeLspPos_IsIdentChar((unsigned char)g_docText[k - 1u]) &&
                   (g_docText[k - 1u] != '.')) {
                --k;
            }
            const size_t rlen = (i - 1u) - k;
            if ((rlen > 0u) && (rlen < sizeof(receiver))) {
                memcpy(receiver, g_docText + k, rlen);
                receiver[rlen] = '\0';
                memberPrefixStart = i; // start of the typed member prefix
            }
        }
    }

    char prefix[256];
    const size_t prefixLen = (offset >= memberPrefixStart) ? (offset - memberPrefixStart) : 0u;
    const size_t copyLen = (prefixLen < (sizeof(prefix) - 1u)) ? prefixLen : (sizeof(prefix) - 1u);
    if ((copyLen > 0u) && g_docText) {
        memcpy(prefix, g_docText + memberPrefixStart, copyLen);
    }
    prefix[copyLen] = '\0';

    // np3watch context: on a directive comment line every watch word is a
    // first-class candidate; elsewhere in the Auto Assembler section only the
    // directives are lifted above the generic Lua/API noise.
    const bool inAa = _IsAaRegionAt(offset);
    const bool watchLine = inAa && _LineIsWatchDirective(offset);
    const bool aaBoost = inAa && !watchLine &&
                         _CaseInsensitivePrefix("np3watch", prefix);

    CELSP_JSON* const items = CeLspJson_NewArray();
    int count = 0;

    if (receiver[0] != '\0') {
        // ---- receiver-aware member completion ----
        _AnalyzeDocument();
        const CE_API_TYPE* t = NULL;
        char typeName[40];
        typeName[0] = '\0';
        const CE_LOCAL* const loc = _FindLocal(receiver, line);
        if (loc && loc->type[0]) {
            snprintf(typeName, sizeof(typeName), "%s", loc->type);
        }
        if (typeName[0] && (strcmp(typeName, "table") == 0)) {
            // user-defined table: offer fields collected from `owner.field =`
            for (int i = 0; (i < g_fieldCount) && (count < CELSP_MAX_ITEMS); ++i) {
                if (strcmp(g_fields[i].owner, receiver) != 0) {
                    continue;
                }
                if (!_CaseInsensitivePrefix(g_fields[i].field, prefix)) {
                    continue;
                }
                CELSP_JSON* const item = _MakeItem(g_fields[i].field, "local table field",
                                                   NULL, CE_KIND_VARIABLE, false, -1);
                if (item && CeLspJson_Add(items, item)) {
                    ++count;
                }
                else {
                    CeLspJson_Free(item);
                }
            }
        }
        else {
            if (!typeName[0]) {
                t = CeApi_FindType(receiver); // built-in module or CE type, used directly
            }
            else {
                t = CeApi_FindType(typeName);  // inferred type of the local
            }
            if (t) {
                for (int i = 0; (i < t->memberCount) && (count < CELSP_MAX_ITEMS); ++i) {
                    const CE_API_MEMBER* const m = &t->members[i];
                    if (!_CaseInsensitivePrefix(m->name, prefix)) {
                        continue;
                    }
                    // The client never sends completionItem/resolve, so the
                    // documentation must travel inline with the item.  The
                    // catalogue keys are module-qualified ("math.ceil") while
                    // the label is the bare member name, so look the entry up
                    // through its qualified name.  The member list is small,
                    // which makes inlining affordable even for an empty prefix.
                    char qualified[128];
                    snprintf(qualified, sizeof(qualified), "%s.%s", t->name, m->name);
                    const CE_COMPLETION_ENTRY* const e = _FindEntry(qualified);
                    const char* const detail = (e && e->signature) ? e->signature
                                             : (m->type ? m->type : t->name);
                    const char* const doc = _FindDoc(qualified);
                    const int dataIndex = e ? (int)(e - g_ceCompletions) : -1;
                    CELSP_JSON* const item = _MakeItem(m->name, detail, doc,
                                                       m->kind, false, dataIndex);
                    if (item && CeLspJson_Add(items, item)) {
                        ++count;
                    }
                    else {
                        CeLspJson_Free(item);
                    }
                }
            }
            // Unknown receiver -> empty list: no generic catalogue spam
            // (this is exactly what removes `math.floor` from `foo.` lists).
        }
    }
    else {
        // ---- word / glob completion (no dot) ----
        // Module-qualified catalogue entries ("math.floor") are member
        // completions for their dot only; a plain word root must never match
        // them, otherwise typing "math" floods the list with math.* entries.
        const bool qualifiedPrefix = strchr(prefix, '.') != NULL;
        for (int i = 0; (i < g_ceCompletionCount) && (count < CELSP_MAX_ITEMS); ++i) {
            const CE_COMPLETION_ENTRY* const e = &g_ceCompletions[i];
            if (!_CaseInsensitivePrefix(e->name, prefix)) {
                continue;
            }
            if (!qualifiedPrefix && strchr(e->name, '.')) {
                continue;
            }
            const char* const doc = (prefixLen > 0u) ? _FindDoc(e->name)
                                   : ((watchLine && _IsWatchKeyword(e->name)) ? _FindDoc(e->name) : NULL);
            CELSP_JSON* const item = _MakeItem(e->name, e->signature, doc, e->kind,
                                               (e->isCall != 0), i);
            if (item) {
                const int rank = watchLine ? (_IsWatchKeyword(e->name) ? 0 : 2)
                              : ((aaBoost && _IsWatchDirective(e->name)) ? 0 : -1);
                if (rank >= 0) {
                    _SetSortText(item, rank, e->name);
                }
            }
            if (item && CeLspJson_Add(items, item)) {
                ++count;
            }
            else {
                CeLspJson_Free(item);
            }
        }
        for (int i = 0; (i < g_symbolCount) && (count < CELSP_MAX_ITEMS); ++i) {
            if (!_CaseInsensitivePrefix(g_symbols[i].name, prefix)) {
                continue;
            }
            if (_FindEntry(g_symbols[i].name)) {
                continue; // already in the catalogue
            }
            CELSP_JSON* const item = _MakeItem(g_symbols[i].name, "document symbol", NULL,
                                               g_symbols[i].kind, false, -1);
            if (item && watchLine) {
                _SetSortText(item, 1, g_symbols[i].name);
            }
            if (item && CeLspJson_Add(items, item)) {
                ++count;
            }
            else {
                CeLspJson_Free(item);
            }
        }
        // Registered CE object types and built-in modules as word candidates:
        // with the dotted catalogue entries ("math.floor") excluded from
        // plain-word matching, a module name like "math" would otherwise have
        // no suggestion at all.  Selecting it and typing '.' lands in member
        // completion.  g_ceApiTypes comes from ce_api_types.h and lives in
        // this translation unit, so it is iterated directly.
        for (int i = 0; g_ceApiTypes[i].name && (count < CELSP_MAX_ITEMS); ++i) {
            const char* const tname = g_ceApiTypes[i].name;
            if (!_CaseInsensitivePrefix(tname, prefix) || _FindEntry(tname)) {
                continue;
            }
            CELSP_JSON* const item = _MakeItem(tname, "CE class / module", NULL,
                                               CE_KIND_TYPE, false, -1);
            if (item && watchLine) {
                _SetSortText(item, 3, tname);
            }
            if (item && CeLspJson_Add(items, item)) {
                ++count;
            }
            else {
                CeLspJson_Free(item);
            }
        }
    }

    CELSP_JSON* const result = CeLspJson_NewObject();
    if (result) {
        CeLspJson_Set(result, "isIncomplete", CeLspJson_NewBool(false));
        CeLspJson_Set(result, "items", items);
    }
    else {
        CeLspJson_Free(items);
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

static void _HandleResolve(int id, const CELSP_JSON* params)
{
    const char* const label = CeLspJson_GetString(params, "label");
    CELSP_JSON* const result = CeLspJson_NewObject();
    if (result) {
        CeLspJson_Set(result, "label", CeLspJson_NewString(label ? label : ""));
        const char* const detail = CeLspJson_GetString(params, "detail");
        CeLspJson_Set(result, "detail", CeLspJson_NewString(detail ? detail : ""));
        const CE_COMPLETION_ENTRY* const e = _FindEntry(label);
        const char* const doc = _FindDoc(label);
        char body[4096];
        body[0] = '\0';
        if (e && e->signature) {
            snprintf(body, sizeof(body), "`%s`", e->signature);
        }
        if (doc && *doc) {
            const size_t used = strlen(body);
            snprintf(body + used, sizeof(body) - used, "%s%s",
                     (used > 0u) ? "\n\n" : "", doc);
        }
        if (body[0] != '\0') {
            CELSP_JSON* const documentation = CeLspJson_NewObject();
            if (documentation) {
                CeLspJson_Set(documentation, "kind", CeLspJson_NewString("markdown"));
                CeLspJson_Set(documentation, "value", CeLspJson_NewString(body));
                CeLspJson_Set(result, "documentation", documentation);
            }
        }
        const char* const insertText = CeLspJson_GetString(params, "insertText");
        CeLspJson_Set(result, "insertText", CeLspJson_NewString(insertText ? insertText : (label ? label : "")));
        const double format = CeLspJson_GetNumber(params, "insertTextFormat");
        CeLspJson_Set(result, "insertTextFormat",
                      CeLspJson_NewNumber((format == 2.0) ? 2.0 : 1.0));
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

static void _HandleSignatureHelp(int id, const CELSP_JSON* params)
{
    const CELSP_JSON* const pos = CeLspJson_Get(params, "position");
    const size_t offset = CeLspPos_ToOffset(g_docText, g_docLen,
                                            CeLspJson_GetInt(pos, "line"),
                                            CeLspJson_GetInt(pos, "character"), g_utf16);
    // Walk back to the function name before the nearest unmatched '('.
    size_t i = offset;
    while ((i > 0u) && g_docText && (g_docText[i - 1u] != '(')) { --i; }
    if ((i > 0u) && g_docText) {
        i = CeLspPos_WordStart(g_docText, g_docLen, i - 1u);
        const size_t end = CeLspPos_WordEnd(g_docText, g_docLen, i);
        char name[256];
        const size_t n = (end - i < (sizeof(name) - 1u)) ? (end - i) : (sizeof(name) - 1u);
        memcpy(name, g_docText + i, n);
        name[n] = '\0';
        const CE_COMPLETION_ENTRY* const e = _FindEntry(name);
        const char* const doc = _FindDoc(name);
        CELSP_JSON* const signatures = CeLspJson_NewArray();
        if (e || doc) {
            CELSP_JSON* const sig = CeLspJson_NewObject();
            if (sig) {
                CeLspJson_Set(sig, "label", CeLspJson_NewString(e ? e->signature : name));
                if (doc && *doc) {
                    CELSP_JSON* const documentation = CeLspJson_NewObject();
                    if (documentation) {
                        CeLspJson_Set(documentation, "kind", CeLspJson_NewString("markdown"));
                        CeLspJson_Set(documentation, "value", CeLspJson_NewString(doc));
                        CeLspJson_Set(sig, "documentation", documentation);
                    }
                }
                if (!CeLspJson_Add(signatures, sig)) {
                    CeLspJson_Free(sig);
                }
            }
        }
        CELSP_JSON* const result = CeLspJson_NewObject();
        if (result) {
            CeLspJson_Set(result, "signatures", signatures);
            CeLspJson_Set(result, "activeSignature", CeLspJson_NewNumber(0.0));
            CeLspJson_Set(result, "activeParameter", CeLspJson_NewNumber(0.0));
        }
        else {
            CeLspJson_Free(signatures);
        }
        _SendResult(id, result);
        CeLspJson_Free(result);
        return;
    }
    _SendResult(id, NULL);
}

static void _HandleHover(int id, const CELSP_JSON* params)
{
    const CELSP_JSON* const pos = CeLspJson_Get(params, "position");
    const size_t offset = CeLspPos_ToOffset(g_docText, g_docLen,
                                            CeLspJson_GetInt(pos, "line"),
                                            CeLspJson_GetInt(pos, "character"), g_utf16);
    const size_t start = CeLspPos_WordStart(g_docText, g_docLen, offset);
    const size_t end = CeLspPos_WordEnd(g_docText, g_docLen, start);
    char name[256];
    const size_t n = ((end > start) && (end - start < (sizeof(name) - 1u))) ?
                     (end - start) : 0u;
    if ((n > 0u) && g_docText) {
        memcpy(name, g_docText + start, n);
    }
    name[n] = '\0';
    const CE_COMPLETION_ENTRY* const e = _FindEntry(name);
    const char* const doc = _FindDoc(name);
    if (!e && !doc) {
        _SendResult(id, NULL);
        return;
    }
    char body[4096];
    body[0] = '\0';
    if (e) {
        snprintf(body, sizeof(body), "### %s\n\n`%s`", e->name, e->signature);
    }
    else {
        snprintf(body, sizeof(body), "### %s", name);
    }
    if (doc && *doc) {
        const size_t used = strlen(body);
        snprintf(body + used, sizeof(body) - used, "\n\n%s", doc);
    }
    CELSP_JSON* const contents = CeLspJson_NewObject();
    CELSP_JSON* const result = CeLspJson_NewObject();
    if (contents && result) {
        CeLspJson_Set(contents, "kind", CeLspJson_NewString("markdown"));
        CeLspJson_Set(contents, "value", CeLspJson_NewString(body));
        CeLspJson_Set(result, "contents", contents);
    }
    else {
        CeLspJson_Free(contents);
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ----------------------------------------------------------------------------
// Positional features: definition / declaration / references / document and
// workspace symbols / formatting / semantic tokens / inlay hints / code lens /
// document links / highlight, plus the extended script diagnostics.
// ----------------------------------------------------------------------------

#define CELSP_MAX_REFS      1000
#define CELSP_MAX_EDITS     4096
#define CELSP_MAX_TOKENS    16384
#define CELSP_MAX_HINTS     200
#define CELSP_MAX_LENS      200
#define CELSP_MAX_LINKS     100
#define CELSP_MAX_HILITE    100
#define CELSP_DIAG_CAP      50
#define CELSP_MAX_WS_TOTAL  500
#define CELSP_MAX_WS_CAT    300

// Semantic token legend indices (order matters, it is advertised as-is).
#define CE_TOK_NAMESPACE   0
#define CE_TOK_TYPE        1
#define CE_TOK_FUNCTION    2
#define CE_TOK_VARIABLE    3
#define CE_TOK_KEYWORD     4
#define CE_TOK_STRING      5
#define CE_TOK_NUMBER      6
#define CE_TOK_COMMENT     7
#define CE_TOK_OPERATOR    8
#define CE_TOK_MACRO       9
#define CE_TOK_LABEL       10
#define CE_TOK_REGISTER    11
#define CE_TOK_INSTRUCTION 12
#define CE_TOK_API         13
#define CE_MOD_DECLARATION 0
#define CE_MOD_READONLY    1

static const char* const g_semTokenTypes[] = {
    "namespace", "type", "function", "variable", "keyword", "string", "number",
    "comment", "operator", "macro", "label", "register", "instruction", "api",
};
static const char* const g_semTokenModifiers[] = { "declaration", "readonly" };

static const char* const g_luaKeywords[] = {
    "if", "then", "else", "elseif", "end", "for", "while", "do", "repeat",
    "until", "function", "local", "return", "in", "nil", "true", "false",
    "and", "or", "not", NULL,
};
static const char* const g_luaStdlib[] = {
    "print", "pairs", "ipairs", "tonumber", "tostring", "type", "rawget",
    "rawset", "setmetatable", "getmetatable", "require", "pcall", "xpcall",
    "error", "assert", "table", "string", "math", "os", "io", "coroutine",
    "select", "unpack", "next", "format", "byte", "char", "sub", "rep",
    "find", "match", "gmatch", NULL,
};
static const char* const g_asmMnemonics[] = {
    "mov", "push", "pop", "call", "jmp", "add", "sub", "imul", "idiv", "and",
    "or", "xor", "not", "neg", "cmp", "test", "lea", "movzx", "movsxd",
    "ret", "nop", "je", "jne", "jb", "ja", "jae", "jbe", "jg", "jge", "jl",
    "jle", "loop", "inc", "dec", "xchg", "cdq", "cqo", "int", "leave",
    "retf", "jz", "jnz", "jcxz", "jecxz", "db", "dw", "dd", "dq", "align",
    NULL,
};
static const char* const g_jumpMnemonics[] = {
    "jmp", "call", "je", "jne", "jz", "jnz", "jb", "ja", "jae", "jbe",
    "jg", "jge", "jl", "jle", "loop", "jecxz", NULL,
};
static const char* const g_asmRegisters[] = {
    "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "ax", "bx",
    "cx", "dx", "si", "di", "bp", "sp", "al", "bl", "cl", "dl", "ah",
    "bh", "ch", "dh", "sil", "dil", "bpl", "spl", "rax", "rbx", "rcx",
    "rdx", "rsi", "rdi", "rbp", "rsp", "rip", "r8", "r9", "r10", "r11",
    "r12", "r13", "r14", "r15", "r8d", "r9d", "r10d", "r11d", "r12d",
    "r13d", "r14d", "r15d", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b",
    "r14b", "r15b", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
    "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14",
    "xmm15", NULL,
};
static const char* const g_aaDirectives[] = {
    "alloc", "label", "define", "dealloc", "registersymbol",
    "unregistersymbol", "newexec", "endalloc", "globalalloc", "fullaccess",
    "readmem", "getprocedureaddress", "code", "data", "executable",
    "includeasmfile", "injectdll", "enable", "disable", "np3watch",
    "np3unwatch", "np3nowatch", NULL,
};

static char _LowerCh(char c)
{
    return ((c >= 'A') && (c <= 'Z')) ? (char)(c + ('a' - 'A')) : c;
}

static bool _StrIEq(const char* a, size_t alen, const char* b)
{
    size_t i = 0u;
    for (; b[i] != '\0'; ++i) {
        if (i >= alen) {
            return false;
        }
        if (_LowerCh(a[i]) != _LowerCh(b[i])) {
            return false;
        }
    }
    return i == alen;
}

static bool _InTableI(const char* const* tbl, const char* w, size_t len)
{
    for (; *tbl != NULL; ++tbl) {
        if (_StrIEq(w, len, *tbl)) {
            return true;
        }
    }
    return false;
}

static bool _ContainsCI(const char* hay, const char* needle)
{
    if (!hay) {
        return false;
    }
    if (!needle || (*needle == '\0')) {
        return true;
    }
    const size_t hl = strlen(hay);
    const size_t nl = strlen(needle);
    if (nl > hl) {
        return false;
    }
    for (size_t i = 0u; (i + nl) <= hl; ++i) {
        size_t j = 0u;
        for (; j < nl; ++j) {
            if (_LowerCh(hay[i + j]) != _LowerCh(needle[j])) {
                break;
            }
        }
        if (j == nl) {
            return true;
        }
    }
    return false;
}

static void _LineBounds(size_t off, size_t* start, size_t* end)
{
    *start = 0u;
    *end = 0u;
    if (!g_docText) {
        return;
    }
    if (off > g_docLen) {
        off = g_docLen;
    }
    size_t s = off;
    while ((s > 0u) && (g_docText[s - 1u] != '\n')) {
        --s;
    }
    size_t e = off;
    while ((e < g_docLen) && (g_docText[e] != '\n')) {
        ++e;
    }
    *start = s;
    *end = e;
}

static CELSP_JSON* _MakeRange(size_t startOff, size_t endOff)
{
    CELSP_JSON* const range = CeLspJson_NewObject();
    CELSP_JSON* const start = CeLspJson_NewObject();
    CELSP_JSON* const end = CeLspJson_NewObject();
    if (!range || !start || !end) {
        CeLspJson_Free(range);
        CeLspJson_Free(start);
        CeLspJson_Free(end);
        return NULL;
    }
    const char* const text = g_docText ? g_docText : "";
    int line = 0, character = 0, endLine = 0, endCharacter = 0;
    CeLspPos_FromOffset(text, g_docLen, startOff, g_utf16, &line, &character);
    CeLspPos_FromOffset(text, g_docLen, endOff, g_utf16, &endLine, &endCharacter);
    CeLspJson_Set(start, "line", CeLspJson_NewNumber((double)line));
    CeLspJson_Set(start, "character", CeLspJson_NewNumber((double)character));
    CeLspJson_Set(end, "line", CeLspJson_NewNumber((double)endLine));
    CeLspJson_Set(end, "character", CeLspJson_NewNumber((double)endCharacter));
    CeLspJson_Set(range, "start", start);
    CeLspJson_Set(range, "end", end);
    return range;
}

static CELSP_JSON* _MakePosition(size_t off)
{
    CELSP_JSON* const pos = CeLspJson_NewObject();
    if (!pos) {
        return NULL;
    }
    const char* const text = g_docText ? g_docText : "";
    int line = 0, character = 0;
    CeLspPos_FromOffset(text, g_docLen, off, g_utf16, &line, &character);
    CeLspJson_Set(pos, "line", CeLspJson_NewNumber((double)line));
    CeLspJson_Set(pos, "character", CeLspJson_NewNumber((double)character));
    return pos;
}

static CELSP_JSON* _MakeLocation(size_t startOff, size_t endOff)
{
    CELSP_JSON* const loc = CeLspJson_NewObject();
    CELSP_JSON* const range = _MakeRange(startOff, endOff);
    if (!loc || !range) {
        CeLspJson_Free(loc);
        CeLspJson_Free(range);
        return NULL;
    }
    CeLspJson_Set(loc, "uri", CeLspJson_NewString(g_docUri ? g_docUri : "file:///untitled.cea"));
    CeLspJson_Set(loc, "range", range);
    return loc;
}

static int _LookupSymbol(const char* name, size_t len)
{
    for (int i = 0; i < g_symbolCount; ++i) {
        if ((strlen(g_symbols[i].name) == len) && (strncmp(g_symbols[i].name, name, len) == 0)) {
            return i;
        }
    }
    return -1;
}

// Word under the cursor using the shared CeLspPos helpers (same behaviour as
// hover).  Returns the copied length (0 when there is no word).
static size_t _CursorWord(const CELSP_JSON* params, char* name, size_t cap,
                          size_t* pStart, size_t* pEnd)
{
    name[0] = '\0';
    if (pStart) { *pStart = 0u; }
    if (pEnd) { *pEnd = 0u; }
    if (!g_docText) {
        return 0u;
    }
    const CELSP_JSON* const pos = CeLspJson_Get(params, "position");
    const size_t offset = CeLspPos_ToOffset(g_docText, g_docLen,
                                            CeLspJson_GetInt(pos, "line"),
                                            CeLspJson_GetInt(pos, "character"), g_utf16);
    const size_t ws = CeLspPos_WordStart(g_docText, g_docLen, offset);
    const size_t we = CeLspPos_WordEnd(g_docText, g_docLen, ws);
    if (we <= ws) {
        return 0u;
    }
    size_t n = we - ws;
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(name, g_docText + ws, n);
    name[n] = '\0';
    if (pStart) { *pStart = ws; }
    if (pEnd) { *pEnd = we; }
    return n;
}

// Whole-word walk over the document, skipping line comments (`//`, `--`,
// AA line-leading `;`) and string literals.  cb sees every identifier run.
typedef void (*CE_WORD_CB)(const char* word, size_t off, size_t len, void* ctx);

static void _ForEachWordRaw(CE_WORD_CB cb, void* ctx)
{
    if (!g_docText || !cb) {
        return;
    }
    const char* const t = g_docText;
    size_t i = 0u;
    bool atLineStart = true;
    while (i < g_docLen) {
        const char c = t[i];
        if (c == '\n') {
            atLineStart = true;
            ++i;
            continue;
        }
        if ((c == ' ') || (c == '\t') || (c == '\r')) {
            ++i;
            continue;
        }
        if ((c == ';') && atLineStart) {
            while ((i < g_docLen) && (t[i] != '\n')) { ++i; }
            continue;
        }
        atLineStart = false;
        if (((c == '/') && (i + 1u < g_docLen) && (t[i + 1u] == '/')) ||
            ((c == '-') && (i + 1u < g_docLen) && (t[i + 1u] == '-'))) {
            while ((i < g_docLen) && (t[i] != '\n')) { ++i; }
            continue;
        }
        if ((c == '"') || (c == '\'')) {
            ++i;
            while ((i < g_docLen) && (t[i] != c) && (t[i] != '\n')) { ++i; }
            if ((i < g_docLen) && (t[i] == c)) { ++i; }
            continue;
        }
        if (_IsWordStart((unsigned char)c)) {
            const size_t s = i;
            while ((i < g_docLen) && _IsWordBody((unsigned char)t[i])) { ++i; }
            cb(t + s, s, i - s, ctx);
            continue;
        }
        ++i;
    }
}

typedef struct {
    const char* name;
    size_t len;
    void (*cb)(size_t off, size_t len, void* ctx);
    void* ctx;
} CE_WORD_MATCH;

static void _WordMatchCb(const char* word, size_t off, size_t len, void* vctx)
{
    CE_WORD_MATCH* const m = (CE_WORD_MATCH*)vctx;
    if ((len == m->len) && (memcmp(word, m->name, len) == 0)) {
        m->cb(off, len, m->ctx);
    }
}

static void _ForEachWord(const char* name, size_t nameLen,
                         void (*cb)(size_t off, size_t len, void* ctx), void* ctx)
{
    CE_WORD_MATCH m;
    m.name = name;
    m.len = nameLen;
    m.cb = cb;
    m.ctx = ctx;
    _ForEachWordRaw(_WordMatchCb, &m);
}

// ---- definition / declaration ----------------------------------------------

static void _HandleDefinition(int id, const CELSP_JSON* params)
{
    _ScanSymbols();
    CELSP_JSON* const result = CeLspJson_NewArray();
    char name[128];
    size_t ws = 0u, we = 0u;
    const size_t n = _CursorWord(params, name, sizeof(name), &ws, &we);
    if ((n > 0u) && result) {
        const int idx = _LookupSymbol(name, n);
        if (idx >= 0) {
            const size_t off = (size_t)g_symbols[idx].offset;
            const size_t len = (size_t)g_symbols[idx].nameLen;
            CELSP_JSON* const loc = _MakeLocation(off, off + len);
            if (loc && !CeLspJson_Add(result, loc)) {
                CeLspJson_Free(loc);
            }
        }
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ---- references -------------------------------------------------------------

typedef struct {
    CELSP_JSON* arr;
    int count;
    bool includeDecl;
    size_t declOff;
    size_t declLen;
} CE_REF_CTX;

static void _RefCb(size_t off, size_t len, void* vctx)
{
    CE_REF_CTX* const c = (CE_REF_CTX*)vctx;
    if (c->count >= CELSP_MAX_REFS) {
        return;
    }
    if ((c->declLen == len) && (c->declOff == off) && !c->includeDecl) {
        return;
    }
    CELSP_JSON* const loc = _MakeLocation(off, off + len);
    if (loc && CeLspJson_Add(c->arr, loc)) {
        ++c->count;
    }
    else {
        CeLspJson_Free(loc);
    }
}

static void _HandleReferences(int id, const CELSP_JSON* params)
{
    _ScanSymbols();
    char name[128];
    size_t ws = 0u, we = 0u;
    const size_t n = _CursorWord(params, name, sizeof(name), &ws, &we);
    CELSP_JSON* const result = CeLspJson_NewArray();
    if ((n > 0u) && result) {
        const CELSP_JSON* const ctxNode = CeLspJson_Get(params, "context");
        const CELSP_JSON* const incNode = ctxNode ? CeLspJson_Get(ctxNode, "includeDeclaration") : NULL;
        CE_REF_CTX c;
        c.arr = result;
        c.count = 0;
        c.includeDecl = incNode ? CeLspJson_Bool(incNode) : true;
        c.declOff = 0u;
        c.declLen = 0u;
        const int idx = _LookupSymbol(name, n);
        if (idx >= 0) {
            c.declOff = (size_t)g_symbols[idx].offset;
            c.declLen = (size_t)g_symbols[idx].nameLen;
        }
        _ForEachWord(name, n, _RefCb, &c);
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ---- documentSymbol (flat, no children) -------------------------------------

static void _HandleDocumentSymbol(int id, const CELSP_JSON* params)
{
    (void)params;
    _ScanSymbols();
    CELSP_JSON* const result = CeLspJson_NewArray();
    if (result) {
        for (int i = 0; i < g_symbolCount; ++i) {
            const size_t off = (size_t)g_symbols[i].offset;
            const size_t len = (size_t)g_symbols[i].nameLen;
            size_t ls = 0u, le = 0u;
            _LineBounds(off, &ls, &le);
            CELSP_JSON* const item = CeLspJson_NewObject();
            CELSP_JSON* const range = _MakeRange(ls, le);
            CELSP_JSON* const sel = _MakeRange(off, off + len);
            if (!item || !range || !sel) {
                CeLspJson_Free(item);
                CeLspJson_Free(range);
                CeLspJson_Free(sel);
                break;
            }
            CeLspJson_Set(item, "name", CeLspJson_NewString(g_symbols[i].name));
            CeLspJson_Set(item, "kind", CeLspJson_NewNumber((double)g_symbols[i].symKind));
            CeLspJson_Set(item, "detail", CeLspJson_NewString(g_symbols[i].detail));
            CeLspJson_Set(item, "range", range);
            CeLspJson_Set(item, "selectionRange", sel);
            if (!CeLspJson_Add(result, item)) {
                CeLspJson_Free(item);
                break;
            }
        }
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ---- workspace/symbol --------------------------------------------------------

static int _CeKindToSymKind(int kind)
{
    switch (kind) {
    case CE_KIND_FUNCTION: return 12;
    case CE_KIND_VARIABLE: return 13;
    case CE_KIND_CONSTANT: return 14;
    case CE_KIND_TYPE:     return 5;
    default:               return 20;
    }
}

static void _HandleWorkspaceSymbol(int id, const CELSP_JSON* params)
{
    const char* query = CeLspJson_GetString(params, "query");
    if (!query) {
        query = "";
    }
    _ScanSymbols();
    CELSP_JSON* const result = CeLspJson_NewArray();
    int total = 0;
    if (result) {
        for (int i = 0; (i < g_symbolCount) && (total < CELSP_MAX_WS_TOTAL); ++i) {
            if (!_ContainsCI(g_symbols[i].name, query)) {
                continue;
            }
            const size_t off = (size_t)g_symbols[i].offset;
            const size_t len = (size_t)g_symbols[i].nameLen;
            CELSP_JSON* const item = CeLspJson_NewObject();
            CELSP_JSON* const loc = _MakeLocation(off, off + len);
            if (!item || !loc) {
                CeLspJson_Free(item);
                CeLspJson_Free(loc);
                break;
            }
            CeLspJson_Set(item, "name", CeLspJson_NewString(g_symbols[i].name));
            CeLspJson_Set(item, "kind", CeLspJson_NewNumber((double)g_symbols[i].symKind));
            CeLspJson_Set(item, "location", loc);
            CeLspJson_Set(item, "containerName", CeLspJson_NewString("document"));
            if (!CeLspJson_Add(result, item)) {
                CeLspJson_Free(item);
                break;
            }
            ++total;
        }
        int catHits = 0;
        for (int i = 0; (i < g_ceCompletionCount) && (catHits < CELSP_MAX_WS_CAT) &&
                        (total < CELSP_MAX_WS_TOTAL); ++i) {
            const CE_COMPLETION_ENTRY* const e = &g_ceCompletions[i];
            if (!_ContainsCI(e->name, query)) {
                continue;
            }
            ++catHits;
            CELSP_JSON* const item = CeLspJson_NewObject();
            CELSP_JSON* const loc = CeLspJson_NewObject();
            CELSP_JSON* const range = _MakeRange(0u, 0u);
            if (!item || !loc || !range) {
                CeLspJson_Free(item);
                CeLspJson_Free(loc);
                CeLspJson_Free(range);
                break;
            }
            CeLspJson_Set(loc, "uri", CeLspJson_NewString(g_docUri ? g_docUri : "file:///untitled.cea"));
            CeLspJson_Set(loc, "range", range);
            CeLspJson_Set(item, "name", CeLspJson_NewString(e->name));
            CeLspJson_Set(item, "kind", CeLspJson_NewNumber((double)_CeKindToSymKind(e->kind)));
            CeLspJson_Set(item, "location", loc);
            CeLspJson_Set(item, "containerName", CeLspJson_NewString("CE API"));
            if (!CeLspJson_Add(result, item)) {
                CeLspJson_Free(item);
                break;
            }
            ++total;
        }
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ---- formatting ---------------------------------------------------------------

static size_t _CopyInto(char* dst, size_t cap, size_t pos, const char* src, size_t n)
{
    if (pos + n + 1u > cap) {
        n = (pos + 1u < cap) ? (cap - pos - 1u) : 0u;
    }
    if (n > 0u) {
        memcpy(dst + pos, src, n);
    }
    return pos + n;
}

static void _HandleFormatting(int id, const CELSP_JSON* params)
{
    const CELSP_JSON* const opts = CeLspJson_Get(params, "options");
    if (opts) {
        const int tabSize = CeLspJson_GetInt(opts, "tabSize");
        (void)tabSize; // the CE house style uses fixed two-space indents
    }
    CELSP_JSON* const result = CeLspJson_NewArray();
    if (result && g_docText) {
        bool inLua = false;
        bool otherRegion = false;
        int depth = 0;
        int count = 0;
        const char* p = g_docText;
        size_t lineOff = 0u;
        while (lineOff <= g_docLen) {
            const char* const eol = (const char*)memchr(p, '\n', g_docLen - lineOff);
            const size_t len = eol ? (size_t)(eol - p) : (g_docLen - lineOff);
            size_t blen = len;
            const bool hadCR = (blen > 0u) && (p[blen - 1u] == '\r');
            if (hadCR) {
                --blen;
            }
            size_t core = blen;
            while ((core > 0u) && ((p[core - 1u] == ' ') || (p[core - 1u] == '\t'))) {
                --core;
            }
            size_t lead = 0u;
            while ((lead < core) && ((p[lead] == ' ') || (p[lead] == '\t'))) {
                ++lead;
            }
            const char* const body = p + lead;
            const size_t bLen = core - lead;
            const bool marker = (bLen > 0u) &&
                (_MemContains(body, bLen, "{$lua}") || _MemContains(body, bLen, "{$asm}") ||
                 _MemContains(body, bLen, "{$ccode}") || _MemContains(body, bLen, "{$CODE}"));
            const bool comment = (bLen > 0u) && ((body[0] == ';') ||
                ((bLen >= 2u) && (((body[0] == '/') && (body[1] == '/')) ||
                                  ((body[0] == '-') && (body[1] == '-')))));
            char fmt[4096];
            size_t fi = 0u;
            if (bLen == 0u) {
                fi = 0u; // blank line: drop trailing whitespace
            }
            else if (marker || comment || (body[0] == '[') || otherRegion) {
                fi = _CopyInto(fmt, sizeof(fmt), 0u, body, bLen);
            }
            else if (inLua) {
                bool dedent = false;
                if ((bLen >= 3u) && (strncmp(body, "end", 3u) == 0) &&
                    ((bLen == 3u) || !_IsWordBody((unsigned char)body[3]))) {
                    dedent = true;
                }
                else if ((bLen >= 5u) && (strncmp(body, "until", 5u) == 0) &&
                         ((bLen == 5u) || !_IsWordBody((unsigned char)body[5]))) {
                    dedent = true;
                }
                int d = depth - (dedent ? 1 : 0);
                if (d < 0) {
                    d = 0;
                }
                size_t ind = (size_t)(d * 2);
                if ((ind + bLen + 1u) > sizeof(fmt)) {
                    ind = 0u; // line too long to re-indent safely
                }
                for (size_t k = 0u; k < ind; ++k) {
                    fmt[fi++] = ' ';
                }
                fi = _CopyInto(fmt, sizeof(fmt), fi, body, bLen);
                // depth bookkeeping on a comment-stripped copy of the line
                char strip[4096];
                size_t si = 0u;
                for (size_t k = 0u; (k < bLen) && ((si + 1u) < sizeof(strip)); ++k) {
                    const char ch = body[k];
                    if (((ch == '/') || (ch == '-')) && ((k + 1u) < bLen) && (body[k + 1u] == ch)) {
                        break;
                    }
                    strip[si++] = ch;
                }
                strip[si] = '\0';
                depth += _CountKeyword(strip, "function") + _CountKeyword(strip, "do");
                depth += _CountKeyword(strip, "then") + _CountKeyword(strip, "repeat");
                depth -= _CountKeyword(strip, "end") + _CountKeyword(strip, "until");
                if (depth < 0) {
                    depth = 0;
                }
            }
            else {
                // AA region: labels and markers flush left, instructions 2 spaces.
                bool indent2 = false;
                size_t w = 0u;
                while ((w < bLen) && _IsWordBody((unsigned char)body[w])) {
                    ++w;
                }
                if (w > 0u) {
                    char word[64];
                    const size_t wl = _CopyInto(word, sizeof(word), 0u, body, w);
                    word[wl] = '\0';
                    size_t k = w;
                    while ((k < bLen) && ((body[k] == ' ') || (body[k] == '\t'))) {
                        ++k;
                    }
                    const bool labelLine = (k < bLen) && (body[k] == ':');
                    if (!labelLine &&
                        (_InTableI(g_asmMnemonics, word, wl) || _InTableI(g_aaDirectives, word, wl))) {
                        indent2 = true;
                    }
                }
                if (indent2) {
                    fmt[fi++] = ' ';
                    fmt[fi++] = ' ';
                }
                fi = _CopyInto(fmt, sizeof(fmt), fi, body, bLen);
            }
            fmt[fi] = '\0';
            const bool truncated = (bLen > 0u) && ((bLen + 3u) >= sizeof(fmt));
            if (!truncated && ((fi != blen) || ((fi > 0u) && (memcmp(fmt, p, fi) != 0))) &&
                (count < CELSP_MAX_EDITS)) {
                char newText[4100];
                size_t ni = _CopyInto(newText, sizeof(newText), 0u, fmt, fi);
                if (hadCR && (ni + 1u < sizeof(newText))) {
                    newText[ni++] = '\r';
                }
                newText[ni] = '\0';
                CELSP_JSON* const edit = CeLspJson_NewObject();
                CELSP_JSON* const range = _MakeRange(lineOff, lineOff + len);
                if (!edit || !range) {
                    CeLspJson_Free(edit);
                    CeLspJson_Free(range);
                }
                else {
                    CeLspJson_Set(edit, "range", range);
                    CeLspJson_Set(edit, "newText", CeLspJson_NewString(newText));
                    if (CeLspJson_Add(result, edit)) {
                        ++count;
                    }
                    else {
                        CeLspJson_Free(edit);
                    }
                }
            }
            if (marker) {
                if (_MemContains(body, bLen, "{$lua}")) {
                    inLua = !inLua;
                    otherRegion = false;
                    depth = 0;
                }
                else {
                    otherRegion = true;
                }
            }
            if (!eol) {
                break;
            }
            p = eol + 1u;
            lineOff = (size_t)(p - g_docText);
        }
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ---- semanticTokens/full -------------------------------------------------------

typedef struct { int line; int col; int len; int type; int mods; } CE_TOKEN;

static CE_TOKEN g_tokens[CELSP_MAX_TOKENS];
static int      g_tokenCount = 0;

static int _ColInLine(size_t lineStart, size_t off)
{
    int col = 0;
    if (!g_docText) {
        return 0;
    }
    for (size_t i = lineStart; (i < off) && (i < g_docLen); ) {
        const unsigned char c = (unsigned char)g_docText[i];
        if (c < 0x80u) { ++col; i += 1u; }
        else if (c < 0xE0u) { ++col; i += 2u; }
        else if (c < 0xF0u) { ++col; i += 3u; }
        else { col += g_utf16 ? 2 : 1; i += 4u; }
    }
    return col;
}

static int _DeclMods(size_t off, size_t len)
{
    for (int i = 0; i < g_symbolCount; ++i) {
        if (((size_t)g_symbols[i].offset == off) && ((size_t)g_symbols[i].nameLen == len)) {
            return 1 << CE_MOD_DECLARATION;
        }
    }
    return 0;
}

static void _EmitToken(int line, size_t lineOff, size_t start, size_t end, int type, int mods)
{
    if ((g_tokenCount >= CELSP_MAX_TOKENS) || (end <= start)) {
        return;
    }
    CE_TOKEN* const t = &g_tokens[g_tokenCount++];
    t->line = line;
    t->col = _ColInLine(lineOff, start);
    t->len = (int)(end - start);
    t->type = type;
    t->mods = mods;
}

// Catalogue kind (see g_ceKindLabels) or 0 when the name is unknown.
static int _EntryKindN(const char* w, size_t len)
{
    char name[128];
    if ((len == 0u) || (len >= sizeof(name))) {
        return 0;
    }
    memcpy(name, w, len);
    name[len] = '\0';
    const CE_COMPLETION_ENTRY* const e = _FindEntry(name);
    return e ? e->kind : 0;
}

static bool _SymbolKindN(const char* w, size_t len, int* pSymKind)
{
    char name[128];
    if ((len == 0u) || (len >= sizeof(name))) {
        return false;
    }
    memcpy(name, w, len);
    name[len] = '\0';
    const int idx = _LookupSymbol(name, len);
    if (idx < 0) {
        return false;
    }
    *pSymKind = g_symbols[idx].symKind;
    return true;
}

static void _TokenizeLine(const char* line, size_t len, int lineNo, size_t lineOff, int region)
{
    size_t i = 0u;
    while (i < len) {
        const char c = line[i];
        if ((c == ' ') || (c == '\t') || (c == '\r')) {
            ++i;
            continue;
        }
        if (((c == '/') && (i + 1u < len) && (line[i + 1u] == '/')) ||
            ((c == '-') && (i + 1u < len) && (line[i + 1u] == '-')) ||
            ((c == ';') && (region != 1))) {
            _EmitToken(lineNo, lineOff, lineOff + i, lineOff + len, CE_TOK_COMMENT, 0);
            break;
        }
        if ((c == '"') || (c == '\'')) {
            const size_t s = i;
            ++i;
            while ((i < len) && (line[i] != c) && (line[i] != '\n')) { ++i; }
            if ((i < len) && (line[i] == c)) { ++i; }
            _EmitToken(lineNo, lineOff, lineOff + s, lineOff + i, CE_TOK_STRING, 0);
            continue;
        }
        if ((c == '{') && (i + 1u < len) && (line[i + 1u] == '$')) {
            const size_t s = i;
            while ((i < len) && (line[i] != '}')) { ++i; }
            if (i < len) { ++i; }
            _EmitToken(lineNo, lineOff, lineOff + s, lineOff + i, CE_TOK_MACRO, 0);
            continue;
        }
        if ((c >= '0') && (c <= '9')) {
            const size_t s = i;
            while ((i < len) && _IsWordBody((unsigned char)line[i])) { ++i; }
            _EmitToken(lineNo, lineOff, lineOff + s, lineOff + i, CE_TOK_NUMBER, 0);
            continue;
        }
        if (_IsWordStart((unsigned char)c)) {
            const size_t s = i;
            while ((i < len) && _IsWordBody((unsigned char)line[i])) { ++i; }
            const size_t wl = i - s;
            size_t j = i;
            while ((j < len) && ((line[j] == ' ') || (line[j] == '\t'))) { ++j; }
            // AA/ASM label definition `name:` (also Lua `::name::`)
            if ((j < len) && (line[j] == ':')) {
                size_t k = j + 1u;
                while ((k < len) && ((line[k] == ' ') || (line[k] == '\t'))) { ++k; }
                const bool endsLine = (k >= len) || (line[k] == ';') || (line[k] == ':') ||
                    ((line[k] == '/') && (k + 1u < len) && (line[k + 1u] == '/')) ||
                    ((line[k] == '-') && (k + 1u < len) && (line[k + 1u] == '-'));
                if (endsLine) {
                    _EmitToken(lineNo, lineOff, lineOff + s, lineOff + i,
                               CE_TOK_LABEL, 1 << CE_MOD_DECLARATION);
                    i = j + 1u;
                    if ((i < len) && (line[i] == ':')) { ++i; }
                    continue;
                }
            }
            int type = -1;
            int mods = 0;
            if (region == 1) {
                if (_InTableI(g_luaKeywords, line + s, wl)) {
                    type = CE_TOK_KEYWORD;
                }
                else {
                    const int kind = _EntryKindN(line + s, wl);
                    if (kind == CE_KIND_FUNCTION) {
                        // Keep the lexer's CE API colour instead of the
                        // user-function purple after semantic tokens load.
                        type = CE_TOK_API;
                    }
                    else if (kind != 0) {
                        type = CE_TOK_FUNCTION;
                    }
                    else {
                        int sk = 0;
                        if (_SymbolKindN(line + s, wl, &sk)) {
                            type = (sk == 12) ? CE_TOK_FUNCTION : CE_TOK_VARIABLE;
                            mods = _DeclMods(lineOff + s, wl);
                        }
                        else {
                            char name[128];
                            if ((wl > 0u) && (wl < sizeof(name))) {
                                memcpy(name, line + s, wl);
                                name[wl] = '\0';
                                if (CeApi_FindType(name)) {
                                    type = CE_TOK_TYPE;
                                }
                            }
                        }
                    }
                }
            }
            else {
                if (_InTableI(g_aaDirectives, line + s, wl)) {
                    type = CE_TOK_MACRO;
                }
                else if (_InTableI(g_asmMnemonics, line + s, wl)) {
                    type = CE_TOK_INSTRUCTION;
                }
                else if (_InTableI(g_asmRegisters, line + s, wl)) {
                    type = CE_TOK_REGISTER;
                    mods = 1 << CE_MOD_READONLY;
                }
                else {
                    const int kind = _EntryKindN(line + s, wl);
                    if (kind == CE_KIND_FUNCTION) {
                        type = CE_TOK_API;
                    }
                    else if (kind != 0) {
                        type = CE_TOK_FUNCTION;
                    }
                    else {
                        int sk = 0;
                        if (_SymbolKindN(line + s, wl, &sk)) {
                            type = (sk == 12) ? CE_TOK_FUNCTION : CE_TOK_VARIABLE;
                            mods = _DeclMods(lineOff + s, wl);
                        }
                        else {
                            char name[128];
                            if ((wl > 0u) && (wl < sizeof(name))) {
                                memcpy(name, line + s, wl);
                                name[wl] = '\0';
                                if (CeApi_FindType(name)) {
                                    type = CE_TOK_TYPE;
                                }
                            }
                        }
                    }
                }
            }
            if (type >= 0) {
                _EmitToken(lineNo, lineOff, lineOff + s, lineOff + i, type, mods);
            }
            continue;
        }
        ++i;
    }
}

static void _HandleSemanticTokensFull(int id, const CELSP_JSON* params)
{
    (void)params;
    CELSP_JSON* data = NULL;
    if ((g_tokenCacheVersion == g_docVersion) && g_tokenCacheData) {
        data = CeLspJson_Parse(g_tokenCacheData, 0u);
    }
    if (!data) {
        g_tokenCacheVersion = -1;
        free(g_tokenCacheData); g_tokenCacheData = NULL;
    }
    if (!data) {
    _ScanSymbols();
    g_tokenCount = 0;
    if (g_docText) {
        int region = 0; // 0 AA, 1 Lua, 2 asm, 3 ccode
        const char* p = g_docText;
        size_t lineOff = 0u;
        int lineNo = 0;
        while (lineOff <= g_docLen) {
            const char* const eol = (const char*)memchr(p, '\n', g_docLen - lineOff);
            const size_t len = eol ? (size_t)(eol - p) : (g_docLen - lineOff);
            _TokenizeLine(p, len, lineNo, lineOff, region);
            if (_MemContains(p, len, "{$lua}")) {
                region = 1;
            }
            else if (_MemContains(p, len, "{$asm}")) {
                region = 2;
            }
            else if (_MemContains(p, len, "{$ccode}") || _MemContains(p, len, "{$CODE}")) {
                region = 3;
            }
            if (!eol) {
                break;
            }
            p = eol + 1u;
            lineOff = (size_t)(p - g_docText);
            ++lineNo;
        }
    }
    data = CeLspJson_NewArray();
    int prevLine = 0, prevCol = 0;
    if (data) {
        for (int i = 0; i < g_tokenCount; ++i) {
            const CE_TOKEN* const t = &g_tokens[i];
            const int deltaLine = t->line - prevLine;
            const int deltaCol = (deltaLine == 0) ? (t->col - prevCol) : t->col;
            CeLspJson_Add(data, CeLspJson_NewNumber((double)deltaLine));
            CeLspJson_Add(data, CeLspJson_NewNumber((double)deltaCol));
            CeLspJson_Add(data, CeLspJson_NewNumber((double)t->len));
            CeLspJson_Add(data, CeLspJson_NewNumber((double)t->type));
            CeLspJson_Add(data, CeLspJson_NewNumber((double)t->mods));
            prevLine = t->line;
            prevCol = t->col;
        }
    }
        if (data) {
            char* const cached = CeLspJson_Write(data);
            if (cached) {
                free(g_tokenCacheData);
                g_tokenCacheData = cached;
                g_tokenCacheVersion = g_docVersion;
            }
        }
    }
    CELSP_JSON* const result = CeLspJson_NewObject();
    if (result) {
        CeLspJson_Set(result, "data", data);
    }
    else {
        CeLspJson_Free(data);
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ---- inlayHint ------------------------------------------------------------------

static void _AddHint(CELSP_JSON* arr, size_t off, const char* label, int* count)
{
    if (!arr || !count || (*count >= CELSP_MAX_HINTS)) {
        return;
    }
    CELSP_JSON* const item = CeLspJson_NewObject();
    CELSP_JSON* const position = _MakePosition(off);
    if (!item || !position) {
        CeLspJson_Free(item);
        CeLspJson_Free(position);
        return;
    }
    CeLspJson_Set(item, "position", position);
    CeLspJson_Set(item, "label", CeLspJson_NewString(label));
    CeLspJson_Set(item, "kind", CeLspJson_NewNumber(1.0)); // TypeHint
    CeLspJson_Set(item, "paddingLeft", CeLspJson_NewBool(true));
    if (CeLspJson_Add(arr, item)) {
        ++*count;
    }
    else {
        CeLspJson_Free(item);
    }
}

// Extract up to three parameter names from a catalogue signature.
static int _ParseSigParams(const char* sig, char out[3][48])
{
    if (!sig) {
        return 0;
    }
    const char* open = strchr(sig, '(');
    if (!open) {
        return 0;
    }
    const char* p = open + 1;
    int n = 0;
    while ((n < 3) && (*p != '\0') && (*p != ')')) {
        while ((*p == ' ') || (*p == ',') || (*p == '[') || (*p == ']')) { ++p; }
        if ((*p == '\0') || (*p == ')')) {
            break;
        }
        if (_IsWordStart((unsigned char)*p)) {
            size_t k = 0u;
            while ((*p != '\0') && _IsWordBody((unsigned char)*p) && ((k + 1u) < sizeof(out[0]))) {
                out[n][k++] = *p++;
            }
            out[n][k] = '\0';
            if (k > 0u) {
                ++n;
            }
        }
        while ((*p != '\0') && (*p != ',') && (*p != ')')) { ++p; }
    }
    return n;
}

// `local x = <inferable>` -> ": <type>" after the name.
static void _LocalTypeHints(CELSP_JSON* arr, int* count)
{
    const char* p = g_docText;
    size_t lineOff = 0u;
    while (p && (lineOff <= g_docLen)) {
        const char* const eol = (const char*)memchr(p, '\n', g_docLen - lineOff);
        const size_t len = eol ? (size_t)(eol - p) : (g_docLen - lineOff);
        size_t i = 0u;
        while ((i < len) && ((p[i] == ' ') || (p[i] == '\t'))) { ++i; }
        if (((i + 5u) <= len) && (strncmp(p + i, "local", 5u) == 0) &&
            ((i + 5u >= len) || !_IsWordBody((unsigned char)p[i + 5u]))) {
            i += 5u;
            while ((i < len) && ((p[i] == ' ') || (p[i] == '\t'))) { ++i; }
            if ((i < len) && _IsWordStart((unsigned char)p[i])) {
                const size_t ns = i;
                while ((i < len) && _IsWordBody((unsigned char)p[i])) { ++i; }
                size_t j = i;
                while ((j < len) && ((p[j] == ' ') || (p[j] == '\t'))) { ++j; }
                if ((j < len) && (p[j] == '=') && ((j + 1u >= len) || (p[j + 1u] != '='))) {
                    ++j;
                    while ((j < len) && ((p[j] == ' ') || (p[j] == '\t'))) { ++j; }
                    if (j < len) {
                        char rhs[512];
                        const size_t rl = _CopyInto(rhs, sizeof(rhs), 0u, p + j, len - j);
                        rhs[rl] = '\0';
                        char type[40];
                        _InferType(rhs, type, sizeof(type));
                        if (type[0] != '\0') {
                            char label[64];
                            snprintf(label, sizeof(label), ": %s", type);
                            _AddHint(arr, lineOff + i, label, count);
                        }
                    }
                }
            }
        }
        if (!eol) {
            break;
        }
        p = eol + 1u;
        lineOff = (size_t)(p - g_docText);
    }
}

typedef struct { CELSP_JSON* arr; int count; } CE_HINT_CTX;

static void _ParamHintCb(const char* word, size_t off, size_t len, void* vctx)
{
    CE_HINT_CTX* const c = (CE_HINT_CTX*)vctx;
    if ((c->count >= CELSP_MAX_HINTS) || !g_docText) {
        return;
    }
    if ((off > 0u) && (g_docText[off - 1u] == '.')) {
        return; // member call, the bare catalogue key does not apply
    }
    char name[128];
    if ((len == 0u) || (len >= sizeof(name))) {
        return;
    }
    memcpy(name, word, len);
    name[len] = '\0';
    const CE_COMPLETION_ENTRY* const e = _FindEntry(name);
    if (!e || !e->signature) {
        return;
    }
    char params[3][48];
    const int np = _ParseSigParams(e->signature, params);
    if (np <= 0) {
        return;
    }
    size_t j = off + len;
    while ((j < g_docLen) && ((g_docText[j] == ' ') || (g_docText[j] == '\t'))) { ++j; }
    if ((j >= g_docLen) || (g_docText[j] != '(')) {
        return;
    }
    ++j;
    for (int k = 0; k < np; ++k) {
        while ((j < g_docLen) && ((g_docText[j] == ' ') || (g_docText[j] == '\t') ||
                                  (g_docText[j] == '\r') || (g_docText[j] == '\n'))) { ++j; }
        if ((j >= g_docLen) || (g_docText[j] == ')') || (g_docText[j] == ',')) {
            break; // empty or missing argument
        }
        char label[64];
        snprintf(label, sizeof(label), "%s:", params[k]);
        _AddHint(c->arr, j, label, &c->count);
        int dep = 0;
        while (j < g_docLen) {
            const char ch = g_docText[j];
            if (ch == '(') {
                ++dep;
            }
            else if (ch == ')') {
                if (dep == 0) { ++j; break; }
                --dep;
            }
            else if ((ch == '"') || (ch == '\'')) {
                ++j;
                while ((j < g_docLen) && (g_docText[j] != ch) && (g_docText[j] != '\n')) { ++j; }
                if ((j < g_docLen) && (g_docText[j] == ch)) { ++j; }
                continue;
            }
            else if ((ch == ',') && (dep == 0)) {
                ++j;
                break;
            }
            ++j;
        }
    }
}

static void _HandleInlayHint(int id, const CELSP_JSON* params)
{
    (void)params;
    _ScanSymbols();
    CELSP_JSON* const result = CeLspJson_NewArray();
    int count = 0;
    if (result && g_docText) {
        _LocalTypeHints(result, &count);
        CE_HINT_CTX ctx;
        ctx.arr = result;
        ctx.count = count;
        _ForEachWordRaw(_ParamHintCb, &ctx);
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ---- codeLens ---------------------------------------------------------------------

static void _CountCb(size_t off, size_t len, void* ctx)
{
    (void)off;
    (void)len;
    ++(*(int*)ctx);
}

static void _HandleCodeLens(int id, const CELSP_JSON* params)
{
    (void)params;
    _ScanSymbols();
    CELSP_JSON* const result = CeLspJson_NewArray();
    if (result) {
        for (int i = 0; (i < g_symbolCount) && (i < CELSP_MAX_LENS); ++i) {
            const size_t off = (size_t)g_symbols[i].offset;
            const size_t len = (size_t)g_symbols[i].nameLen;
            int n = 0;
            _ForEachWord(g_symbols[i].name, len, _CountCb, &n);
            char title[32];
            snprintf(title, sizeof(title), "%d references", n);
            CELSP_JSON* const lens = CeLspJson_NewObject();
            CELSP_JSON* const range = _MakeRange(off, off + len);
            CELSP_JSON* const command = CeLspJson_NewObject();
            CELSP_JSON* const args = CeLspJson_NewArray();
            CELSP_JSON* const pos = _MakePosition(off);
            if (!lens || !range || !command || !args || !pos) {
                CeLspJson_Free(lens); CeLspJson_Free(range); CeLspJson_Free(command);
                CeLspJson_Free(args); CeLspJson_Free(pos);
                break;
            }
            CeLspJson_Add(args, CeLspJson_NewString(g_docUri ? g_docUri : "file:///untitled.cea"));
            CeLspJson_Add(args, pos);
            CeLspJson_Set(command, "title", CeLspJson_NewString(title));
            CeLspJson_Set(command, "command", CeLspJson_NewString("np3.ce.showReferences"));
            CeLspJson_Set(command, "arguments", args);
            CeLspJson_Set(lens, "range", range);
            CeLspJson_Set(lens, "command", command);
            if (!CeLspJson_Add(result, lens)) {
                CeLspJson_Free(lens);
                break;
            }
        }
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ---- documentLink -------------------------------------------------------------------

typedef struct { CELSP_JSON* arr; int count; const char* dir; size_t dirLen; } CE_LINK_CTX;

// dir + relative path -> file:/// URI (hand-built, backslashes to slashes).
static char* _LinkTarget(const char* dir, size_t dirLen, const char* name, size_t nlen,
                         const char* suffix)
{
    bool hasSep = false;
    for (size_t i = 0u; i < nlen; ++i) {
        if ((name[i] == '/') || (name[i] == '\\')) {
            hasSep = true;
            break;
        }
    }
    const size_t cap = dirLen + nlen + strlen(suffix) + 2u;
    char* const out = (char*)malloc(cap);
    if (!out) {
        return NULL;
    }
    size_t k = 0u;
    memcpy(out, dir, dirLen);
    k = dirLen;
    for (size_t i = 0u; i < nlen; ++i) {
        char ch = name[i];
        if (ch == '\\') {
            ch = '/';
        }
        else if (!hasSep && (ch == '.')) {
            ch = '/'; // module dots become path separators
        }
        out[k++] = ch;
    }
    if (!hasSep) {
        const size_t sl = strlen(suffix);
        memcpy(out + k, suffix, sl);
        k += sl;
    }
    out[k] = '\0';
    return out;
}

static void _LinkCb(const char* word, size_t off, size_t len, void* vctx)
{
    CE_LINK_CTX* const c = (CE_LINK_CTX*)vctx;
    if ((c->count >= CELSP_MAX_LINKS) || !g_docText) {
        return;
    }
    const bool isRequire = _StrIEq(word, len, "require");
    const bool isDofile = _StrIEq(word, len, "dofile");
    const bool isAsmFile = _StrIEq(word, len, "includeasmfile");
    const bool isDll = _StrIEq(word, len, "injectdll");
    if (!isRequire && !isDofile && !isAsmFile && !isDll) {
        return;
    }
    const bool aaForm = isAsmFile || isDll;
    size_t j = off + len;
    while ((j < g_docLen) && ((g_docText[j] == ' ') || (g_docText[j] == '\t'))) { ++j; }
    size_t nameStart = 0u, nameEnd = 0u, linkStart = 0u, linkEnd = 0u;
    if (aaForm) {
        if ((j >= g_docLen) || (g_docText[j] != '(')) {
            return;
        }
        ++j;
        while ((j < g_docLen) && ((g_docText[j] == ' ') || (g_docText[j] == '\t'))) { ++j; }
        nameStart = j;
        while (j < g_docLen) {
            const char ch = g_docText[j];
            if (_IsWordBody((unsigned char)ch) || (ch == '.') || (ch == '/') || (ch == '\\')) {
                ++j;
            }
            else {
                break;
            }
        }
        nameEnd = j;
        if (nameEnd <= nameStart) {
            return;
        }
        linkStart = nameStart;
        linkEnd = nameEnd;
    }
    else {
        if ((j < g_docLen) && (g_docText[j] == '(')) {
            ++j;
            while ((j < g_docLen) && ((g_docText[j] == ' ') || (g_docText[j] == '\t'))) { ++j; }
        }
        if ((j >= g_docLen) || ((g_docText[j] != '"') && (g_docText[j] != '\''))) {
            return;
        }
        const char quote = g_docText[j];
        linkStart = j;
        ++j;
        nameStart = j;
        while ((j < g_docLen) && (g_docText[j] != quote) && (g_docText[j] != '\n')) { ++j; }
        if ((j >= g_docLen) || (g_docText[j] != quote)) {
            return;
        }
        nameEnd = j;
        linkEnd = j + 1u;
        if (nameEnd <= nameStart) {
            return;
        }
    }
    char* const target = _LinkTarget(c->dir, c->dirLen, g_docText + nameStart,
                                     nameEnd - nameStart, aaForm ? ".asm" : ".lua");
    if (!target) {
        return;
    }
    char tip[256];
    const size_t tl = _CopyInto(tip, sizeof(tip), 0u, g_docText + nameStart, nameEnd - nameStart);
    tip[tl] = '\0';
    CELSP_JSON* const item = CeLspJson_NewObject();
    CELSP_JSON* const range = _MakeRange(linkStart, linkEnd);
    if (!item || !range) {
        CeLspJson_Free(item);
        CeLspJson_Free(range);
        free(target);
        return;
    }
    CeLspJson_Set(item, "range", range);
    CeLspJson_Set(item, "target", CeLspJson_NewString(target));
    CeLspJson_Set(item, "tooltip", CeLspJson_NewString(tip));
    if (CeLspJson_Add(c->arr, item)) {
        ++c->count;
    }
    else {
        CeLspJson_Free(item);
    }
    free(target);
}

static void _HandleDocumentLink(int id, const CELSP_JSON* params)
{
    (void)params;
    CELSP_JSON* const result = CeLspJson_NewArray();
    if (result && g_docText) {
        const char* uri = g_docUri ? g_docUri : "file:///untitled.cea";
        size_t dirLen = 0u;
        for (size_t i = 0u; uri[i] != '\0'; ++i) {
            if (uri[i] == '/') {
                dirLen = i + 1u;
            }
        }
        CE_LINK_CTX ctx;
        ctx.arr = result;
        ctx.count = 0;
        ctx.dir = uri;
        ctx.dirLen = dirLen;
        _ForEachWordRaw(_LinkCb, &ctx);
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ---- documentHighlight ----------------------------------------------------------------

typedef struct { CELSP_JSON* arr; int count; } CE_HL_CTX;

static void _HlCb(size_t off, size_t len, void* vctx)
{
    CE_HL_CTX* const c = (CE_HL_CTX*)vctx;
    if (c->count >= CELSP_MAX_HILITE) {
        return;
    }
    CELSP_JSON* const item = CeLspJson_NewObject();
    CELSP_JSON* const range = _MakeRange(off, off + len);
    if (!item || !range) {
        CeLspJson_Free(item);
        CeLspJson_Free(range);
        return;
    }
    CeLspJson_Set(item, "range", range);
    CeLspJson_Set(item, "kind", CeLspJson_NewNumber(3.0)); // Read
    if (CeLspJson_Add(c->arr, item)) {
        ++c->count;
    }
    else {
        CeLspJson_Free(item);
    }
}

static void _HandleDocumentHighlight(int id, const CELSP_JSON* params)
{
    _ScanSymbols();
    char name[128];
    size_t ws = 0u, we = 0u;
    const size_t n = _CursorWord(params, name, sizeof(name), &ws, &we);
    CELSP_JSON* const result = CeLspJson_NewArray();
    if ((n > 0u) && result && (_LookupSymbol(name, n) >= 0)) {
        CE_HL_CTX ctx;
        ctx.arr = result;
        ctx.count = 0;
        _ForEachWord(name, n, _HlCb, &ctx);
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

// ---- extended diagnostics ---------------------------------------------------------------

static void _RunExtraDiagnostics(void)
{
    if (!g_docText) {
        return;
    }
    int region = 0; // 0 AA, 1 Lua, 2 asm, 3 ccode
    bool inEnable = false;
    bool sawLua = false;
    int fnCount = 0, endCount = 0;
    int nApi = 0, nMnem = 0, nTgt = 0, nTrail = 0, nTab = 0;
    char forVars[64][32];
    int nForVars = 0;

    const char* p = g_docText;
    size_t lineOff = 0u;
    while (lineOff <= g_docLen) {
        const char* const eol = (const char*)memchr(p, '\n', g_docLen - lineOff);
        const size_t len = eol ? (size_t)(eol - p) : (g_docLen - lineOff);

        if (_MemContains(p, len, "{$lua}")) {
            region = (region == 1) ? 0 : 1;
            sawLua = true;
        }
        else if (_MemContains(p, len, "{$asm}")) {
            region = (region == 2) ? 1 : 2;
        }
        else if (_MemContains(p, len, "{$ccode}") || _MemContains(p, len, "{$CODE}")) {
            region = (region == 3) ? 1 : 3;
        }
        if (_MemContains(p, len, "[ENABLE]")) {
            inEnable = true;
        }
        if (_MemContains(p, len, "[DISABLE]")) {
            inEnable = false;
        }

        size_t blen = len;
        if ((blen > 0u) && (p[blen - 1u] == '\r')) {
            --blen;
        }
        size_t core = blen;
        while ((core > 0u) && ((p[core - 1u] == ' ') || (p[core - 1u] == '\t'))) {
            --core;
        }
        if ((core < blen) && (nTrail < CELSP_DIAG_CAP)) {
            _AddDiag(lineOff + core, lineOff + blen, 4, "ce.style-trailing-ws",
                     "Trailing whitespace");
            ++nTrail;
        }
        size_t lead = 0u;
        bool hasTab = false;
        while ((lead < core) && ((p[lead] == ' ') || (p[lead] == '\t'))) {
            if (p[lead] == '\t') {
                hasTab = true;
            }
            ++lead;
        }
        if (hasTab && (nTab < CELSP_DIAG_CAP)) {
            _AddDiag(lineOff, lineOff + lead, 4, "ce.style-tab", "Tab indentation");
            ++nTab;
        }
        const bool comment = (core == 0u) || (p[lead] == ';') ||
            ((lead + 1u < blen) && (((p[lead] == '/') && (p[lead + 1u] == '/')) ||
                                    ((p[lead] == '-') && (p[lead + 1u] == '-'))));

        if (!comment && (region == 1)) {
            char buf[1024];
            size_t bi = 0u;
            for (size_t k = lead; (k < core) && ((bi + 1u) < sizeof(buf)); ++k) {
                const char ch = p[k];
                if (((ch == '/') || (ch == '-')) && ((k + 1u) < core) && (p[k + 1u] == ch)) {
                    break;
                }
                buf[bi++] = ch;
            }
            buf[bi] = '\0';
            fnCount += _CountKeyword(buf, "function");
            endCount += _CountKeyword(buf, "end");

            // for-loop variables are legitimate callees' neighbours; collect them
            if ((bi >= 3u) && (strncmp(buf, "for", 3u) == 0) && !_IsWordBody((unsigned char)buf[3])) {
                size_t k = 3u;
                for (int pass = 0; (pass < 2) && (nForVars < 64); ++pass) {
                    while ((k < bi) && ((buf[k] == ' ') || (buf[k] == '\t') || (buf[k] == ','))) { ++k; }
                    if ((k < bi) && _IsWordStart((unsigned char)buf[k])) {
                        const size_t s = k;
                        while ((k < bi) && _IsWordBody((unsigned char)buf[k])) { ++k; }
                        if ((k - s) < sizeof(forVars[0])) {
                            memcpy(forVars[nForVars], buf + s, k - s);
                            forVars[nForVars][k - s] = '\0';
                            ++nForVars;
                        }
                    }
                    else {
                        break;
                    }
                }
            }

            // unknown API calls: identifier directly followed by '('
            size_t i = 0u;
            while (i < bi) {
                const char ch = buf[i];
                if ((ch == '"') || (ch == '\'')) {
                    ++i;
                    while ((i < bi) && (buf[i] != ch)) { ++i; }
                    if (i < bi) { ++i; }
                    continue;
                }
                if (_IsWordStart((unsigned char)ch)) {
                    const size_t s = i;
                    while ((i < bi) && _IsWordBody((unsigned char)buf[i])) { ++i; }
                    const size_t wl = i - s;
                    size_t j = i;
                    while ((j < bi) && ((buf[j] == ' ') || (buf[j] == '\t'))) { ++j; }
                    if ((j < bi) && (buf[j] == '(') && ((s == 0u) || (buf[s - 1u] != '.'))) {
                        char word[64];
                        if ((wl > 0u) && (wl < sizeof(word))) {
                            memcpy(word, buf + s, wl);
                            word[wl] = '\0';
                            bool known = _InTableI(g_luaKeywords, buf + s, wl) ||
                                         _InTableI(g_luaStdlib, buf + s, wl) ||
                                         (_FindEntry(word) != NULL) ||
                                         (CeApi_FindType(word) != NULL) ||
                                         (_LookupSymbol(word, wl) >= 0);
                            if (!known) {
                                for (int f = 0; f < nForVars; ++f) {
                                    if (strcmp(forVars[f], word) == 0) {
                                        known = true;
                                        break;
                                    }
                                }
                            }
                            if (!known && (nApi < CELSP_DIAG_CAP)) {
                                _AddDiag(lineOff + s, lineOff + i, 2, "ce.lua-unknown-api",
                                         "Unknown function '%s'", word);
                                ++nApi;
                            }
                        }
                    }
                    continue;
                }
                ++i;
            }
        }
        else if (!comment && ((region == 2) || ((region == 0) && inEnable))) {
            // AA/asm instruction lines: first word must be mnemonic/directive/label
            size_t s = lead;
            while ((s < core) && ((p[s] == ' ') || (p[s] == '\t'))) { ++s; }
            if ((s < core) && _IsWordStart((unsigned char)p[s])) {
                size_t e = s;
                while ((e < core) && _IsWordBody((unsigned char)p[e])) { ++e; }
                bool labelLine = false;
                size_t k = e;
                while ((k < core) && ((p[k] == ' ') || (p[k] == '\t'))) { ++k; }
                if ((k < core) && (p[k] == ':')) {
                    labelLine = true;
                }
                for (size_t z = e; (z < core) && !labelLine; ++z) {
                    if (p[z] == ':') {
                        labelLine = true; // offset label like `newmem+10:`
                    }
                }
                if (!labelLine) {
                    char word[64];
                    const size_t wl = (e - s < (sizeof(word) - 1u)) ? (e - s) : (sizeof(word) - 1u);
                    memcpy(word, p + s, wl);
                    word[wl] = '\0';
                    if (_InTableI(g_asmMnemonics, word, wl)) {
                        if (_InTableI(g_jumpMnemonics, word, wl) && (nTgt < CELSP_DIAG_CAP)) {
                            size_t t = e;
                            while ((t < core) && ((p[t] == ' ') || (p[t] == '\t'))) { ++t; }
                            if (((t + 5u) <= core) && (strncmp(p + t, "short", 5u) == 0)) {
                                t += 5u;
                                while ((t < core) && ((p[t] == ' ') || (p[t] == '\t'))) { ++t; }
                            }
                            if ((t < core) && ((p[t] < '0') || (p[t] > '9')) &&
                                (p[t] != '[') && (p[t] != '$') && (p[t] != '(')) {
                                const size_t ts = t;
                                bool dotted = false;
                                while (t < core) {
                                    const char ch = p[t];
                                    if (_IsWordBody((unsigned char)ch)) {
                                        ++t;
                                    }
                                    else if (ch == '.') {
                                        dotted = true;
                                        ++t;
                                    }
                                    else {
                                        break;
                                    }
                                }
                                const size_t tl = t - ts;
                                if ((tl > 0u) && !dotted && (tl < sizeof(word))) {
                                    memcpy(word, p + ts, tl);
                                    word[tl] = '\0';
                                    if (_LookupSymbol(word, tl) < 0) {
                                        _AddDiag(lineOff + ts, lineOff + t, 2,
                                                 "ce.asm-undefined-target",
                                                 "Undefined jump/call target '%s'", word);
                                        ++nTgt;
                                    }
                                }
                            }
                        }
                    }
                    else if (_InTableI(g_aaDirectives, word, wl) ||
                             _InTableI(g_asmRegisters, word, wl) ||
                             (_FindEntry(word) != NULL) ||
                             (_LookupSymbol(word, wl) >= 0)) {
                        // legitimate line (directive, register, catalogue, symbol)
                    }
                    else if (nMnem < CELSP_DIAG_CAP) {
                        _AddDiag(lineOff + s, lineOff + e, 2, "ce.asm-unknown-mnemonic",
                                 "Suspicious mnemonic '%s'", word);
                        ++nMnem;
                    }
                }
            }
        }

        if (!eol) {
            break;
        }
        p = eol + 1u;
        lineOff = (size_t)(p - g_docText);
    }

    if (sawLua && (fnCount != endCount)) {
        _AddDiag(0u, 0u, 1, "ce.lua-blocks",
                 "Unbalanced Lua blocks: %d 'function' vs %d 'end'", fnCount, endCount);
    }
}

// ----------------------------------------------------------------------------
// Dispatch
// ----------------------------------------------------------------------------

static void _HandleInitialize(int id, const CELSP_JSON* params)
{
    (void)params;
    g_utf16 = false;
    CELSP_JSON* const encodings = CeLspJson_NewArray();
    if (encodings) {
        CeLspJson_Add(encodings, CeLspJson_NewString("utf-8"));
        CeLspJson_Add(encodings, CeLspJson_NewString("utf-16"));
    }
    CELSP_JSON* const completion = CeLspJson_NewObject();
    CELSP_JSON* const signature = CeLspJson_NewObject();
    CELSP_JSON* const triggerChars = CeLspJson_NewArray();
    CELSP_JSON* const sigChars = CeLspJson_NewArray();
    CELSP_JSON* const capabilities = CeLspJson_NewObject();
    CELSP_JSON* const serverInfo = CeLspJson_NewObject();
    CELSP_JSON* const result = CeLspJson_NewObject();
    if (triggerChars) {
        CeLspJson_Add(triggerChars, CeLspJson_NewString("."));
        CeLspJson_Add(triggerChars, CeLspJson_NewString("("));
    }
    if (sigChars) {
        CeLspJson_Add(sigChars, CeLspJson_NewString("("));
        CeLspJson_Add(sigChars, CeLspJson_NewString(","));
    }
    if (completion) {
        CeLspJson_Set(completion, "triggerCharacters", triggerChars);
        CeLspJson_Set(completion, "resolveProvider", CeLspJson_NewBool(true));
    }
    else { CeLspJson_Free(triggerChars); }
    if (signature) {
        CeLspJson_Set(signature, "triggerCharacters", sigChars);
    }
    else { CeLspJson_Free(sigChars); }

    CELSP_JSON* const semTokens = CeLspJson_NewObject();
    CELSP_JSON* const legend = CeLspJson_NewObject();
    CELSP_JSON* const tokTypes = CeLspJson_NewArray();
    CELSP_JSON* const tokMods = CeLspJson_NewArray();
    if (tokTypes) {
        for (size_t i = 0u; i < (sizeof(g_semTokenTypes) / sizeof(g_semTokenTypes[0])); ++i) {
            CeLspJson_Add(tokTypes, CeLspJson_NewString(g_semTokenTypes[i]));
        }
    }
    if (tokMods) {
        for (size_t i = 0u; i < (sizeof(g_semTokenModifiers) / sizeof(g_semTokenModifiers[0])); ++i) {
            CeLspJson_Add(tokMods, CeLspJson_NewString(g_semTokenModifiers[i]));
        }
    }
    if (legend) {
        CeLspJson_Set(legend, "tokenTypes", tokTypes);
        CeLspJson_Set(legend, "tokenModifiers", tokMods);
    }
    else {
        CeLspJson_Free(tokTypes);
        CeLspJson_Free(tokMods);
    }
    if (semTokens) {
        CeLspJson_Set(semTokens, "legend", legend);
        CeLspJson_Set(semTokens, "full", CeLspJson_NewBool(true));
    }
    else { CeLspJson_Free(legend); }
    CELSP_JSON* const codeLens = CeLspJson_NewObject();
    if (codeLens) {
        CeLspJson_Set(codeLens, "resolveProvider", CeLspJson_NewBool(false));
    }
    CELSP_JSON* const docLink = CeLspJson_NewObject();
    if (docLink) {
        CeLspJson_Set(docLink, "resolveProvider", CeLspJson_NewBool(false));
    }

    if (capabilities) {
        CeLspJson_Set(capabilities, "positionEncoding", CeLspJson_NewString("utf-8"));
        CeLspJson_Set(capabilities, "positionEncodings", encodings);
        CeLspJson_Set(capabilities, "textDocumentSync", CeLspJson_NewNumber(2.0)); // Incremental
        CeLspJson_Set(capabilities, "completionProvider", completion);
        CeLspJson_Set(capabilities, "signatureHelpProvider", signature);
        CeLspJson_Set(capabilities, "hoverProvider", CeLspJson_NewBool(true));
        CeLspJson_Set(capabilities, "definitionProvider", CeLspJson_NewBool(true));
        CeLspJson_Set(capabilities, "declarationProvider", CeLspJson_NewBool(true));
        CeLspJson_Set(capabilities, "referencesProvider", CeLspJson_NewBool(true));
        CeLspJson_Set(capabilities, "documentSymbolProvider", CeLspJson_NewBool(true));
        CeLspJson_Set(capabilities, "workspaceSymbolProvider", CeLspJson_NewBool(true));
        CeLspJson_Set(capabilities, "documentFormattingProvider", CeLspJson_NewBool(true));
        CeLspJson_Set(capabilities, "documentHighlightProvider", CeLspJson_NewBool(true));
        CeLspJson_Set(capabilities, "inlayHintProvider", CeLspJson_NewBool(true));
        CeLspJson_Set(capabilities, "semanticTokensProvider", semTokens);
        CeLspJson_Set(capabilities, "codeLensProvider", codeLens);
        CeLspJson_Set(capabilities, "documentLinkProvider", docLink);
    }
    else {
        CeLspJson_Free(encodings);
        CeLspJson_Free(completion);
        CeLspJson_Free(signature);
        CeLspJson_Free(semTokens);
        CeLspJson_Free(codeLens);
        CeLspJson_Free(docLink);
    }
    if (serverInfo) {
        CeLspJson_Set(serverInfo, "name", CeLspJson_NewString(CELSP_SERVER_NAME));
        CeLspJson_Set(serverInfo, "version", CeLspJson_NewString(CELSP_SERVER_VERSION));
    }
    if (result) {
        CeLspJson_Set(result, "capabilities", capabilities);
        CeLspJson_Set(result, "serverInfo", serverInfo);
    }
    else {
        CeLspJson_Free(capabilities);
        CeLspJson_Free(serverInfo);
    }
    _SendResult(id, result);
    CeLspJson_Free(result);
}

static void _HandleDidOpen(const CELSP_JSON* params)
{
    const CELSP_JSON* const doc = CeLspJson_Get(params, "textDocument");
    const char* const uri = CeLspJson_GetString(doc, "uri");
    const char* const text = CeLspJson_GetString(doc, "text");
    _SetUri(uri ? uri : "file:///untitled.cea");
    _SetText(text ? text : "");
    g_docVersion = CeLspJson_GetInt(doc, "version");
    g_docOpen = true;
    g_diagDirtyFirst = INT_MAX;
    g_diagDirtyLast = -1;
    _PublishDiagnostics();
}

static void _HandleDidChange(const CELSP_JSON* params)
{
    const CELSP_JSON* const doc = CeLspJson_Get(params, "textDocument");
    g_docVersion = CeLspJson_GetInt(doc, "version");
    const CELSP_JSON* const changes = CeLspJson_Get(params, "contentChanges");
    const size_t count = CeLspJson_Count(changes);
    g_diagDirtyFirst = INT_MAX;
    g_diagDirtyLast = -1;
    for (size_t i = 0u; i < count; ++i) {
        const CELSP_JSON* const change = CeLspJson_At(changes, i);
        const char* const text = CeLspJson_GetString(change, "text");
        const CELSP_JSON* const range = CeLspJson_Get(change, "range");
        if (!range) {
            _SetText(text);
            g_diagDirtyFirst = INT_MAX;
            g_diagDirtyLast = -1;
            continue;
        }
        const CELSP_JSON* const start = CeLspJson_Get(range, "start");
        const CELSP_JSON* const end = CeLspJson_Get(range, "end");
        if (!start || !end) {
            g_diagDirtyFirst = INT_MAX;
            g_diagDirtyLast = -1;
            continue;
        }
        const int firstLine = CeLspJson_GetInt(start, "line");
        const int lastLine = CeLspJson_GetInt(end, "line");
        if (firstLine < g_diagDirtyFirst) { g_diagDirtyFirst = firstLine; }
        if (lastLine > g_diagDirtyLast) { g_diagDirtyLast = lastLine; }
        if (!_ApplyRangeChange(text, firstLine,
                               CeLspJson_GetInt(start, "character"),
                               lastLine, CeLspJson_GetInt(end, "character"))) {
            _SendNotification("np3/lspResync", NULL);
        }
    }
    g_docOpen = true;
    if (g_diagDirtyFirst <= g_diagDirtyLast) {
        _PublishDiagnosticsForLines(g_diagDirtyFirst, g_diagDirtyLast);
    }
    else {
        _PublishDiagnostics();
    }
}

static void _HandleDidClose(const CELSP_JSON* params)
{
    (void)params;
    g_docOpen = false;
    CELSP_JSON* const empty = CeLspJson_NewArray();
    CELSP_JSON* const notify = CeLspJson_NewObject();
    if (notify) {
        CeLspJson_Set(notify, "uri", CeLspJson_NewString(g_docUri ? g_docUri : "file:///untitled.cea"));
        CeLspJson_Set(notify, "diagnostics", empty);
    }
    else {
        CeLspJson_Free(empty);
    }
    _SendNotification("textDocument/publishDiagnostics", notify);
    CeLspJson_Free(notify);
    _SetText(NULL);
    _SetUri(NULL);
}

static void _Dispatch(const CELSP_JSON* msg)
{
    const char* const method = CeLspJson_GetString(msg, "method");
    const CELSP_JSON* const idNode = CeLspJson_Get(msg, "id");
    const bool hasId = (idNode != NULL);
    const int id = CeLspJson_Int(idNode);
    const CELSP_JSON* const params = CeLspJson_Get(msg, "params");

    if (!method) {
        // A response from the client (we never send requests). Nothing to do.
        return;
    }
    if (strcmp(method, "initialize") == 0) {
        _HandleInitialize(id, params);
    }
    else if (strcmp(method, "initialized") == 0) {
        _Log("initialized");
    }
    else if (strcmp(method, "shutdown") == 0) {
        g_shutdown = true;
        _SendResult(id, NULL);
    }
    else if (strcmp(method, "exit") == 0) {
        g_exit = true;
    }
    else if (strcmp(method, "$/cancelRequest") == 0) {
        // All handlers are fast and synchronous; cancellation is a no-op.
    }
    else if (strcmp(method, "textDocument/didOpen") == 0) {
        _HandleDidOpen(params);
    }
    else if (strcmp(method, "textDocument/didChange") == 0) {
        _HandleDidChange(params);
    }
    else if (strcmp(method, "textDocument/didSave") == 0) {
        if (g_docOpen) { _PublishDiagnostics(); }
    }
    else if (strcmp(method, "textDocument/didClose") == 0) {
        _HandleDidClose(params);
    }
    else if (strcmp(method, "textDocument/completion") == 0) {
        _HandleCompletion(id, params);
    }
    else if (strcmp(method, "completionItem/resolve") == 0) {
        _HandleResolve(id, params);
    }
    else if (strcmp(method, "textDocument/signatureHelp") == 0) {
        _HandleSignatureHelp(id, params);
    }
    else if (strcmp(method, "textDocument/hover") == 0) {
        _HandleHover(id, params);
    }
    else if (strcmp(method, "textDocument/definition") == 0) {
        _HandleDefinition(id, params);
    }
    else if (strcmp(method, "textDocument/declaration") == 0) {
        _HandleDefinition(id, params);
    }
    else if (strcmp(method, "textDocument/references") == 0) {
        _HandleReferences(id, params);
    }
    else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        _HandleDocumentSymbol(id, params);
    }
    else if (strcmp(method, "workspace/symbol") == 0) {
        _HandleWorkspaceSymbol(id, params);
    }
    else if (strcmp(method, "textDocument/formatting") == 0) {
        _HandleFormatting(id, params);
    }
    else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
        _HandleSemanticTokensFull(id, params);
    }
    else if (strcmp(method, "textDocument/inlayHint") == 0) {
        _HandleInlayHint(id, params);
    }
    else if (strcmp(method, "textDocument/codeLens") == 0) {
        _HandleCodeLens(id, params);
    }
    else if (strcmp(method, "textDocument/documentLink") == 0) {
        _HandleDocumentLink(id, params);
    }
    else if (strcmp(method, "textDocument/documentHighlight") == 0) {
        _HandleDocumentHighlight(id, params);
    }
    else if (hasId) {
        _SendError(id, -32601, "Method not found");
    }
    else {
        _Log("ignoring unknown notification: %s", method);
    }
}

int main(void)
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _Log("server started");

    while (!g_exit) {
        char* const json = _ReadFrame();
        if (!json) {
            break; // stdin closed: the editor exited
        }
        CELSP_JSON* const msg = CeLspJson_Parse(json, 0u);
        free(json);
        if (msg) {
            _Dispatch(msg);
            CeLspJson_Free(msg);
        }
        else {
            _Log("malformed JSON-RPC message ignored");
        }
    }

    _SetText(NULL);
    _SetUri(NULL);
    free(g_inBuf);
    _Log("server exiting (shutdown=%d)", g_shutdown ? 1 : 0);
    return 0;
}
