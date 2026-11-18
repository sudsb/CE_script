// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLint.c                                                                    *
*   Syntax checks for Cheat Engine scripts.                                   *
*                                                                             *
*   Messages are plain ASCII so they render correctly in every document       *
*   encoding; the report dialog is a standard message box so the feature      *
*   needs no new dialog resource or localisation table.                       *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "MuiLanguage.h"
#include "SciCall.h"
#include "..\language\common_res.h"
#include "CeCompletion.h"
#include "CeDiagnostics.h"
#include "CeScript.h"
#include "CeLint.h"
#include "StyleLexers/ce_completions_data.h"

#define CE_LINT_INTERVAL_MS       350
#define CE_LINT_MAX_DOC_LEN       (256 * 1024)
#define CE_LINT_FULL_CHECKS_MAX   (96 * 1024)
#define CE_LINT_HEURISTIC_MAX     40 // cap per heuristic so real errors survive
#define CE_LINT_MAX_EXTERNAL       64

// ----------------------------------------------------------------------------

// Findings from CE's own syntax checker.  Written on the UI thread after the
// online-check worker finishes, read by CeLint_Run on the same thread.
static CE_EXTERNAL_DIAG s_external[CE_LINT_MAX_EXTERNAL];
static int s_externalCount = 0;

void CeLint_SetExternalDiags(const CE_EXTERNAL_DIAG* items, int count) {
    s_externalCount = 0;
    if (!items || count <= 0) {
        return;
    }
    if (count > CE_LINT_MAX_EXTERNAL) {
        count = CE_LINT_MAX_EXTERNAL;
    }
    for (int i = 0; i < count; ++i) {
        s_external[i] = items[i];
        s_external[i].message[CE_LINT_MSG_CCH - 1] = '\0';
    }
    s_externalCount = count;
}

void CeLint_ClearExternalDiags(void) {
    s_externalCount = 0;
}

static void _AddDiagAt(CE_LINT* lint, int line, int col, int len, bool error, const char* msg) {
    if (!lint || lint->count >= CE_LINT_MAX_DIAGS) {
        return;
    }
    CE_DIAG* const diag = &lint->items[lint->count++];
    diag->line = (line < 1) ? 1 : line;
    diag->col = col;
    diag->len = len;
    diag->error = error;
    diag->alert = false;
    StringCchCopyA(diag->message, CE_LINT_MSG_CCH, msg ? msg : "");
    if (error) {
        ++lint->errors;
    }
    else {
        ++lint->warnings;
    }
}

static void _AddDiag(CE_LINT* lint, int line, bool error, const char* msg) {
    _AddDiagAt(lint, line, -1, 0, error, msg);
}

// Warnings that must be visible inline (see CeLint_ApplyAnnotations).
static void _AddAlertDiag(CE_LINT* lint, int line, const char* msg) {
    if (!lint || lint->count >= CE_LINT_MAX_DIAGS) {
        return;
    }
    CE_DIAG* const diag = &lint->items[lint->count++];
    diag->line = (line < 1) ? 1 : line;
    diag->col = -1;
    diag->len = 0;
    diag->error = false;
    diag->alert = true;
    StringCchCopyA(diag->message, CE_LINT_MSG_CCH, msg ? msg : "");
    ++lint->warnings;
}

static char _Lower(char ch) {
    return (char)((ch >= 'A' && ch <= 'Z') ? (ch + ('a' - 'A')) : ch);
}

static bool _IsIdentStart(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static bool _IsIdentChar(char ch) {
    return _IsIdentStart(ch) || (ch >= '0' && ch <= '9');
}

static bool _EqualsCI(const char* text, size_t len, const char* word) {
    size_t const wordLen = strlen(word);
    if (len != wordLen) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (_Lower(text[i]) != _Lower(word[i])) {
            return false;
        }
    }
    return true;
}

static bool _StartsWithCI(const char* text, size_t len, const char* prefix) {
    size_t const prefixLen = strlen(prefix);
    return (len >= prefixLen) && _EqualsCI(text, prefixLen, prefix);
}

// Search a generated space separated keyword list (CE_KEYWORDS_*).
static bool _InKeywordList(const char* list, const char* word) {
    if (!list || !word || word[0] == '\0') {
        return false;
    }
    const char* p = list;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        const char* const start = p;
        while (*p && *p != ' ') {
            ++p;
        }
        if (_EqualsCI(start, (size_t)(p - start), word)) {
            return true;
        }
    }
    return false;
}

static bool _IsCommentStart(const char* p, const char* end) {
    if (p >= end) {
        return true;
    }
    return (*p == ';') ||
        ((p + 1 < end) && ((p[0] == '-' && p[1] == '-') || (p[0] == '/' && p[1] == '/')));
}

// A jump target must have a real definition ("foo:", "foo=", alloc(...),
// aobscan...(...)), not merely a "label(foo)" declaration.
static bool _IsRealDefinition(const CE_DEF* def) {
    static const char* const kinds[] = {
        "label:", "assign", "local", "function", "alloc", "globalalloc",
        "sharedalloc", "aobscan", "aobscanmodule", "aobscanregion", "define",
    };
    for (int i = 0; i < (int)COUNTOF(kinds); ++i) {
        if (_EqualsCI(def->kind, strlen(def->kind), kinds[i])) {
            return true;
        }
    }
    return false;
}

static bool _FindRealDef(const CE_DEF* defs, int count, const char* name) {
    for (int i = 0; i < count; ++i) {
        if ((_stricmp(defs[i].name, name) == 0) && _IsRealDefinition(&defs[i])) {
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// Symbol scope helpers shared by the semantic checks.
//
// A script is not a closed world: Cheat Engine itself defines a few names, the
// user may {$include} another table, and Lua code can inject globals at
// runtime.  Unknown-name findings are only useful while the document is the
// whole truth, so the checks below decide that once per pass.

// Names CE accepts in every generated template: "newmem"/"originalcode" and
// the jump-back labels.  CE's own template emits "label(return)" too, so
// "return" must not be reported as a keyword clash even though it is listed
// among the AA keywords.
static const char* const s_ceBuiltinSymbols[] = {
    "newmem", "originalcode", "returnhere", "exit", "return",
};

static bool _IsCeBuiltinSymbol(const char* name)
{
    for (int i = 0; i < (int)COUNTOF(s_ceBuiltinSymbols); ++i) {
        if (_stricmp(name, s_ceBuiltinSymbols[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool _DocContainsCI(const CE_DOC* doc, const char* needle)
{
    size_t const len = strlen(needle);
    if (!doc || (len == 0)) {
        return false;
    }
    for (DocPos i = 0; (i + (DocPos)len) <= doc->len; ++i) {
        if (_EqualsCI(doc->text + i, len, needle)) {
            return true;
        }
    }
    return false;
}

// rawset(_G, name, value) / setmetatable(_G, ...) can define names that no
// static pass can see.  The function name must be followed by "(_G" or "(_ENV"
// (spaces allowed) to count: setmetatable(t, mt) on an ordinary table does not
// open the global namespace.
static bool _DocInjectsGlobals(const CE_DOC* doc)
{
    static const char* const fns[] = { "rawset", "setmetatable" };
    static const char* const envs[] = { "_G", "_ENV" };
    for (int f = 0; f < (int)COUNTOF(fns); ++f) {
        size_t const fnLen = strlen(fns[f]);
        for (DocPos i = 0; (i + (DocPos)fnLen) <= doc->len; ++i) {
            if (!_EqualsCI(doc->text + i, fnLen, fns[f])) {
                continue;
            }
            if ((i > 0) && _IsIdentChar(doc->text[i - 1])) {
                continue;
            }
            const char* p = doc->text + i + (DocPos)fnLen;
            const char* const end = doc->text + doc->len;
            p = CeScript_SkipBlanks(p, end);
            if ((p >= end) || (*p != '(')) {
                continue;
            }
            p = CeScript_SkipBlanks(p + 1, end);
            for (int e = 0; e < (int)COUNTOF(envs); ++e) {
                size_t const envLen = strlen(envs[e]);
                if (((size_t)(end - p) >= envLen) && _EqualsCI(p, envLen, envs[e]) &&
                    (((p + envLen) >= end) || !_IsIdentChar(p[envLen]))) {
                    return true;
                }
            }
        }
    }
    return false;
}

// True when the script pulls in symbols or generates them at runtime.
static bool _DocHasOpenSymbolScope(const CE_DOC* doc)
{
    return _DocContainsCI(doc, "{$include") ||
           _DocContainsCI(doc, "{$try") ||
           _DocContainsCI(doc, "loadstring") ||
           _DocContainsCI(doc, "load(") ||
           _DocContainsCI(doc, "dofile") ||
           _DocContainsCI(doc, "require") ||
           _DocInjectsGlobals(doc);
}

// Index of the [ENABLE]/[DISABLE] block that contains |line| (0-based), or -1.
static int _SectionOfLine(const CE_SECTION* secs, int count, DocLn line)
{
    int found = -1;
    for (int i = 0; i < count; ++i) {
        if (secs[i].line <= line) {
            found = i;
        }
        else {
            break;
        }
    }
    return found;
}

static bool _SameSection(const CE_SECTION* secs, int count, DocLn a, DocLn b)
{
    return _SectionOfLine(secs, count, a) == _SectionOfLine(secs, count, b);
}

// A symbol definition line: "name:", "Namespace.Class:Method+16f:", "label-4:",
// or a symbol CE read from the target process, e.g.
// "System.Collections.Generic.Dictionary`2[System.String,System.Single]:TryInsert+3d0:".
// Such names may contain '.', ':', '+', '-', '`', '[', ']', ',' and more, so the
// only reliable test is "the first token ends with ':'".
static bool _IsLabelDefinitionLine(const char* p, const char* end)
{
    const char* r = p;
    while ((r < end) && (*r != ' ') && (*r != '\t') && (*r != ';')) {
        ++r;
    }
    return (r > (p + 1)) && (r[-1] == ':');
}

static bool _IsKnownBareOperand(const char* name);
static bool _IsKnownAaCommand(const char* name);
static int  _FindWordCol(const CE_DOC* doc, int line1, const char* name);

// ----------------------------------------------------------------------------
// Brackets, strings and block comments.

static void _CheckBrackets(const CE_DOC* doc, const unsigned char* modes, CE_LINT* lint) {
    char stack[128] = { 0 };
    int stackLine[128] = { 0 };
    int stackCol[128] = { 0 };
    int depth = 0;
    int blockMode = 0; // 0 none, 1 Lua long comment/string, 2 C block comment
    int blockLine = 0;

    for (DocLn line = 0; line < doc->lines; ++line) {
        DocPos n = 0;
        const char* const lineStart = doc->text + doc->starts[line];
        const char* p = CeScript_LinePtr(doc, line, &n);
        const char* const end = p + n;
        CE_LANG const mode = (CE_LANG)modes[line];
        if (mode == CE_LANG_COMMENT) {
            continue; // AA "{ ... }" block comment, not code
        }
        bool inString = false;
        char quote = '\0';
        int stringCol = 0;

        while (p < end) {
            if (blockMode == 1) {
                if (p[0] == ']' && (p + 1) < end && p[1] == ']') {
                    blockMode = 0;
                    p += 2;
                    continue;
                }
                ++p;
                continue;
            }
            if (blockMode == 2) {
                if (p[0] == '*' && (p + 1) < end && p[1] == '/') {
                    blockMode = 0;
                    p += 2;
                    continue;
                }
                ++p;
                continue;
            }
            if (inString) {
                if (p[0] == '\\' && (p + 1) < end) {
                    p += 2;
                    continue;
                }
                if (p[0] == quote) {
                    inString = false;
                }
                ++p;
                continue;
            }
            if (p[0] == '-' && (p + 1) < end && p[1] == '-') {
                if ((mode == CE_LANG_LUA) && (p + 3) < end && p[2] == '[' && p[3] == '[') {
                    blockMode = 1;
                    blockLine = (int)line + 1;
                    p += 4;
                    continue;
                }
                break; // line comment
            }
            if ((mode != CE_LANG_LUA) && p[0] == '/' && (p + 1) < end && p[1] == '/') {
                break; // Lua 5.3 uses "//" as floor division, not as a comment
            }
            if ((mode == CE_LANG_AA) && p[0] == ';') {
                break;
            }
            if ((mode == CE_LANG_C) && p[0] == '/' && (p + 1) < end && p[1] == '*') {
                blockMode = 2;
                blockLine = (int)line + 1;
                p += 2;
                continue;
            }
            if (p[0] == '"' || p[0] == '\'') {
                inString = true;
                quote = p[0];
                stringCol = (int)(p - lineStart);
                ++p;
                continue;
            }
            if ((mode == CE_LANG_LUA) && p[0] == '[' && (p + 1) < end && p[1] == '[') {
                blockMode = 1;
                blockLine = (int)line + 1;
                p += 2;
                continue;
            }
            if (p[0] == '(' || p[0] == '[' || p[0] == '{') {
                if (depth < (int)COUNTOF(stack)) {
                    stack[depth] = p[0];
                    stackLine[depth] = (int)line + 1;
                    stackCol[depth] = (int)(p - lineStart);
                    ++depth;
                }
                ++p;
                continue;
            }
            if (p[0] == ')' || p[0] == ']' || p[0] == '}') {
                char const wanted = (p[0] == ')') ? '(' : ((p[0] == ']') ? '[' : '{');
                if (depth > 0 && stack[depth - 1] == wanted) {
                    --depth;
                }
                else {
                    char msg[CE_LINT_MSG_CCH];
                    StringCchPrintfA(msg, COUNTOF(msg), "多余的 '%c'", p[0]);
                    _AddDiagAt(lint, (int)line + 1, (int)(p - lineStart), 1, true, msg);
                }
                ++p;
                continue;
            }
            ++p;
        }

        if (inString) {
            _AddDiagAt(lint, (int)line + 1, stringCol, 1, true, "字符串未闭合");
        }
    }

    if (blockMode != 0) {
        _AddDiag(lint, blockLine, true, "块注释未闭合");
    }
    if (depth > 0) {
        char msg[CE_LINT_MSG_CCH];
        StringCchPrintfA(msg, COUNTOF(msg), "缺少 '%c'", stack[depth - 1]);
        _AddDiagAt(lint, stackLine[depth - 1], stackCol[depth - 1], 1, true, msg);
    }
}

// ----------------------------------------------------------------------------
// Sections.

static void _CheckSections(const CE_DOC* doc, const unsigned char* modes, CE_LINT* lint) {
    CE_SECTION sections[CE_MAX_SECTIONS];
    int const count = CeScript_CollectSections(doc, sections, CE_MAX_SECTIONS);
    int enableCount = 0;
    int disableCount = 0;
    for (int i = 0; i < count; ++i) {
        if (sections[i].enable) {
            ++enableCount;
        }
        else {
            ++disableCount;
        }
    }
    // A missing counterpart section is not reported: many valid Lua-only
    // scripts only have [ENABLE], and templates add the counterpart later.
    if (enableCount > 1) {
        _AddDiag(lint, 1, false, "重复的 [ENABLE] 段");
    }
    if (disableCount > 1) {
        _AddDiag(lint, 1, false, "重复的 [DISABLE] 段");
    }

    // Unknown [SECTION] markers.  Use the same strict, whole-line marker
    // grammar as the section model: a Lua table constructor key such as
    // "[1] = 'hello'" or "[key] = value" is NOT a section, and neither is an
    // index expression.  Keeping this check (rather than disabling it) still
    // catches genuinely misspelled markers like [ENABL].
    int block = 0;
    for (DocLn line = 0; line < doc->lines; ++line) {
        DocPos n = 0;
        const char* const start = CeScript_LinePtr(doc, line, &n);
        const char* const end = start + n;
        int const mode = (modes[line] == (unsigned char)CE_LANG_C) ?
            (int)CE_LANG_C : (int)CE_LANG_LUA;
        if (block != 0) {
            CeScript_UpdateBlockComment(start, end, mode, &block);
            continue;
        }

        DocPos tn = 0;
        const char* const p = CeScript_Trimmed(doc, line, &tn);
        char marker[64] = { '\0' };
        if (!_IsCommentStart(p, p + tn) && (modes[line] != (unsigned char)CE_LANG_C) &&
            CeScript_SectionMarker(p, (size_t)tn, marker, COUNTOF(marker)) &&
            !_EqualsCI(marker, strlen(marker), "enable") &&
            !_EqualsCI(marker, strlen(marker), "disable")) {
            char msg[CE_LINT_MSG_CCH];
            StringCchPrintfA(msg, COUNTOF(msg), "未知的段标记 [%s]", marker);
            _AddDiag(lint, (int)line + 1, false, msg);
        }
        CeScript_UpdateBlockComment(start, end, mode, &block);
    }
}

// ----------------------------------------------------------------------------
// alloc/dealloc and registersymbol/unregistersymbol pairs.

static bool _MentionsCall(const CE_DOC* doc, const char* func, const char* name) {
    size_t const funcLen = strlen(func);
    size_t const nameLen = strlen(name);
    const char* p = doc->text;
    const char* const end = doc->text + doc->len;
    while (p < end) {
        const char* const hit = (const char*)memchr(p, func[0], (size_t)(end - p));
        if (!hit) {
            break;
        }
        p = hit + 1;
        if (hit > doc->text && _IsIdentChar(hit[-1])) {
            continue; // "dealloc(" must not count as "alloc("
        }
        if ((size_t)(end - hit) < funcLen || !_EqualsCI(hit, funcLen, func)) {
            continue;
        }
        const char* q = hit + funcLen;
        while (q < end && (*q == ' ' || *q == '\t')) {
            ++q;
        }
        if (q >= end || *q != '(') {
            continue;
        }
        ++q;
        while (q < end && (*q == ' ' || *q == '\t')) {
            ++q;
        }
        if ((size_t)(end - q) < nameLen || !_EqualsCI(q, nameLen, name)) {
            continue;
        }
        const char* after = q + nameLen;
        if (after < end && (_IsIdentChar(*after) || *after == '.')) {
            continue;
        }
        return true;
    }
    return false;
}

static void _CheckPairs(const CE_DOC* doc, const CE_DEF* defs, int defCount, CE_LINT* lint) {
    if (_DocHasOpenSymbolScope(doc)) {
        return; // the release may live in an included table
    }
    CE_SECTION secs[CE_MAX_SECTIONS];
    int const secCount = CeScript_CollectSections(doc, secs, CE_MAX_SECTIONS);
    // A missing [DISABLE] block is reported through the pair itself (below):
    // alloc/registersymbol without the release half is exactly what a script
    // that still needs its counterpart looks like.
    for (int i = 0; i < defCount; ++i) {
        DocLn const defLine = (defs[i].line > 0) ? (DocLn)(defs[i].line - 1) : 0;
        int const sec = _SectionOfLine(secs, secCount, defLine);
        // alloc/registersymbol inside [DISABLE] is the release half of a pair.
        if ((sec >= 0) && !secs[sec].enable) {
            continue;
        }
        if (_EqualsCI(defs[i].kind, strlen(defs[i].kind), "alloc")) {
            if (!_MentionsCall(doc, "dealloc", defs[i].name)) {
                char msg[CE_LINT_MSG_CCH];
                StringCchPrintfA(msg, COUNTOF(msg), "alloc(%s) 没有对应的 dealloc", defs[i].name);
                _AddDiag(lint, (int)defs[i].line, false, msg);
            }
        }
        else if (_EqualsCI(defs[i].kind, strlen(defs[i].kind), "registersymbol")) {
            if (!_MentionsCall(doc, "unregistersymbol", defs[i].name)) {
                char msg[CE_LINT_MSG_CCH];
                StringCchPrintfA(msg, COUNTOF(msg), "registersymbol(%s) 没有对应的 unregistersymbol", defs[i].name);
                _AddDiag(lint, (int)defs[i].line, false, msg);
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Labels and jump targets.

// "jmp short foo" / "call qword ptr [rax]" - the first word is not the target.
static bool _IsJumpOperandPrefix(const char* word)
{
    static const char* const prefixes[] = { "short", "near", "far", "rel", "abs" };
    for (int i = 0; i < (int)COUNTOF(prefixes); ++i) {
        if (_stricmp(word, prefixes[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool _IsJumpOpcode(const char* op)
{
    static const char* const jumps[] = {
        "jmp", "call", "ja", "jae", "jb", "jbe", "jc", "je", "jg", "jge",
        "jl", "jle", "jna", "jnae", "jnb", "jnbe", "jnc", "jne", "jng",
        "jnge", "jnl", "jnle", "jno", "jnp", "jns", "jnz", "jo", "jp",
        "jpe", "jpo", "js", "jz", "jcxz", "jecxz", "jrcxz", NULL
    };
    for (int i = 0; jumps[i]; ++i) {
        if (_stricmp(op, jumps[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void _CheckLabels(const CE_DOC* doc, const unsigned char* modes, const CE_DEF* defs, int defCount, CE_LINT* lint) {
    bool const openScope = _DocHasOpenSymbolScope(doc);
    bool const defsTruncated = (defCount >= CE_MAX_DEFS);
    CE_SECTION secs[CE_MAX_SECTIONS];
    int const secCount = CeScript_CollectSections(doc, secs, CE_MAX_SECTIONS);

    // A "label(x)" declaration without the matching "x:" line: the template
    // inserts the declaration first, and the label stays dangling until the
    // user writes the definition - which is exactly what must be flagged.
    if (!openScope && !defsTruncated) {
        for (int i = 0; i < defCount; ++i) {
            if (!_EqualsCI(defs[i].kind, strlen(defs[i].kind), "label") ||
                _IsCeBuiltinSymbol(defs[i].name)) {
                continue;
            }
            // Any real definition satisfies the declaration: "x:", define(x),
            // an aobscan result, alloc(x), ... - not just the label form.
            if (!_FindRealDef(defs, defCount, defs[i].name)) {
                char msg[CE_LINT_MSG_CCH];
                StringCchPrintfA(msg, COUNTOF(msg), "label(%s) 缺少 %s: 定义",
                                 defs[i].name, defs[i].name);
                int const col = _FindWordCol(doc, (int)defs[i].line, defs[i].name);
                _AddDiagAt(lint, (int)defs[i].line, col, (int)strlen(defs[i].name), false, msg);
            }
        }
    }

    // Two "foo:" definitions in the same section are ambiguous.  The same name
    // in [ENABLE] and [DISABLE] is the normal mirror and must stay quiet.
    for (int i = 0; i < defCount; ++i) {
        if (!_EqualsCI(defs[i].kind, strlen(defs[i].kind), "label:")) {
            continue;
        }
        for (int j = i + 1; j < defCount; ++j) {
            if (!_EqualsCI(defs[j].kind, strlen(defs[j].kind), "label:") ||
                (_stricmp(defs[i].name, defs[j].name) != 0)) {
                continue;
            }
            DocLn const li = (defs[i].line > 0) ? (DocLn)(defs[i].line - 1) : 0;
            DocLn const lj = (defs[j].line > 0) ? (DocLn)(defs[j].line - 1) : 0;
            if (!_SameSection(secs, secCount, li, lj)) {
                continue;
            }
            char msg[CE_LINT_MSG_CCH];
            StringCchPrintfA(msg, COUNTOF(msg), "重复的标签定义: %s", defs[i].name);
            _AddDiag(lint, (int)defs[j].line, false, msg);
        }
    }

    if (openScope || defsTruncated) {
        return;
    }

    for (DocLn line = 0; line < doc->lines; ++line) {
        if (modes[line] != (unsigned char)CE_LANG_AA) {
            continue;
        }
        DocPos n = 0;
        const char* const lineStart = doc->text + doc->starts[line];
        const char* const p = CeScript_Trimmed(doc, line, &n);
        const char* const end = p + n;
        if (_IsCommentStart(p, end)) {
            continue;
        }
        char op[16] = { '\0' };
        const char* const afterOp = CeScript_ReadIdent(p, end, op, COUNTOF(op));
        if (afterOp == p) {
            continue;
        }
        bool const isJump = _IsJumpOpcode(op);
        if (!isJump) {
            continue;
        }
        const char* t = CeScript_SkipBlanks(afterOp, end);
        char target[64] = { '\0' };
        const char* afterTarget = CeScript_ReadIdent(t, end, target, COUNTOF(target));
        if (afterTarget == t) {
            continue; // "jmp [rax+8]" / "jmp rax" style operand
        }
        if (_IsJumpOperandPrefix(target)) {
            t = CeScript_SkipBlanks(afterTarget, end);
            afterTarget = CeScript_ReadIdent(t, end, target, COUNTOF(target));
            if (afterTarget == t) {
                continue;
            }
        }
        if (_IsKnownBareOperand(target) || _IsCeBuiltinSymbol(target)) {
            continue; // register / size keyword / CE template label
        }
        // "user32.MessageBoxA", "mod.exports" and "name::local" are external.
        if ((afterTarget < end) && ((*afterTarget == '.') || (*afterTarget == ':'))) {
            continue;
        }
        if (!_FindRealDef(defs, defCount, target)) {
            char msg[CE_LINT_MSG_CCH];
            StringCchPrintfA(msg, COUNTOF(msg), "跳转目标未定义: %s", target);
            _AddDiagAt(lint, (int)line + 1, (int)(t - lineStart), (int)strlen(target), false, msg);
        }
    }
}

// ----------------------------------------------------------------------------
// Lua block balance.

static void _CheckLuaBlocks(const CE_DOC* doc, const unsigned char* modes, CE_LINT* lint) {
    int depth = 0;
    int openLine = 0;
    int openCol = 0;
    int blockMode = 0; // 1 = --[[ ]] / [[ ]], 2 = /* */
    for (DocLn line = 0; line < doc->lines; ++line) {
        if (modes[line] != (unsigned char)CE_LANG_LUA) {
            continue;
        }
        DocPos n = 0;
        const char* const lineStart = doc->text + doc->starts[line];
        const char* p = CeScript_LinePtr(doc, line, &n);
        const char* const end = p + n;
        bool hasLoop = false;

        if (blockMode != 0) {
            char const term0 = (blockMode == 1) ? ']' : '*';
            char const term1 = (blockMode == 1) ? ']' : '/';
            const char* close = NULL;
            for (const char* r = p; (r + 1) < end; ++r) {
                if ((r[0] == term0) && (r[1] == term1)) {
                    close = r;
                    break;
                }
            }
            if (!close) {
                continue; // the block still spans the next line
            }
            p = close + 2;
            blockMode = 0;
        }

        while (p < end) {
            if (p[0] == '-' && (p + 1) < end && p[1] == '-') {
                if ((p + 3) < end && p[2] == '[' && p[3] == '[') {
                    blockMode = 1;
                    break;
                }
                break;
            }
            if (p[0] == '/' && (p + 1) < end && p[1] == '*') {
                blockMode = 2;
                break;
            }
            if (p[0] == '[' && (p + 1) < end && p[1] == '[') {
                blockMode = 1;
                break;
            }
            if (p[0] == '"' || p[0] == '\'') {
                char const quote = p[0];
                ++p;
                while (p < end) {
                    if (p[0] == '\\' && (p + 1) < end) {
                        p += 2;
                        continue;
                    }
                    if (p[0] == quote) {
                        ++p;
                        break;
                    }
                    ++p;
                }
                continue;
            }
            if (!_IsIdentStart(*p)) {
                ++p;
                continue;
            }
            const char* const start = p;
            while (p < end && _IsIdentChar(*p)) {
                ++p;
            }
            size_t const len = (size_t)(p - start);
            if (_EqualsCI(start, len, "for") || _EqualsCI(start, len, "while")) {
                hasLoop = true;
                ++depth;
                openLine = (int)line + 1;
                openCol = (int)(start - lineStart);
            }
            else if (_EqualsCI(start, len, "function") || _EqualsCI(start, len, "if") ||
                     _EqualsCI(start, len, "repeat")) {
                ++depth;
                openLine = (int)line + 1;
                openCol = (int)(start - lineStart);
            }
            else if (_EqualsCI(start, len, "do")) {
                if (!hasLoop) {
                    ++depth;
                    openLine = (int)line + 1;
                    openCol = (int)(start - lineStart);
                }
            }
            else if (_EqualsCI(start, len, "end") || _EqualsCI(start, len, "until")) {
                if (depth > 0) {
                    --depth;
                }
                else {
                    _AddDiagAt(lint, (int)line + 1, (int)(start - lineStart), (int)len,
                               true, "多余的 end");
                }
            }
        }
    }
    if (depth > 0) {
        _AddDiagAt(lint, openLine, openCol, 0, true, "缺少 end");
    }
}

// ----------------------------------------------------------------------------
// {$lua} / {$asm} balance.  Parse directives with a line-aware state machine:
// literal occurrences in strings/comments must not count, and each inline
// language switch needs its own matching {$asm}.

static const char* _DirectiveAt(const char* p, const char* end, char* name, size_t cchName) {
    if ((end - p) < 5 || *p != '{' || p[1] != '$') {
        return NULL;
    }
    const char* q = p + 2;
    const char* const ident = CeScript_ReadIdent(q, end, name, cchName);
    if (ident == q) {
        return NULL;
    }
    q = CeScript_SkipBlanks(ident, end);
    if (q >= end || *q != '}') {
        return NULL;
    }
    for (char* r = name; *r; ++r) {
        *r = _Lower(*r);
    }
    return q + 1;
}

static bool _IsCBlockDirective(const char* name) {
    static const char* const aliases[] = {
        "c", "ccode", "code", "stdc", "stdcall", "fastcode", NULL
    };
    for (int i = 0; aliases[i]; ++i) {
        if (strcmp(name, aliases[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void _CheckDirectiveBlocks(const CE_DOC* doc, CE_LINT* lint) {
    if (!doc || (doc->lines <= 0)) {
        return;
    }
    unsigned char* const modes = (unsigned char*)AllocMem((size_t)doc->lines, HEAP_ZERO_MEMORY);
    if (!modes) {
        return;
    }
    CeScript_LineModes(doc, modes);
    int luaOpen = 0;
    int cOpen = 0;
    int block = 0;
    for (DocLn line = 0; line < doc->lines; ++line) {
        DocPos n = 0;
        const char* const start = CeScript_LinePtr(doc, line, &n);
        const char* const end = start + n;
        bool const inBlock = (block != 0);
        CeScript_UpdateBlockComment(start, end, (int)modes[line], &block);
        if (inBlock || (block != 0)) {
            continue;
        }
        bool inString = false;
        char quote = '\0';
        const char* p = start;
        while (p < end) {
            if (inString) {
                if ((*p == '\\') && ((p + 1) < end)) {
                    p += 2;
                    continue;
                }
                if (*p == quote) {
                    inString = false;
                }
                ++p;
                continue;
            }
            if ((*p == '-' && (p + 1) < end && p[1] == '-') ||
                (*p == '/' && (p + 1) < end && p[1] == '/')) {
                break;
            }
            if (*p == '\'' || *p == '"') {
                inString = true;
                quote = *p++;
                continue;
            }
            if (*p == '{') {
                char name[16] = { '\0' };
                const char* const after = _DirectiveAt(p, end, name, COUNTOF(name));
                if (after) {
                    if (strcmp(name, "lua") == 0) {
                        ++luaOpen;
                    }
                    else if (strcmp(name, "asm") == 0) {
                        if (luaOpen > 0) { --luaOpen; }
                        else if (cOpen > 0) { --cOpen; }
                    }
                    else if (_IsCBlockDirective(name)) {
                        ++cOpen;
                    }
                    p = after;
                    continue;
                }
            }
            ++p;
        }
    }
    FreeMem(modes);
    if (luaOpen > 0) {
        char msg[CE_LINT_MSG_CCH];
        StringCchPrintfA(msg, COUNTOF(msg), "%d 个 {$lua} 缺少 {$asm} 收尾", luaOpen);
        _AddDiag(lint, 1, true, msg);
    }
    if (cOpen > 0) {
        char msg[CE_LINT_MSG_CCH];
        StringCchPrintfA(msg, COUNTOF(msg), "%d 个 {$ccode} 缺少 {$asm} 收尾", cOpen);
        _AddDiag(lint, 1, true, msg);
    }
}

// ----------------------------------------------------------------------------
// Unreplaced template placeholders.
//
// A template fills "$name$" with the selected/clipboard variable.  When no
// variable was available the placeholder is padded with spaces, leaving a
// statement the user still has to complete ("registersymbol( )").  Custom
// templates can also leave "$x$" or "<name>" behind.  Both mean "draft", and
// Cheat Engine rejects the script on activation, so they are reported inline.

static bool _IsPlaceholderName(const char* p, const char* end)
{
    if (p >= end) {
        return false;
    }
    for (const char* r = p; r < end; ++r) {
        unsigned char const ch = (unsigned char)*r;
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == ' ' || ch == '.')) {
            return false;
        }
    }
    return true;
}

static void _CheckPlaceholders(const CE_DOC* doc, const unsigned char* modes, CE_LINT* lint)
{
    int emitted = 0;
    for (DocLn line = 0; (line < doc->lines) && (emitted < CE_LINT_HEURISTIC_MAX); ++line) {
        unsigned char const mode = modes[line];
        if ((mode != (unsigned char)CE_LANG_AA) && (mode != (unsigned char)CE_LANG_LUA)) {
            continue;
        }
        DocPos n = 0;
        const char* const lineStart = doc->text + doc->starts[line];
        const char* const p = CeScript_LinePtr(doc, line, &n);
        const char* const end = p + n;
        if ((p >= end) || _IsCommentStart(p, end) || (*p == '[') || (*p == '{')) {
            continue;
        }
        for (const char* r = p; r < end; ++r) {
            char const ch = *r;
            if ((ch == '"') || (ch == '\'')) {
                char const quote = ch;
                ++r;
                while ((r < end) && (*r != quote)) {
                    if ((*r == '\\') && (r + 1 < end)) {
                        ++r;
                    }
                    ++r;
                }
                continue;
            }
            char const closer = (ch == '$') ? '$' : '>';
            if ((ch == '$') ||
                ((ch == '<') && (mode == (unsigned char)CE_LANG_AA) && (r + 1 < end) &&
                 _IsIdentStart(r[1]))) {
                const char* const close = (const char*)memchr(r + 1, closer, (size_t)(end - (r + 1)));
                if (!close || !_IsPlaceholderName(r + 1, close)) {
                    continue;
                }
                char msg[CE_LINT_MSG_CCH];
                StringCchPrintfA(msg, COUNTOF(msg), "占位符未替换: %c%.*s%c",
                                 ch, (int)(close - r - 1), r + 1, closer);
                _AddDiagAt(lint, (int)line + 1, (int)(r - lineStart),
                           (int)(close - r + 1), false, msg);
                ++emitted;
                r = close;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Duplicate allocations.

static void _CheckDuplicates(const CE_DOC* doc, const unsigned char* modes, CE_LINT* lint) {
    if (_DocHasOpenSymbolScope(doc)) {
        return;
    }
    if (doc->lines <= 0) {
        return;
    }
    CE_SECTION secs[CE_MAX_SECTIONS];
    int const secCount = CeScript_CollectSections(doc, secs, CE_MAX_SECTIONS);

    typedef struct _ce_alloc_seen {
        char  name[64];
        int   count;
        int   section;
        DocLn firstLine;
    } CE_ALLOC_SEEN;
    CE_ALLOC_SEEN* const seen = (CE_ALLOC_SEEN*)AllocMem((size_t)doc->lines * sizeof(*seen),
                                                         HEAP_ZERO_MEMORY);
    if (!seen) {
        return;
    }
    int used = 0;
    static const char* const kinds[] = { "alloc", "globalalloc", "sharedalloc" };

    for (DocLn line = 0; line < doc->lines; ++line) {
        if (modes[line] == (unsigned char)CE_LANG_COMMENT) {
            continue;
        }
        DocPos n = 0;
        const char* const p = CeScript_Trimmed(doc, line, &n);
        const char* const end = p + n;
        for (int k = 0; k < (int)COUNTOF(kinds); ++k) {
            size_t const klen = strlen(kinds[k]);
            if (!_StartsWithCI(p, (size_t)(end - p), kinds[k])) {
                continue;
            }
            const char* q = CeScript_SkipBlanks(p + klen, end);
            if (q >= end || *q != '(') {
                continue;
            }
            q = CeScript_SkipBlanks(q + 1, end);
            char name[64] = { '\0' };
            if (CeScript_ReadIdent(q, end, name, COUNTOF(name)) == q) {
                continue;
            }
            int const sec = _SectionOfLine(secs, secCount, line);
            int found = -1;
            for (int i = 0; i < used; ++i) {
                // The same symbol allocated in [ENABLE] and [DISABLE] is the
                // normal mirror pair, not a duplicate.
                if ((seen[i].section == sec) && (_stricmp(seen[i].name, name) == 0)) {
                    found = i;
                    break;
                }
            }
            if ((found < 0) && (used < doc->lines)) {
                found = used++;
                StringCchCopyA(seen[found].name, COUNTOF(seen[found].name), name);
                seen[found].section = sec;
                seen[found].firstLine = line;
            }
            if (found >= 0) {
                ++seen[found].count;
            }
            break;
        }
    }

    for (int i = 0; i < used; ++i) {
        if (seen[i].count > 1) {
            char msg[CE_LINT_MSG_CCH];
            StringCchPrintfA(msg, COUNTOF(msg), "%s 在同一段中重复 alloc（%d 次）",
                             seen[i].name, seen[i].count);
            _AddDiag(lint, (int)seen[i].firstLine + 1, false, msg);
        }
    }
    FreeMem(seen);
}

// ----------------------------------------------------------------------------
// Unknown Auto Assembler statements.

static bool _IsSingleIdentifier(const char* p, const char* end, char* out, size_t outCch) {
    const char* const after = CeScript_ReadIdent(p, end, out, outCch);
    if (after == p) {
        return false;
    }
    return CeScript_SkipBlanks(after, end) >= end;
}

static void _CheckAaStatements(const CE_DOC* doc, const unsigned char* modes, const CE_DEF* defs, int defCount, CE_LINT* lint) {
    bool const openScope = _DocHasOpenSymbolScope(doc);
    if (openScope) {
        return;
    }
    for (DocLn line = 0; line < doc->lines; ++line) {
        if (modes[line] != (unsigned char)CE_LANG_AA) {
            continue;
        }
        DocPos n = 0;
        const char* const lineStart = doc->text + doc->starts[line];
        const char* const p = CeScript_Trimmed(doc, line, &n);
        const char* const end = p + n;
        if ((p >= end) || _IsCommentStart(p, end) || *p == '[' || *p == '{') {
            continue;
        }
        char word[64] = { '\0' };
        if (!_IsSingleIdentifier(p, end, word, COUNTOF(word))) {
            continue;
        }
        if (strlen(word) < 3) {
            continue;
        }
        if (_InKeywordList(CE_KEYWORDS_AA, word) || _InKeywordList(CE_KEYWORDS_ASM, word) ||
            _InKeywordList(CE_KEYWORDS_TYPES, word) || _InKeywordList(CE_KEYWORDS_LUA, word) ||
            _IsCeBuiltinSymbol(word) || _IsKnownAaCommand(word) ||
            CeCompletion_IsBuiltinKeyword(word)) {
            continue;
        }
        if (CeScript_FindDef(defs, defCount, word) >= 0) {
            continue;
        }
        char msg[CE_LINT_MSG_CCH];
        StringCchPrintfA(msg, COUNTOF(msg), "未知指令或语句: %s", word);
        _AddDiagAt(lint, (int)line + 1, (int)(p - lineStart), (int)strlen(word), false, msg);
    }
}

// ----------------------------------------------------------------------------
// Static analysis helpers.
//
// These checks answer "is this script plausible / executable" without running
// the script.  Every finding is a warning: a heuristic must never turn a valid
// CE script into a hard error, and only errors get inline annotations.

typedef struct _arg_span {
    const char* p;
    const char* end;
} ARG_SPAN;

// Split a comma separated argument list on top level commas.  Quoted strings
// and nested (), [], {} keep their commas out of the split.
static int _SplitArgs(const char* p, const char* end, ARG_SPAN* out, int max)
{
    int count = 0;
    int depth = 0;
    char quote = '\0';
    const char* start = p;
    for (const char* r = p; r <= end; ++r) {
        bool const atEnd = (r == end);
        if (!atEnd) {
            char const ch = *r;
            if (quote) {
                if (ch == quote) {
                    quote = '\0';
                }
                continue;
            }
            if ((ch == '"') || (ch == '\'')) {
                quote = ch;
                continue;
            }
            if ((ch == '(') || (ch == '[') || (ch == '{')) {
                ++depth;
                continue;
            }
            if ((ch == ')') || (ch == ']') || (ch == '}')) {
                if (depth > 0) {
                    --depth;
                }
                continue;
            }
            if ((ch != ',') || (depth != 0)) {
                continue;
            }
        }
        if (count < max) {
            const char* s = start;
            const char* e = r;
            while ((s < e) && ((*s == ' ') || (*s == '\t'))) {
                ++s;
            }
            while ((e > s) && ((e[-1] == ' ') || (e[-1] == '\t'))) {
                --e;
            }
            out[count].p = s;
            out[count].end = e;
            ++count;
        }
        start = r + 1;
        if (atEnd) {
            break;
        }
    }
    return count;
}

// CE AA sizes are decimal, 0x-hex, $hex or 123h literals.  A value must start
// with a digit after its sign/prefix: "ecx" is a register, not the hex number
// "ec" followed by "x", and treating it as one hid real operand errors.
static bool _SpanIsNumeric(const ARG_SPAN* a)
{
    const char* p = a->p;
    const char* const end = a->end;
    if (p >= end) {
        return false;
    }
    if ((*p == '+') || (*p == '-') || (*p == '~')) {
        ++p;
    }
    if ((p < end) && (*p == '(')) {
        ++p;
    }
    if (p >= end) {
        return false;
    }
    if (*p == '$') {
        ++p;
    }
    else if (((p + 1) < end) && (p[0] == '0') && ((p[1] == 'x') || (p[1] == 'X'))) {
        p += 2;
    }
    if (p >= end) {
        return false;
    }
    if ((*p < '0') || (*p > '9')) {
        return false; // identifiers (registers, symbols) are never numbers
    }
    for (; p < end; ++p) {
        unsigned char const ch = (unsigned char)*p;
        bool const digit = (ch >= '0') && (ch <= '9');
        bool const hex = ((ch >= 'a') && (ch <= 'f')) || ((ch >= 'A') && (ch <= 'F'));
        bool const allowed = (ch == '.') || (ch == '_') || (ch == 'h') || (ch == 'H') || (ch == ')');
        if (!(digit || hex || allowed)) {
            return false;
        }
    }
    return true;
}

// Read a plain identifier that occupies the whole span.
static bool _SpanIsPlainIdent(const ARG_SPAN* a, char* out, size_t outCch)
{
    const char* const after = CeScript_ReadIdent(a->p, a->end, out, outCch);
    return (after != a->p) && (after == a->end);
}

static bool _IsKnownBareOperand(const char* name)
{
    // Registers, mnemonics, size specifiers and CE object names are all valid
    // bare identifiers inside an AA operand.
    if (_InKeywordList(CE_KEYWORDS_ASM, name) ||
        _InKeywordList(CE_KEYWORDS_AA, name) ||
        _InKeywordList(CE_KEYWORDS_TYPES, name) ||
        _InKeywordList(CE_KEYWORDS_LUA, name)) {
        return true;
    }
    // AVX-512 mask registers (k0-k7) and MPX bounds (bnd0-bnd3) are not part of
    // the generated keyword set but are ordinary operands.
    size_t const len = strlen(name);
    char const c0 = (char)_Lower(name[0]);
    if ((len == 2) && (c0 == 'k') && (name[1] >= '0') && (name[1] <= '7')) {
        return true;
    }
    if ((len == 4) && _StartsWithCI(name, len, "bnd") && (name[3] >= '0') && (name[3] <= '3')) {
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Assembly instruction shape: operand count, immediate destinations and
// unknown bare operands.  Only lines starting with a known x86 mnemonic are
// inspected, so AA commands, labels and directives are never misread.

typedef struct _asm_rule {
    const char* op;
    int minArgs;
    int maxArgs;
} ASM_RULE;

static const ASM_RULE s_asmRules[] = {
    { "mov", 2, 2 }, { "movabs", 2, 2 }, { "movzx", 2, 2 }, { "movsx", 2, 2 },
    { "movsxd", 2, 2 }, { "movbe", 2, 2 }, { "lea", 2, 2 }, { "xchg", 2, 2 },
    { "add", 2, 2 }, { "sub", 2, 2 }, { "adc", 2, 2 }, { "sbb", 2, 2 },
    { "cmp", 2, 2 }, { "test", 2, 2 }, { "and", 2, 2 }, { "or", 2, 2 },
    { "xor", 2, 2 }, { "shl", 2, 2 }, { "shr", 2, 2 }, { "sal", 2, 2 },
    { "sar", 2, 2 }, { "bt", 2, 2 }, { "bts", 2, 2 }, { "btr", 2, 2 },
    { "btc", 2, 2 }, { "imul", 1, 3 }, { "shld", 3, 3 }, { "shrd", 3, 3 },
    { "push", 1, 1 }, { "pop", 1, 1 }, { "inc", 1, 1 }, { "dec", 1, 1 },
    { "neg", 1, 1 }, { "not", 1, 1 }, { "mul", 1, 1 }, { "div", 1, 1 },
    { "idiv", 1, 1 }, { "call", 1, 1 }, { "jmp", 1, 1 }, { "int", 1, 1 },
    { "ret", 0, 1 }, { "retn", 0, 1 }, { "retf", 0, 1 },
    // "nop 2" is CE's repeat count for the nop mnemonic, not an operand error.
    { "nop", 0, 1 }, { "leave", 0, 0 }, { "pushfq", 0, 0 }, { "popfq", 0, 0 },
};

static bool _AsmOperandsExpected(const char* op, int* minArgs, int* maxArgs)
{
    for (int i = 0; i < (int)COUNTOF(s_asmRules); ++i) {
        if (_stricmp(op, s_asmRules[i].op) == 0) {
            *minArgs = s_asmRules[i].minArgs;
            *maxArgs = s_asmRules[i].maxArgs;
            return true;
        }
    }
    if (_InKeywordList(CE_KEYWORDS_ASM, op)) {
        if ((op[0] == 'j') || (op[0] == 'J')) {
            // Jcc / jecxz / jrcxz, with an optional short/near/far hint.
            *minArgs = 1;
            *maxArgs = 2;
            return true;
        }
        size_t const len = strlen(op);
        if (_StartsWithCI(op, len, "set")) {
            *minArgs = 1;
            *maxArgs = 1;
            return true;
        }
        if (_StartsWithCI(op, len, "cmov")) {
            *minArgs = 2;
            *maxArgs = 2;
            return true;
        }
    }
    return false;
}

static void _CheckAsmInstructions(const CE_DOC* doc, const unsigned char* modes,
                                  const CE_DEF* defs, int defCount, CE_LINT* lint)
{
    int emitted = 0;
    bool const openScope = _DocHasOpenSymbolScope(doc);
    for (DocLn line = 0; (line < doc->lines) && (emitted < CE_LINT_HEURISTIC_MAX); ++line) {
        if (modes[line] != (unsigned char)CE_LANG_AA) {
            continue;
        }
        DocPos n = 0;
        const char* const lineStart = doc->text + doc->starts[line];
        const char* p = CeScript_Trimmed(doc, line, &n);
        const char* const end = p + n;
        if ((p >= end) || _IsCommentStart(p, end) || (*p == '[') || (*p == '{') || (*p == '#')) {
            continue;
        }
        if (_IsLabelDefinitionLine(p, end)) {
            continue; // "Namespace.Class:Method+16f:" and friends
        }
        char op[32] = { '\0' };
        const char* const afterOp = CeScript_ReadIdent(p, end, op, COUNTOF(op));
        if (afterOp == p) {
            continue;
        }
        const char* q = CeScript_SkipBlanks(afterOp, end);
        int minArgs = 0;
        int maxArgs = 0;
        if (!_AsmOperandsExpected(op, &minArgs, &maxArgs)) {
            // Not a mnemonic we know.  A line with operands is either a typo or
            // an instruction of a newer CE build; a bare statement is handled by
            // _CheckAaStatements.  Labels ("name:"), assignments ("name = ...")
            // and known AA/type/Lua words are never instructions.
            bool const hasOperands = (q < end) && !_IsCommentStart(q, end) &&
                                      (*q != ':') && (*q != '=') && (*q != ',');
            if (hasOperands && !openScope && (defCount < CE_MAX_DEFS) &&
                !_InKeywordList(CE_KEYWORDS_AA, op) && !_InKeywordList(CE_KEYWORDS_TYPES, op) &&
                !_InKeywordList(CE_KEYWORDS_LUA, op) && !_IsCeBuiltinSymbol(op) &&
                !_IsKnownAaCommand(op) && !CeCompletion_IsBuiltinKeyword(op) &&
                (CeScript_FindDef(defs, defCount, op) < 0)) {
                char msg[CE_LINT_MSG_CCH];
                StringCchPrintfA(msg, COUNTOF(msg), "未知指令: %s", op);
                _AddDiagAt(lint, (int)line + 1, (int)(p - lineStart), (int)strlen(op), false, msg);
                ++emitted;
            }
            continue;
        }
        const char* argsEnd = end;
        for (const char* r = q; r < end; ++r) {
            if ((*r == ';') || ((*r == '/') && (r + 1 < end) && (r[1] == '/'))) {
                argsEnd = r;
                break;
            }
        }
        ARG_SPAN spans[8] = { 0 };
        int argc = _SplitArgs(q, argsEnd, spans, COUNTOF(spans));
        if ((argc == 1) && (spans[0].p >= spans[0].end)) {
            argc = 0;
        }
        int const opCol = (int)(p - lineStart);
        int const opLen = (int)strlen(op);
        char msg[CE_LINT_MSG_CCH];
        if ((argc < minArgs) || (argc > maxArgs)) {
            StringCchPrintfA(msg, COUNTOF(msg), "%s 需要 %d 个操作数，当前 %d", op, minArgs, argc);
            _AddDiagAt(lint, (int)line + 1, opCol, opLen, false, msg);
            ++emitted;
        }
        if ((argc >= 1) && (minArgs >= 2) && _SpanIsNumeric(&spans[0])) {
            StringCchPrintfA(msg, COUNTOF(msg), "%s 的目标操作数不能是立即数", op);
            _AddDiagAt(lint, (int)line + 1, opCol, opLen, false, msg);
            ++emitted;
        }
        for (int i = 0; (i < argc) && (emitted < CE_LINT_HEURISTIC_MAX); ++i) {
            const ARG_SPAN* const a = &spans[i];
            int const argCol = (int)(a->p - lineStart);
            int const argLen = (int)(a->end - a->p);
            if (a->p >= a->end) {
                StringCchPrintfA(msg, COUNTOF(msg), "%s 有空操作数", op);
                _AddDiagAt(lint, (int)line + 1, opCol, opLen, false, msg);
                ++emitted;
                continue;
            }
            int depth = 0;
            bool badBrackets = false;
            for (const char* r = a->p; r < a->end; ++r) {
                if (*r == '[') {
                    ++depth;
                }
                else if ((*r == ']') && (--depth < 0)) {
                    badBrackets = true;
                    break;
                }
            }
            if (badBrackets || (depth != 0)) {
                StringCchPrintfA(msg, COUNTOF(msg), "%s 的操作数中 [] 不匹配", op);
                _AddDiagAt(lint, (int)line + 1, argCol, argLen, false, msg);
                ++emitted;
                continue;
            }
            char name[64] = { '\0' };
            // When the definition table is full, or the script pulls in
            // external symbols, it is no longer authoritative.
            if (openScope || (defCount >= CE_MAX_DEFS) ||
                !_SpanIsPlainIdent(a, name, COUNTOF(name)) ||
                _SpanIsNumeric(a) || _IsKnownBareOperand(name) || _IsCeBuiltinSymbol(name) ||
                (CeScript_FindDef(defs, defCount, name) >= 0) || (strlen(name) > 24)) {
                continue;
            }
            StringCchPrintfA(msg, COUNTOF(msg), "未知变量或标签: %s", name);
            _AddDiagAt(lint, (int)line + 1, argCol, argLen, false, msg);
            ++emitted;
        }
    }
}

// ----------------------------------------------------------------------------
// Lua / CE-API static checks: declaration collection, unknown calls, argument
// counts and statement logic.  CE API names come from the generated catalogue,
// so this stays in sync with the completion data.

// Lua 5.1 - 5.4 standard globals plus the CE engine names that are not part of
// the generated keyword lists.  Library members (math.floor, package.loaded,
// string.format, ...) never need an entry: a name after '.'/':' is treated as
// a member and is not resolved here.
static const char* const s_luaStdGlobals[] = {
    // base library
    "assert", "collectgarbage", "dofile", "error", "getmetatable", "ipairs",
    "load", "loadfile", "loadstring", "module", "next", "pairs", "pcall",
    "print", "rawequal", "rawget", "rawlen", "rawset", "require", "select",
    "setmetatable", "tonumber", "tostring", "type", "unpack", "xpcall",
    "warn", "getfenv", "setfenv", "newproxy", "gcinfo",
    // libraries / package management
    "coroutine", "debug", "io", "math", "os", "package", "string", "table",
    "utf8", "bit", "bit32", "jit",
    // environment, arguments and varargs
    "_G", "_VERSION", "_ENV", "self", "arg",
    // CE callbacks are registered by the engine and need no local definition.
    "onOpenProcess", "onCloseProcess", "onAttach", "onDetach", "onDisconnect",
    "onTimer", "onHotkey", "onClick", "onClose", "onChange", "onActivate",
    "onDeactivate", "onDestroy", "onPaint", "onMouseMove", "onExecute",
    "onError", "onPostProcess",
    // CE Lua functions that the generated keyword set does not list yet.
    "registerClass", "objectDestroy", "copyTable", "createAddressList",
    "loadImage", "tableToString", "getModuleBase",
};

static bool _IsKnownLuaName(const char* name, DocPos pos, const CE_LUA_MODEL* model,
                            const CE_DEF* defs, int defCount)
{
    if (_InKeywordList(CE_KEYWORDS_LUA, name) || _InKeywordList(CE_KEYWORDS_API, name) ||
        _InKeywordList(CE_KEYWORDS_TYPES, name) || _InKeywordList(CE_KEYWORDS_AA, name) ||
        _InKeywordList(CE_KEYWORDS_ASM, name)) {
        return true;
    }
    for (int i = 0; i < (int)COUNTOF(s_luaStdGlobals); ++i) {
        if (_stricmp(name, s_luaStdGlobals[i]) == 0) {
            return true;
        }
    }
    // Resolve against the real lexical scope first.  A saturated model cannot
    // prove a name is undefined, so the document-wide defs are a safe fallback.
    if (model && !model->truncated &&
        (CeLuaScope_Resolve(model, name, pos) != CE_LUA_SYM_NONE)) {
        return true;
    }
    return (CeScript_FindDef(defs, defCount, name) >= 0);
}

// Count top level arguments of a call whose '(' is at |p|; -1 when unclosed.
static int _CountCallArgs(const char* p, const char* end)
{
    if ((p >= end) || (*p != '(')) {
        return -1;
    }
    int depth = 1;
    int commas = 0;
    bool hasText = false;
    char quote = '\0';
    for (const char* r = p + 1; r < end; ++r) {
        char const ch = *r;
        if (quote) {
            if (ch == quote) {
                quote = '\0';
            }
            hasText = true;
            continue;
        }
        if ((ch == '"') || (ch == '\'')) {
            quote = ch;
            hasText = true;
            continue;
        }
        if ((ch == '(') || (ch == '[') || (ch == '{')) {
            ++depth;
            hasText = true;
            continue;
        }
        if (ch == ')') {
            if (depth == 1) {
                return hasText ? (commas + 1) : 0;
            }
            --depth;
            continue;
        }
        if ((ch == ']') || (ch == '}')) {
            --depth;
            continue;
        }
        if ((ch == ',') && (depth == 1)) {
            ++commas;
            continue;
        }
        if ((ch != ' ') && (ch != '\t')) {
            hasText = true;
        }
    }
    return -1;
}

typedef struct _lua_arity {
    const char* name;
    int minArgs;
} LUA_ARITY;

static const LUA_ARITY s_luaArity[] = {
    { "readInteger", 1 }, { "readQword", 1 }, { "readDword", 1 }, { "readWord", 1 },
    { "readByte", 1 }, { "readBytes", 1 }, { "readFloat", 1 }, { "readDouble", 1 },
    { "readString", 1 }, { "readPointer", 1 }, { "readSmallInteger", 1 },
    { "writeInteger", 2 }, { "writeQword", 2 }, { "writeDword", 2 }, { "writeWord", 2 },
    { "writeByte", 2 }, { "writeBytes", 2 }, { "writeFloat", 2 }, { "writeDouble", 2 },
    { "writeString", 2 }, { "writeSmallInteger", 2 },
    { "getAddress", 1 }, { "getAddressSafe", 1 }, { "getNameFromAddress", 1 },
    { "sleep", 1 }, { "showMessage", 1 }, { "messageDialog", 2 }, { "inputQuery", 3 },
    { "autoAssemble", 1 }, { "AOBScan", 1 }, { "AOBScanModule", 2 },
    { "AOBScanUnique", 1 }, { "openProcess", 1 }, { "getProcessIDFromProcessName", 1 },
    { "tostring", 1 }, { "tonumber", 1 }, { "type", 1 }, { "pairs", 1 },
    { "ipairs", 1 }, { "select", 1 }, { "setmetatable", 2 }, { "string.format", 1 },
};

static int _LuaMinArgs(const char* name)
{
    for (int i = 0; i < (int)COUNTOF(s_luaArity); ++i) {
        if (_stricmp(name, s_luaArity[i].name) == 0) {
            return s_luaArity[i].minArgs;
        }
    }
    return -1;
}

static bool _LuaLineHasWord(const char* p, const char* end, const char* word)
{
    size_t const len = strlen(word);
    for (const char* r = p; (r + len) <= end; ++r) {
        if (_EqualsCI(r, len, word) &&
            ((r == p) || !_IsIdentChar(r[-1])) &&
            ((r + len == end) || !_IsIdentChar(r[len]))) {
            return true;
        }
    }
    return false;
}

static void _CheckLuaStatics(const CE_DOC* doc, const unsigned char* modes,
                             const CE_DEF* defs, int defCount, CE_LINT* lint)
{
    // A saturated definition table (or a script that pulls in external code)
    // is not authoritative: stay silent instead of reporting valid symbols.
    bool const defsTruncated = (defCount >= CE_MAX_DEFS);
    bool const openScope = _DocHasOpenSymbolScope(doc);
    // One shared token/scope pass replaces the old document-wide flat symbol
    // list.  It understands local/parameter/global/function declarations,
    // table keys, dotted function names and multi-line signatures, and knows
    // which block each declaration belongs to.
    CE_LUA_MODEL* const model = AllocMem(sizeof(CE_LUA_MODEL), HEAP_ZERO_MEMORY);
    if (!model) {
        return;
    }
    CeLuaScope_Init(model);
    for (DocLn line = 0; line < doc->lines; ++line) {
        if (modes[line] == (unsigned char)CE_LANG_LUA) {
            DocPos n = 0;
            const char* const p = CeScript_LinePtr(doc, line, &n);
            CeLuaScope_Line(model, p, p + n, doc->starts[line], line + 1);
        }
    }
    CeLuaScope_Finish(model, doc->len);

    int emitted = 0;
    int blockDepth = 0;
    for (DocLn line = 0; (line < doc->lines) && (emitted < CE_LINT_HEURISTIC_MAX); ++line) {
        if (modes[line] != (unsigned char)CE_LANG_LUA) {
            continue;
        }
        DocPos n = 0;
        const char* const lineStart = CeScript_LinePtr(doc, line, &n);
        const char* p = lineStart;
        const char* const end = p + n;
        char msg[CE_LINT_MSG_CCH];

        DocPos tn = 0;
        const char* const trimmed = CeScript_Trimmed(doc, line, &tn);
        // A whole-line comment is not Lua code.  In CE scripts the header
        // before [ENABLE] is parsed as Lua, where "//" is an operator, so a
        // "//" comment line must be skipped before the identifier scan below
        // (which otherwise reports every word in it as an undefined symbol).
        if (_IsCommentStart(trimmed, trimmed + tn)) {
            continue;
        }
        if (trimmed < trimmed + tn) {
            char first[32] = { '\0' };
            const char* const afterFirst = CeScript_ReadIdent(trimmed, trimmed + tn, first, COUNTOF(first));
            bool const isConditional = (afterFirst != trimmed) &&
                ((_stricmp(first, "if") == 0) || (_stricmp(first, "elseif") == 0));
            if (isConditional && !_LuaLineHasWord(trimmed, trimmed + tn, "then")) {
                _AddDiagAt(lint, (int)line + 1, (int)(trimmed - p), (int)strlen(first),
                           true, "if/elseif 缺少 then");
                ++emitted;
            }
            bool const closesBlock = (afterFirst != trimmed) &&
                ((_stricmp(first, "else") == 0) || (_stricmp(first, "end") == 0) ||
                 (_stricmp(first, "until") == 0));
            if (closesBlock && (blockDepth <= 0)) {
                _AddDiagAt(lint, (int)line + 1, (int)(trimmed - p), (int)strlen(first),
                           true, "多余的 else/end/until");
                ++emitted;
            }
            // Keep a coarse block depth for the orphaned else/end check.  A
            // "do" belonging to for/while must not open a second block.
            bool hasLoop = false;
            for (const char* r = trimmed; r < trimmed + tn; ++r) {
                if (!_IsIdentStart(*r)) {
                    continue;
                }
                const char* const s = r;
                while ((r < trimmed + tn) && _IsIdentChar(*r)) {
                    ++r;
                }
                size_t const len = (size_t)(r - s);
                if (_EqualsCI(s, len, "for") || _EqualsCI(s, len, "while")) {
                    hasLoop = true;
                    ++blockDepth;
                }
                else if (_EqualsCI(s, len, "function") || _EqualsCI(s, len, "if") ||
                         _EqualsCI(s, len, "repeat")) {
                    ++blockDepth;
                }
                else if (_EqualsCI(s, len, "do") && !hasLoop) {
                    ++blockDepth;
                }
                else if (_EqualsCI(s, len, "end") || _EqualsCI(s, len, "until")) {
                    --blockDepth;
                }
            }
        }

        bool afterGoto = false;
        while ((p < end) && (emitted < CE_LINT_HEURISTIC_MAX)) {
            if ((p[0] == '-') && (p + 1 < end) && (p[1] == '-')) {
                break;
            }
            if ((p[0] == '{') && (p + 1 < end) && (p[1] == '$')) {
                break; // {$lua}/{$asm} directive is not Lua code
            }
            if ((p[0] == ':') && (p + 1 < end) && (p[1] == ':')) {
                p += 2; // goto label
                afterGoto = true;
                continue;
            }
            if ((p[0] == '[') && (p + 1 < end) && (p[1] == '[')) {
                p += 2; // Lua long string
                while ((p + 1 < end) && !((p[0] == ']') && (p[1] == ']'))) {
                    ++p;
                }
                p = (p + 1 < end) ? (p + 2) : end;
                continue;
            }
            if ((p[0] == '"') || (p[0] == '\'')) {
                char const quote = *p++;
                while ((p < end) && (*p != quote)) {
                    if ((*p == '\\') && (p + 1 < end)) {
                        ++p;
                    }
                    ++p;
                }
                if ((p < end) && (*p == quote)) {
                    ++p;
                }
                continue;
            }
            if ((*p >= '0') && (*p <= '9')) {
                while ((p < end) && (_IsIdentChar(*p) || (*p == '.'))) {
                    ++p; // numeric literal: 0x1F, 1.5, 1e3
                }
                continue;
            }
            if (!_IsIdentStart(*p)) {
                ++p;
                continue;
            }
            const char* const word = p;
            while ((p < end) && _IsIdentChar(*p)) {
                ++p;
            }
            size_t const len = (size_t)(p - word);
            if ((len == 0) || (len >= 64)) {
                continue;
            }
            char name[64] = { '\0' };
            memcpy(name, word, len);
            name[len] = '\0';
            if (afterGoto) {
                afterGoto = false; // the identifier is a label, not a variable
                continue;
            }
            if (_stricmp(name, "goto") == 0) {
                afterGoto = true;
                continue;
            }
            bool const member = (word > doc->text) && ((word[-1] == '.') || (word[-1] == ':'));
            const char* const after = CeScript_SkipBlanks(p, end);
            bool const isCall = (after < end) && ((*after == '(') || (*after == '{'));
            bool const isAssign = (after < end) && (*after == '=') &&
                                  ((after + 1 >= end) || (after[1] != '='));
            // "[key] = value" inside a table constructor: the key is a field,
            // not an unresolved variable of the enclosing scope.
            const char* before = word;
            while ((before > lineStart) && ((before[-1] == ' ') || (before[-1] == '\t'))) {
                --before;
            }
            bool const tableKey = (before > lineStart) && (before[-1] == '[') &&
                (after < end) && (*after == ']') &&
                (((after + 1) >= end) || (after[1] != ']'));
            if (tableKey) {
                const char* const q = CeScript_SkipBlanks(after + 1, end);
                if ((q < end) && (*q == '=') && (((q + 1) >= end) || (q[1] != '='))) {
                    continue;
                }
            }
            DocPos const wordPos = doc->starts[line] + (word - lineStart);
            if (member || isAssign || _IsKnownLuaName(name, wordPos, model, defs, defCount)) {
                if (isCall && !member && (*after == '(')) {
                    int const minArgs = _LuaMinArgs(name);
                    if (minArgs > 0) {
                        int const argc = _CountCallArgs(after, end);
                        if ((argc >= 0) && (argc < minArgs)) {
                            StringCchPrintfA(msg, COUNTOF(msg),
                                "%s() 至少需要 %d 个参数，当前 %d", name, minArgs, argc);
                            _AddDiagAt(lint, (int)line + 1, (int)(word - lineStart), (int)len, false, msg);
                            ++emitted;
                        }
                    }
                }
                continue;
            }
            if (defsTruncated || openScope || model->truncated) {
                continue; // cannot prove a symbol is undefined
            }
            if (isCall) {
                StringCchPrintfA(msg, COUNTOF(msg), "未知函数: %s", name);
            }
            else {
                StringCchPrintfA(msg, COUNTOF(msg), "未定义的变量: %s", name);
            }
            _AddDiagAt(lint, (int)line + 1, (int)(word - lineStart), (int)len, false, msg);
            ++emitted;
        }
    }
    FreeMem(model);
}

// ----------------------------------------------------------------------------
// AA command argument sanity: empty calls, missing sizes, argument counts,
// non-numeric sizes and references to undefined symbols.

typedef struct _aa_cmd {
    const char* name;
    int minArgs;
    int maxArgs;
    bool needsSymbol;
} AA_CMD;

static const AA_CMD s_aaCmds[] = {
    { "alloc", 2, 3, false }, { "globalalloc", 2, 3, false }, { "sharedalloc", 2, 3, false },
    { "dealloc", 1, 1, true }, { "label", 1, 1, false },
    { "registersymbol", 1, 1, true }, { "unregistersymbol", 1, 1, true },
    { "define", 2, 2, false }, { "aobscan", 2, 4, false },
    { "aobscanmodule", 3, 6, false }, { "aobscanregion", 4, 7, false },
    { "fullaccess", 2, 2, false }, { "assert", 2, 2, false },
    { "createthread", 1, 2, false }, { "createthreadandwait", 1, 2, false },
    { "loadlibrary", 1, 2, false }, { "include", 1, 1, false },
    { "luacall", 1, 8, false }, { "readmem", 2, 2, false },
    { "loadbinary", 2, 2, false }, { "kalloc", 1, 2, false },
    { "reassemble", 1, 1, false }, { "savenop", 1, 2, false },
    { "exports", 1, 8, false }, { "unprotect", 1, 8, false },
    { "createthreadandwaitfinish", 1, 2, false },
    // CE 7.7+ hook/unhook: hook(address, newmem, originalcode) / unhook(address).
    // The pair is only emitted by scripts that locate the target with aobscan*;
    // elsewhere a lone hook(...) is valid, so no pairing requirement is enforced.
    { "hook", 3, 3, false }, { "unhook", 1, 1, true },
};

static bool _IsKnownAaCommand(const char* name) {
    for (int i = 0; i < (int)COUNTOF(s_aaCmds); ++i) {
        if (_stricmp(name, s_aaCmds[i].name) == 0) {
            return true;
        }
    }
    return false;
}

static void _CheckAaCallArgs(const CE_DOC* doc, const unsigned char* modes,
                             const CE_DEF* defs, int defCount, CE_LINT* lint) {
    bool const defsTruncated = (defCount >= CE_MAX_DEFS);
    bool const openScope = _DocHasOpenSymbolScope(doc);
    int emitted = 0;
    for (DocLn line = 0; (line < doc->lines) && (emitted < CE_LINT_HEURISTIC_MAX); ++line) {
        if (modes[line] != (unsigned char)CE_LANG_AA) {
            continue;
        }
        DocPos n = 0;
        const char* const lineStart = doc->text + doc->starts[line];
        const char* const p = CeScript_Trimmed(doc, line, &n);
        const char* const end = p + n;
        if (_IsCommentStart(p, end)) {
            continue;
        }
        char op[32] = { '\0' };
        const char* const afterOp = CeScript_ReadIdent(p, end, op, COUNTOF(op));
        if (afterOp == p) {
            continue;
        }
        int cmdIndex = -1;
        for (int i = 0; i < (int)COUNTOF(s_aaCmds); ++i) {
            if (_stricmp(op, s_aaCmds[i].name) == 0) {
                cmdIndex = i;
                break;
            }
        }
        if (cmdIndex < 0) {
            continue;
        }
        const AA_CMD* const cmd = &s_aaCmds[cmdIndex];
        const char* q = CeScript_SkipBlanks(afterOp, end);
        if ((q >= end) || (*q != '(')) {
            continue;
        }
        const char* const argsStart = q + 1;
        const char* argsEnd = end;
        int depth = 1;
        char quote = '\0';
        for (const char* r = argsStart; r < end; ++r) {
            if (quote) {
                if (*r == quote) {
                    quote = '\0';
                }
                continue;
            }
            if ((*r == '"') || (*r == '\'')) {
                quote = *r;
                continue;
            }
            if ((*r == '(') || (*r == '[') || (*r == '{')) {
                ++depth;
                continue;
            }
            if (*r == ')') {
                if (depth == 1) {
                    argsEnd = r;
                    break;
                }
                --depth;
                continue;
            }
            if ((*r == ']') || (*r == '}')) {
                --depth;
            }
        }
        ARG_SPAN spans[8] = { 0 };
        int argc = _SplitArgs(argsStart, argsEnd, spans, COUNTOF(spans));
        if ((argc == 1) && (spans[0].p >= spans[0].end)) {
            argc = 0;
        }
        int const opCol = (int)(p - lineStart);
        int const opLen = (int)strlen(cmd->name);
        char msg[CE_LINT_MSG_CCH];
        if (argc == 0) {
            StringCchPrintfA(msg, COUNTOF(msg), "%s() 至少需要一个参数", cmd->name);
            _AddDiagAt(lint, (int)line + 1, opCol, opLen, false, msg);
            ++emitted;
        }
        else if ((cmdIndex < 3) && (argc < 2)) {
            StringCchPrintfA(msg, COUNTOF(msg), "%s(名称, 大小) 缺少大小参数", cmd->name);
            _AddDiagAt(lint, (int)line + 1, opCol, opLen, false, msg);
            ++emitted;
        }
        else if ((argc < cmd->minArgs) || (argc > cmd->maxArgs)) {
            if (cmd->maxArgs >= 8) {
                StringCchPrintfA(msg, COUNTOF(msg), "%s() 至少需要 %d 个参数，当前 %d",
                    cmd->name, cmd->minArgs, argc);
            }
            else {
                StringCchPrintfA(msg, COUNTOF(msg), "%s() 需要 %d 个参数，当前 %d",
                    cmd->name, cmd->minArgs, argc);
            }
            _AddDiagAt(lint, (int)line + 1, opCol, opLen, false, msg);
            ++emitted;
        }
        // Static "type" check: the size of an allocation / readmem call must be
        // a literal or a symbol declared in the script.
        if (!defsTruncated && !openScope &&
            ((cmdIndex < 3) || (_stricmp(cmd->name, "readmem") == 0)) && (argc >= 2)) {
            char sym[64] = { '\0' };
            if (!_SpanIsNumeric(&spans[1]) && _SpanIsPlainIdent(&spans[1], sym, COUNTOF(sym)) &&
                !_IsKnownBareOperand(sym) && !_IsCeBuiltinSymbol(sym) &&
                (CeScript_FindDef(defs, defCount, sym) < 0)) {
                StringCchPrintfA(msg, COUNTOF(msg),
                    "%s() 的大小 '%s' 既不是数字也不是已定义符号", cmd->name, sym);
                _AddDiagAt(lint, (int)line + 1, (int)(spans[1].p - lineStart),
                           (int)(spans[1].end - spans[1].p), false, msg);
                ++emitted;
            }
        }
        // Static variable check: dealloc / (un)registersymbol need a symbol that
        // is actually declared somewhere in the script.
        if (!defsTruncated && !openScope && cmd->needsSymbol && (argc >= 1)) {
            char sym[64] = { '\0' };
            if (_SpanIsPlainIdent(&spans[0], sym, COUNTOF(sym)) && !_IsCeBuiltinSymbol(sym) &&
                (CeScript_FindDef(defs, defCount, sym) < 0)) {
                StringCchPrintfA(msg, COUNTOF(msg), "%s(%s) 引用了未定义的符号", cmd->name, sym);
                _AddDiagAt(lint, (int)line + 1, (int)(spans[0].p - lineStart),
                           (int)(spans[0].end - spans[0].p), false, msg);
                ++emitted;
            }
        }
    }
}

// ----------------------------------------------------------------------------

static int __cdecl _CompareDiag(const void* a, const void* b) {
    const CE_DIAG* const da = (const CE_DIAG*)a;
    const CE_DIAG* const db = (const CE_DIAG*)b;
    if (da->line != db->line) {
        return (da->line < db->line) ? -1 : 1;
    }
    if (da->error != db->error) {
        return da->error ? -1 : 1;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// "Will this actually work?" heuristics.  The structural checks above answer
// "is the script well formed"; these answer "will the assembler / Lua runtime
// accept this particular line".  Everything here is a warning.
// ----------------------------------------------------------------------------

static int _AsmRegWidth(const char* name)
{
    static const char* const s_rows[][4] = {
        { "rax",  "eax",  "ax",   "al"   }, { "rbx",  "ebx",  "bx",   "bl"   },
        { "rcx",  "ecx",  "cx",   "cl"   }, { "rdx",  "edx",  "dx",   "dl"   },
        { "rsi",  "esi",  "si",   "sil"  }, { "rdi",  "edi",  "di",   "dil"  },
        { "rbp",  "ebp",  "bp",   "bpl"  }, { "rsp",  "esp",  "sp",   "spl"  },
        { "r8",   "r8d",  "r8w",  "r8b"  }, { "r9",   "r9d",  "r9w",  "r9b"  },
        { "r10",  "r10d", "r10w", "r10b" }, { "r11",  "r11d", "r11w", "r11b" },
        { "r12",  "r12d", "r12w", "r12b" }, { "r13",  "r13d", "r13w", "r13b" },
        { "r14",  "r14d", "r14w", "r14b" }, { "r15",  "r15d", "r15w", "r15b" },
    };
    static const int s_widths[4] = { 64, 32, 16, 8 };
    for (int r = 0; r < (int)COUNTOF(s_rows); ++r) {
        for (int c = 0; c < 4; ++c) {
            if (_stricmp(name, s_rows[r][c]) == 0) {
                return s_widths[c];
            }
        }
    }
    if (_strnicmp(name, "xmm", 3) == 0) {
        return 128;
    }
    if (_strnicmp(name, "ymm", 3) == 0) {
        return 256;
    }
    return 0;
}

static bool _IsBareMemoryOperand(const ARG_SPAN* a)
{
    const char* p = a->p;
    while ((p < a->end) && ((*p == ' ') || (*p == '\t'))) {
        ++p;
    }
    return ((p < a->end) && (*p == '['));
}

// A single '=' inside a Lua condition is an assignment, not a comparison.
static bool _LuaConditionHasAssignment(const char* p, const char* end)
{
    for (const char* r = p; r < end; ++r) {
        if (*r != '=') {
            continue;
        }
        char const prev = (r > p) ? r[-1] : '\0';
        char const next = ((r + 1) < end) ? r[1] : '\0';
        if ((prev == '=') || (prev == '~') || (prev == '<') || (prev == '>') || (next == '=')) {
            continue;
        }
        return true;
    }
    return false;
}

static void _CheckEffectiveCode(const CE_DOC* doc, const unsigned char* modes, CE_LINT* lint)
{
    int emitted = 0;
    char msg[CE_LINT_MSG_CCH];
    for (DocLn line = 0; (line < doc->lines) && (emitted < CE_LINT_HEURISTIC_MAX); ++line) {
        DocPos n = 0;
        const char* const lineStart = doc->text + doc->starts[line];
        const char* const p = CeScript_Trimmed(doc, line, &n);
        const char* const end = p + n;
        if ((p >= end) || _IsCommentStart(p, end)) {
            continue;
        }

        if (modes[line] == (unsigned char)CE_LANG_AA) {
            if ((*p == '[') || (*p == '{') || (*p == '#')) {
                continue;
            }
            char op[32] = { '\0' };
            const char* const afterOp = CeScript_ReadIdent(p, end, op, COUNTOF(op));
            if (afterOp == p) {
                continue;
            }
            const char* q = CeScript_SkipBlanks(afterOp, end);
            if ((q < end) && (*q == ':')) {
                continue; // label definition
            }
            int minArgs = 0;
            int maxArgs = 0;
            if (!_AsmOperandsExpected(op, &minArgs, &maxArgs)) {
                continue;
            }
            const char* argsEnd = end;
            for (const char* r = q; r < end; ++r) {
                if ((*r == ';') || ((*r == '/') && ((r + 1) < end) && (r[1] == '/'))) {
                    argsEnd = r;
                    break;
                }
            }
            ARG_SPAN spans[8] = { 0 };
            int argc = _SplitArgs(q, argsEnd, spans, COUNTOF(spans));
            if ((argc == 1) && (spans[0].p >= spans[0].end)) {
                argc = 0;
            }
            if (argc >= 2) {
                char lhs[64] = { '\0' };
                char rhs[64] = { '\0' };
                int const w0 = _SpanIsPlainIdent(&spans[0], lhs, COUNTOF(lhs)) ? _AsmRegWidth(lhs) : 0;
                int const w1 = _SpanIsPlainIdent(&spans[1], rhs, COUNTOF(rhs)) ? _AsmRegWidth(rhs) : 0;
                if (w0 && w1 && (w0 != w1)) {
                    StringCchPrintfA(msg, COUNTOF(msg), "%s (%d 位) 与 %s (%d 位) 位数不匹配",
                                     lhs, w0, rhs, w1);
                    _AddDiagAt(lint, (int)line + 1, (int)(spans[1].p - lineStart),
                               (int)(spans[1].end - spans[1].p), false, msg);
                    ++emitted;
                }
                if (_IsBareMemoryOperand(&spans[0]) && _SpanIsNumeric(&spans[1])) {
                    StringCchPrintfA(msg, COUNTOF(msg),
                                     "内存写入未指定大小，请加 dword ptr 等前缀");
                    _AddDiagAt(lint, (int)line + 1, (int)(spans[0].p - lineStart),
                               (int)(spans[0].end - spans[0].p), false, msg);
                    ++emitted;
                }
            }
        }
        else if (modes[line] == (unsigned char)CE_LANG_LUA) {
            char first[32] = { '\0' };
            const char* const afterFirst = CeScript_ReadIdent(p, end, first, COUNTOF(first));
            bool const conditional = (afterFirst != p) &&
                ((_stricmp(first, "if") == 0) || (_stricmp(first, "elseif") == 0) ||
                 (_stricmp(first, "while") == 0));
            if (conditional && _LuaConditionHasAssignment(afterFirst, end)) {
                StringCchPrintfA(msg, COUNTOF(msg), "条件中用了 '='，比较应写成 '=='");
                _AddDiagAt(lint, (int)line + 1, (int)(p - lineStart), (int)strlen(first),
                           true, msg);
                ++emitted;
            }
        }
    }
}

// ----------------------------------------------------------------------------

// AOB patterns are the most common silent failure in CE scripts: a malformed
// byte, or a pattern short enough to match several places, only shows up when
// the table is enabled.  Validate the quoted pattern statically.
static void _CheckAobPatterns(const CE_DOC* doc, const unsigned char* modes, CE_LINT* lint) {
    int emitted = 0;
    for (DocLn line = 0; (line < doc->lines) && (emitted < CE_LINT_HEURISTIC_MAX); ++line) {
        if (modes[line] != (unsigned char)CE_LANG_AA) {
            continue;
        }
        DocPos n = 0;
        const char* const p = CeScript_Trimmed(doc, line, &n);
        const char* const end = p + n;
        if (_IsCommentStart(p, end)) {
            continue;
        }
        char op[32] = { '\0' };
        const char* const afterOp = CeScript_ReadIdent(p, end, op, COUNTOF(op));
        if (afterOp == p) {
            continue;
        }
        if ((_stricmp(op, "aobscan") != 0) && (_stricmp(op, "aobscanmodule") != 0) &&
            (_stricmp(op, "aobscanregion") != 0) && (_stricmp(op, "aobscanpattern") != 0)) {
            continue;
        }

        // CE's own scripts pass the pattern either quoted or as bare bytes,
        // e.g. aobscanmodule(INJECT,game.exe,CC CC 48 83 EC 28).  Always pick
        // the pattern argument by position: optional protection/alignment
        // arguments may follow it.
        const char* const lp = CeScript_SkipBlanks(afterOp, end);
        if ((lp >= end) || (*lp != '(')) {
            continue;
        }
        const char* argsEnd = end;
        int depth = 0;
        char parenQuote = '\0';
        for (const char* r = lp; r < end; ++r) {
            if (parenQuote) {
                if ((*r == parenQuote) && (r[-1] != '\\')) {
                    parenQuote = '\0';
                }
                continue;
            }
            if ((*r == '"') || (*r == '\'')) {
                parenQuote = *r;
                continue;
            }
            if (*r == '(') {
                ++depth;
            }
            else if (*r == ')') {
                if (--depth == 0) {
                    argsEnd = r;
                    break;
                }
            }
        }

        ARG_SPAN args[8] = { 0 };
        int const argc = _SplitArgs(lp + 1, argsEnd, args, (int)COUNTOF(args));
        int patIndex = 1;
        if (_stricmp(op, "aobscanmodule") == 0) {
            patIndex = 2;
        }
        else if (_stricmp(op, "aobscanregion") == 0) {
            patIndex = 3;
        }
        if (patIndex >= argc) {
            continue; // argument count is reported elsewhere
        }
        const char* patStart = args[patIndex].p;
        const char* patEnd = args[patIndex].end;
        if ((patStart < patEnd) && ((*patStart == '"') || (*patStart == '\''))) {
            char const quote = *patStart++;
            if ((patEnd > patStart) && (patEnd[-1] == quote)) {
                --patEnd;
            }
        }

        int bytes = 0;
        bool bad = false;
        char badTok[8] = { '\0' };
        for (const char* q = patStart; q < patEnd;) {
            while ((q < patEnd) && isspace((unsigned char)*q)) {
                ++q;
            }
            if (q >= patEnd) {
                break;
            }
            if (*q == '*') {
                ++q;
                ++bytes;
                continue;
            }
            if (*q == '?') {
                q += (((q + 1) < patEnd) && (q[1] == '?')) ? 2 : 1;
                ++bytes;
                continue;
            }
            if (!isxdigit((unsigned char)q[0]) || ((q + 1) >= patEnd) ||
                !isxdigit((unsigned char)q[1])) {
                bad = true;
                size_t k = 0;
                while ((q < patEnd) && !isspace((unsigned char)*q) && (k + 1 < COUNTOF(badTok))) {
                    badTok[k++] = *q++;
                }
                badTok[k] = '\0';
                break;
            }
            q += 2;
            ++bytes;
        }
        if (bad) {
            char msg[CE_LINT_MSG_CCH];
            StringCchPrintfA(msg, COUNTOF(msg), "无效的字节 '%s'", badTok);
            _AddDiag(lint, (int)line + 1, true, msg);
            ++emitted;
            continue;
        }
        if (bytes == 0) {
            _AddDiag(lint, (int)line + 1, true, "AOB 特征码为空");
            ++emitted;
            continue;
        }
    }
}

// A symbol declared with alloc/label/define/registersymbol (or a Lua
// local/function) must not collide with an AA command, register, mnemonic,
// Lua keyword, CE API name or a section/directive name.  Cheat Engine then
// fails to activate the script, so this is surfaced as an inline warning.
static bool _IsReservedSymbolName(const char* name) {
    // Covers the catalogue plus CE_KEYWORDS_LUA / API / AA / ASM / TYPES.
    // Section markers ([ENABLE], {$lua}, ...) are not identifiers and the
    // default template's labels (originalcode, exit) are user symbols, so
    // neither is treated as a reserved name here.
    return CeCompletion_IsBuiltinKeyword(name);
}

// Locate |name| inside the given 1-based line, so the squiggle covers the
// symbol instead of the whole line.
static int _FindWordCol(const CE_DOC* doc, int line1, const char* name)
{
    if ((line1 < 1) || ((DocLn)line1 > doc->lines)) {
        return -1;
    }
    DocPos n = 0;
    const char* const p = CeScript_LinePtr(doc, (DocLn)(line1 - 1), &n);
    const char* const end = p + n;
    size_t const len = strlen(name);
    for (const char* r = p; (r + len) <= end; ++r) {
        if (_EqualsCI(r, len, name) &&
            ((r == p) || !_IsIdentChar(r[-1])) &&
            ((r + len == end) || !_IsIdentChar(r[len]))) {
            return (int)(r - p);
        }
    }
    return -1;
}

static void _CheckReservedSymbols(const CE_DOC* doc, const CE_DEF* defs, int defCount, CE_LINT* lint) {
    int emitted = 0;
    for (int i = 0; i < defCount && emitted < 20; ++i) {
        const CE_DEF* const def = &defs[i];
        // CE's own default template symbols are legitimate names.
        if (_IsCeBuiltinSymbol(def->name)) {
            continue;
        }
        if (!_IsReservedSymbolName(def->name)) {
            continue;
        }
        bool seen = false;
        for (int j = 0; j < i; ++j) {
            if (_stricmp(defs[j].name, def->name) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        char msg[CE_LINT_MSG_CCH];
        StringCchPrintfA(msg, COUNTOF(msg), "符号名 '%s' 与 CE 关键字冲突，改名后才能激活",
                         def->name);
        _AddAlertDiag(lint, (int)def->line, msg);
        // Attach the squiggle to the symbol itself (the alert carries no column).
        if (lint->count > 0) {
            CE_DIAG* const diag = &lint->items[lint->count - 1];
            diag->col = _FindWordCol(doc, (int)def->line, def->name);
            diag->len = (int)strlen(def->name);
        }
        ++emitted;
    }
}

void CeLint_Run(CE_LINT* lint) {
    if (!lint) {
        return;
    }
    ZeroMemory(lint, sizeof(CE_LINT));

    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc)) {
        return;
    }
    unsigned char* const modes = AllocMem((size_t)doc.lines, HEAP_ZERO_MEMORY);
    if (!modes) {
        CeScript_FreeDoc(&doc);
        return;
    }
    CeScript_LineModes(&doc, modes);

    // Structural checks (always on): mistakes that always break activation.
    _CheckBrackets(&doc, modes, lint);
    _CheckAobPatterns(&doc, modes, lint);
    _CheckLuaBlocks(&doc, modes, lint);
    _CheckDirectiveBlocks(&doc, lint);
    _CheckPlaceholders(&doc, modes, lint);

    // Semantic checks ([Settings2] CeLintSemantic, on by default).  The symbol
    // based passes are quadratic in the worst case, so they stay off large
    // documents to keep idle linting from making the editor unresponsive.
    bool const fullChecks = (doc.len <= CE_LINT_FULL_CHECKS_MAX);
    if (Settings2.CeLintSemantic && fullChecks) {
        _CheckSections(&doc, modes, lint);
        CE_DEF* const defs = AllocMem((size_t)CE_MAX_DEFS * sizeof(CE_DEF), HEAP_ZERO_MEMORY);
        int const defCount = defs ? CeScript_CollectDefs(&doc, defs, CE_MAX_DEFS) : 0;
        if (defs) {
            _CheckPairs(&doc, defs, defCount, lint);
            _CheckReservedSymbols(&doc, defs, defCount, lint);
            _CheckLabels(&doc, modes, defs, defCount, lint);
            _CheckAaStatements(&doc, modes, defs, defCount, lint);
            _CheckAaCallArgs(&doc, modes, defs, defCount, lint);
            _CheckAsmInstructions(&doc, modes, defs, defCount, lint);
            _CheckLuaStatics(&doc, modes, defs, defCount, lint);
            _CheckEffectiveCode(&doc, modes, lint);
            FreeMem(defs);
        }
        _CheckDuplicates(&doc, modes, lint);
    }

    // CE's own autoAssembleCheck result (CMD_CHECK_SCRIPT).  These are real
    // parser errors with a line number, so errors become red squiggles and
    // warnings use the always-visible alert style.
    for (int i = 0; i < s_externalCount; ++i) {
        CE_EXTERNAL_DIAG const* const d = &s_external[i];
        if (d->error) {
            _AddDiag(lint, d->line, true, d->message);
        }
        else {
            _AddAlertDiag(lint, d->line, d->message);
        }
    }

    if (lint->count > 1) {
        qsort(lint->items, (size_t)lint->count, sizeof(CE_DIAG), _CompareDiag);
    }

    FreeMem(modes);
    CeScript_FreeDoc(&doc);
}

// ----------------------------------------------------------------------------

// Set when the marks were wiped because the text changed (so the same finding
// set must be re-rendered even though its digest did not change).
static bool s_lastHashValid = false;
static unsigned int s_lastHash = 0;
static bool s_textDirty = false;
// Only a real input change schedules a full lint pass.  Re-running the whole
// (partly quadratic) analysis every 350 ms on an idle document kept a core
// busy for no reason; the hash only protected the rendering, not the scan.
static bool s_needRun = true;

void CeLint_ClearAnnotations(void) {
    CeDiagnostics_Clear();
    s_lastHashValid = false;
    s_needRun = true;
}

void CeLint_Invalidate(void) {
    s_textDirty = true;
    s_needRun = true;
}

void CeLint_ApplyAnnotations(const CE_LINT* lint) {
    CeDiagnostics_Begin();
    if (lint) {
        // The full finding set is handed over in one go: the renderer replaces
        // every previous hint atomically, so a fixed error vanishes together
        // with the line it used to point at.
        for (int i = 0; (i < lint->count) && (i < CE_DIAG_MAX_HINTS); ++i) {
            const CE_DIAG* const diag = &lint->items[i];
            CeDiagnostics_Add((DocLn)(diag->line - 1), diag->col, diag->col + diag->len,
                              diag->error, diag->message);
        }
    }
    CeDiagnostics_End();
}

static unsigned int _LintHash(const CE_LINT* lint) {
    unsigned int hash = 2166136261u;
    if (!lint) {
        return hash;
    }
    for (int i = 0; i < lint->count; ++i) {
        const CE_DIAG* const diag = &lint->items[i];
        hash = (hash ^ (unsigned int)diag->line) * 16777619u;
        hash = (hash ^ (unsigned int)diag->col) * 16777619u;
        hash = (hash ^ (unsigned int)diag->len) * 16777619u;
        hash = (hash ^ (unsigned int)diag->error) * 16777619u;
        for (const char* p = diag->message; *p; ++p) {
            hash = (hash ^ (unsigned char)*p) * 16777619u;
        }
    }
    return hash;
}

bool CeLint_OnIdle(void) {
    static ULONGLONG s_lastTick = 0;
    static bool s_wasActive = false;

    bool const active = CeScript_IsActive();
    if (!active) {
        if (s_wasActive) {
            CeLint_ClearAnnotations();
            s_wasActive = false;
        }
        return false;
    }
    if (!s_wasActive) {
        s_wasActive = true;
        s_needRun = true;
    }

    // Style_SetLexer()/StyleClearAll() resets extended styles; repair the
    // ghost-text colours even when the finding set itself is unchanged.
    CeDiagnostics_RefreshStyles();

    // The text changed since the last pass: wipe the stale hints right away
    // (no throttle) so a fixed error disappears immediately, then fall through
    // to re-render what is still wrong.
    if (s_textDirty) {
        s_textDirty = false;
        CeLint_ClearAnnotations();
    }
    if (!s_needRun) {
        return false;
    }

    ULONGLONG const now = GetTickCount64();
    if ((s_lastTick != 0) && ((now - s_lastTick) < CE_LINT_INTERVAL_MS)) {
        return true; // debounce: ask the caller to re-arm the one-shot timer
    }
    s_lastTick = now;
    s_needRun = false;

    if (Sci_GetDocEndPosition() > CE_LINT_MAX_DOC_LEN) {
        CeDiagnostics_Clear();
        s_lastHashValid = false;
        return false;
    }
    CE_LINT lint = { 0 };
    CeLint_Run(&lint);
    unsigned int const hash = _LintHash(&lint);
    if (!s_lastHashValid || (hash != s_lastHash)) {
        s_lastHash = hash;
        s_lastHashValid = true;
        CeLint_ApplyAnnotations(&lint);
    }
    return false;
}

void CeLint_ShowReview(HWND hwndParent, bool bOnlineChecked) {
    CE_LINT lint = { 0 };
    CeLint_Run(&lint);
    CeLint_ApplyAnnotations(&lint);

    WCHAR text[8192] = { L'\0' };
    WCHAR lineFmt[128] = { L'\0' };
    size_t used = 0;
    if (lint.count == 0) {
        GetLngString(IDS_MUI_CE_REVIEW_OK, text, (int)COUNTOF(text));
        if (text[0] == L'\0') {
            StringCchCopyW(text, COUNTOF(text), L"CE script review: no issues found.");
        }
    }
    else {
        FormatLngStringW(text, (int)COUNTOF(text), IDS_MUI_CE_REVIEW_SUMMARY,
                         lint.errors, lint.warnings);
        if (text[0] == L'\0') {
            StringCchPrintfW(text, COUNTOF(text),
                             L"CE script review: %d error(s), %d warning(s)\r\n\r\n",
                             lint.errors, lint.warnings);
        }
        GetLngString(IDS_MUI_CE_REVIEW_LINE, lineFmt, (int)COUNTOF(lineFmt));
        if (lineFmt[0] == L'\0') {
            StringCchCopyW(lineFmt, COUNTOF(lineFmt), L"%ls line %d: %ls\r\n");
        }
        used = wcslen(text);
        int const shown = min_i(lint.count, 40);
        for (int i = 0; i < shown; ++i) {
            const CE_DIAG* const diag = &lint.items[i];
            // The diagnostic text is UTF-8; "%s" in a wide format string would
            // reinterpret those bytes as UTF-16 (mojibake), so convert first.
            WCHAR wmsg[CE_LINT_MSG_CCH] = { L'\0' };
            MultiByteToWideChar(CP_UTF8, 0, diag->message, -1, wmsg, (int)COUNTOF(wmsg));
            WCHAR line[CE_LINT_MSG_CCH + 64] = { L'\0' };
            StringCchPrintfW(line, COUNTOF(line), lineFmt,
                diag->error ? L"E" : L"W", diag->line, wmsg);
            if (used + wcslen(line) + 1 >= COUNTOF(text)) {
                break;
            }
            StringCchCatW(text, COUNTOF(text), line);
            used = wcslen(text);
        }
        if (lint.count > shown) {
            StringCchCatW(text, COUNTOF(text), L"...\r\n");
        }
    }
    if (!bOnlineChecked) {
        WCHAR note[256] = { L'\0' };
        GetLngString(IDS_MUI_CE_REVIEW_OFFLINE, note, (int)COUNTOF(note));
        if (note[0] != L'\0') {
            StringCchCatW(text, COUNTOF(text), note);
        }
    }

    WCHAR title[128] = { L'\0' };
    GetLngString(IDS_MUI_CE_REVIEW_TITLE, title, (int)COUNTOF(title));
    if (title[0] == L'\0') {
        StringCchCopyW(title, COUNTOF(title), L"CE Code Review");
    }
    UINT const icon = lint.errors ? MB_ICONERROR : (lint.warnings ? MB_ICONWARNING : MB_ICONINFORMATION);
    MessageBoxW(hwndParent, text, title, MB_OK | icon);
}

void CeLint_ShowReport(HWND hwndParent) {
    CeLint_ShowReview(hwndParent, false);
}

void CeLint_WarnReservedSymbols(HWND hwndParent) {
    // The symbol based checks are capped; keep the save path cheap as well.
    if (!CeScript_IsActive() || (SciCall_GetLength() > CE_LINT_FULL_CHECKS_MAX)) {
        return;
    }
    CE_LINT lint = { 0 };
    CeLint_Run(&lint);

    WCHAR text[4096] = L"以下自定义变量名与 CE 关键字/保留字冲突，脚本可能无法正常激活，请改名：\r\n\r\n";
    int shown = 0;
    for (int i = 0; (i < lint.count) && (shown < 12); ++i) {
        const CE_DIAG* const diag = &lint.items[i];
        if (!diag->alert) {
            continue;
        }
        WCHAR wmsg[CE_LINT_MSG_CCH] = { L'\0' };
        MultiByteToWideChar(CP_UTF8, 0, diag->message, -1, wmsg, (int)COUNTOF(wmsg));
        WCHAR line[CE_LINT_MSG_CCH + 32] = { L'\0' };
        StringCchPrintfW(line, COUNTOF(line), L"第 %d 行：%ls\r\n", diag->line, wmsg);
        if (FAILED(StringCchCatW(text, COUNTOF(text), line))) {
            break;
        }
        ++shown;
    }
    if (shown > 0) {
        MessageBoxW(hwndParent, text, L"CE 变量名冲突提醒", MB_OK | MB_ICONWARNING);
    }
}
