// encoding: UTF-8
// Unit tests for the pure CE analysis/ranking helpers.  TestCeLint.ps1 extracts
// those helpers verbatim from src/CeLint.c and src/CeCompletion.c into
// CeLintHelpers.inc; everything else is stubbed here so the test needs neither
// Scintilla nor a running editor.

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

#define CE_KIND_NONE     0
#define CE_KIND_KEYWORD  1
#define CE_KIND_FUNCTION 2
#define CE_KIND_VARIABLE 3
#define CE_KIND_AA       4
#define CE_KIND_ASM      5
#define CE_KIND_TYPE     6
#define CE_KIND_CONSTANT 7
#define CE_KIND_SECTION  8
#define CE_KIND_COUNT    9

#define CE_LANG_LUA 0
#define CE_LANG_AA  1
#define CE_LANG_C   2

typedef struct _arg_span { const char* p; const char* end; } ARG_SPAN;
typedef struct _asm_rule { const char* op; int minArgs; int maxArgs; } ASM_RULE;
typedef struct _lua_arity { const char* name; int minArgs; } LUA_ARITY;
typedef struct _lua_sym { char name[64]; } LUA_SYM;
typedef struct _lua_symtab { LUA_SYM* items; int count; int max; } LUA_SYMTAB;
typedef struct _ce_doc_sym { char* name; int kind; } CE_DOC_SYM;

typedef struct CE_COMPLETION_ENTRY {
    const char* name;
    const char* signature;
    const char* category;
    int isCall;
    int kind;
} CE_COMPLETION_ENTRY;

typedef enum _ce_group {
    CE_GROUP_LUA = 0,
    CE_GROUP_AA,
    CE_GROUP_C,
    CE_GROUP_NEUTRAL
} CE_GROUP;

typedef struct _ce_cand { const char* name; int kind; int score; } CE_CAND;
typedef struct _ce_extra_word { char name[64]; int kind; } CE_EXTRA_WORD;

#define CE_EXTRA_WORD_MAX 512

static CE_EXTRA_WORD* s_extraWords = nullptr;
static int            s_extraCount = 0;

// Representative slices of the generated keyword sets.
static const char CE_KEYWORDS_ASM[] = "mov jmp je jne ret nop add eax rax";
static const char CE_KEYWORDS_AA[] = "alloc dealloc label define registersymbol dword ptr";
static const char CE_KEYWORDS_TYPES[] = "MainForm MemoryRecord";
static const char CE_KEYWORDS_LUA[] = "and break do else end for function if in local nil not or repeat return then true until while print";
static const char CE_KEYWORDS_API[] = "readInteger writeInteger getAddress autoAssemble AOBScan showMessage sleep";

static char LowerCh(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : ch;
}
static bool _IsIdentStart(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
static bool _IsIdentChar(char ch) {
    return _IsIdentStart(ch) || (ch >= '0' && ch <= '9');
}
static bool _EqualsCI(const char* text, size_t len, const char* word) {
    size_t const wordLen = strlen(word);
    if (len != wordLen) { return false; }
    for (size_t i = 0; i < len; ++i) {
        if (LowerCh(text[i]) != LowerCh(word[i])) { return false; }
    }
    return true;
}
static bool _StartsWithCI(const char* text, size_t len, const char* prefix) {
    size_t const prefixLen = strlen(prefix);
    return (len >= prefixLen) && _EqualsCI(text, prefixLen, prefix);
}
static bool _InKeywordList(const char* list, const char* word) {
    const char* p = list;
    while (*p) {
        while (*p == ' ') { ++p; }
        const char* const start = p;
        while (*p && *p != ' ') { ++p; }
        if (_EqualsCI(start, (size_t)(p - start), word)) { return true; }
    }
    return false;
}
static bool _IsIdentifierLike(const char* name) {
    if (!name || !*name) { return false; }
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) { return false; }
    for (const char* p = name; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == ':' || *p == '$')) { return false; }
    }
    return true;
}
static const char* CeScript_ReadIdent(const char* p, const char* end, char* out, size_t outCch) {
    if ((p >= end) || !_IsIdentStart(*p)) { return p; }
    const char* const start = p;
    while ((p < end) && _IsIdentChar(*p)) { ++p; }
    size_t len = (size_t)(p - start);
    if (len >= outCch) { len = outCch - 1; }
    memcpy(out, start, len);
    out[len] = '\0';
    return p;
}

#include "CeLintHelpers.inc"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool IsNumeric(const char* s) {
    ARG_SPAN a = { s, s + strlen(s) };
    return _SpanIsNumeric(&a);
}
static bool IsPlainIdent(const char* s, char* out, size_t cch) {
    ARG_SPAN a = { s, s + strlen(s) };
    return _SpanIsPlainIdent(&a, out, cch);
}
static std::string SpanText(const ARG_SPAN& a) {
    return std::string(a.p, a.end);
}

int main()
{
    // --- AA command / assembly argument splitting ---------------------------
    {
        const char* const text = "a, b, [c+d]";
        ARG_SPAN spans[8] = {};
        int const n = _SplitArgs(text, text + strlen(text), spans, 8);
        CHECK(n == 3);
        CHECK(SpanText(spans[0]) == "a");
        CHECK(SpanText(spans[1]) == "b");
        CHECK(SpanText(spans[2]) == "[c+d]");
    }
    {
        const char* const text = "foo(\"a,b\"), 2";
        ARG_SPAN spans[8] = {};
        CHECK(_SplitArgs(text, text + strlen(text), spans, 8) == 2);
    }
    {
        const char* const text = "";
        ARG_SPAN spans[8] = {};
        CHECK(_SplitArgs(text, text, spans, 8) == 1);
        CHECK(spans[0].p == spans[0].end);
    }

    // --- AA size literals ---------------------------------------------------
    CHECK(IsNumeric("0x1000"));
    CHECK(IsNumeric("1024"));
    CHECK(IsNumeric("$400"));
    CHECK(!IsNumeric("newmem"));
    CHECK(!IsNumeric("size"));
    {
        char name[64] = {};
        CHECK(IsPlainIdent("newmem", name, sizeof(name)) && strcmp(name, "newmem") == 0);
        CHECK(!IsPlainIdent("[eax+4]", name, sizeof(name)));
        CHECK(!IsPlainIdent("dword ptr [eax]", name, sizeof(name)));
    }
    CHECK(_IsKnownBareOperand("eax"));
    CHECK(!_IsKnownBareOperand("totallyUnknown"));

    // --- symbol definition lines (CE symbols may contain `[]`, `,`, `.`) -----
    {
        const char* const sym =
            "System.Collections.Generic.Dictionary`2[System.String,System.Single]:TryInsert+3d0:";
        CHECK(_IsLabelDefinitionLine(sym, sym + strlen(sym)));
        const char* const lbl = "newmem:";
        CHECK(_IsLabelDefinitionLine(lbl, lbl + strlen(lbl)));
        const char* const off = "module.exe+1234:";
        CHECK(_IsLabelDefinitionLine(off, off + strlen(off)));
        const char* const ins = "mov eax, ebx";
        CHECK(!_IsLabelDefinitionLine(ins, ins + strlen(ins)));
        const char* const call = "alloc(newmem, 2048)";
        CHECK(!_IsLabelDefinitionLine(call, call + strlen(call)));
        const char* const bare = "x";
        CHECK(!_IsLabelDefinitionLine(bare, bare + strlen(bare)));
    }

    // --- x86 operand rules --------------------------------------------------
    {
        int mn = 0;
        int mx = 0;
        CHECK(_AsmOperandsExpected("mov", &mn, &mx) && mn == 2 && mx == 2);
        CHECK(_AsmOperandsExpected("ret", &mn, &mx) && mn == 0 && mx == 1);
        CHECK(_AsmOperandsExpected("je", &mn, &mx) && mn == 1 && mx == 2);
        CHECK(!_AsmOperandsExpected("notAnOpcode", &mn, &mx));
    }

    // --- Lua call arity -----------------------------------------------------
    {
        const char* a = "(a, f(b,c))";
        CHECK(_CountCallArgs(a, a + strlen(a)) == 2);
        const char* b = "()";
        CHECK(_CountCallArgs(b, b + strlen(b)) == 0);
        const char* c = "(a";
        CHECK(_CountCallArgs(c, c + strlen(c)) == -1);
        CHECK(_LuaMinArgs("readInteger") == 1);
        CHECK(_LuaMinArgs("writeInteger") == 2);
        CHECK(_LuaMinArgs("unknownFn") == -1);
    }

    // --- keyword-set-only words stay available ------------------------------
    {
        const char* const catalogue[] = { "mov", "readInteger", "table.insert" };
        const char* sortable[3] = { "readInteger", "mov", "table.insert" };
        qsort(sortable, COUNTOF(sortable), sizeof(char*), _CompareNamePtr);
        CHECK(strcmp(sortable[0], "mov") == 0 && strcmp(sortable[1], "readInteger") == 0);
        s_extraWords = (CE_EXTRA_WORD*)calloc(64, sizeof(CE_EXTRA_WORD));
        s_extraCount = 0;
        _AddExtraWords("mov table align createthread BYTE readInteger", CE_KIND_AA,
                       catalogue, (int)COUNTOF(catalogue));
        CHECK(s_extraCount == 4);
        bool hasTable = false, hasAlign = false, hasThread = false, hasByte = false, hasMov = false;
        for (int i = 0; i < s_extraCount; ++i) {
            const char* const n = s_extraWords[i].name;
            hasTable = hasTable || strcmp(n, "table") == 0;
            hasAlign = hasAlign || strcmp(n, "align") == 0;
            hasThread = hasThread || strcmp(n, "createthread") == 0;
            hasByte = hasByte || strcmp(n, "BYTE") == 0;
            hasMov = hasMov || _stricmp(n, "mov") == 0;
            CHECK(s_extraWords[i].kind == CE_KIND_AA);
        }
        CHECK(hasTable && hasAlign && hasThread && hasByte && !hasMov);
        free(s_extraWords);
        s_extraWords = nullptr;
        s_extraCount = 0;
    }

    // --- Context aware completion ranking ----------------------------------
    CE_COMPLETION_ENTRY mov = { "mov", "mov(dst, src)", "asm", 0, CE_KIND_ASM };
    CE_COMPLETION_ENTRY readInt = { "readInteger", "readInteger(addr)", "CE Lua API", 1, CE_KIND_FUNCTION };
    CHECK(_ScoreCatalogue(&mov, "", 0, CE_LANG_AA) > _ScoreCatalogue(&readInt, "", 0, CE_LANG_AA));
    CHECK(_ScoreCatalogue(&readInt, "", 0, CE_LANG_LUA) > _ScoreCatalogue(&mov, "", 0, CE_LANG_LUA));

    CE_COMPLETION_ENTRY cInt = { "int", "int", "C", 0, CE_KIND_TYPE };
    CHECK(_ScoreCatalogue(&cInt, "", 0, CE_LANG_C) > _ScoreCatalogue(&cInt, "", 0, CE_LANG_LUA));

    char symName[] = "addr";
    CE_DOC_SYM sym = { symName, CE_KIND_VARIABLE };
    CE_COMPLETION_ENTRY aaOnly = { "aobscanregion", "aobscanregion(...)", "AA", 1, CE_KIND_AA };
    CHECK(_ScoreDocSym(&sym, "a", 1, CE_LANG_LUA) > _ScoreCatalogue(&aaOnly, "a", 1, CE_LANG_LUA));

    CE_CAND low = { "x", 1, 10 };
    CE_CAND high = { "y", 1, 20 };
    CHECK(_CompareCand(&low, &high) > 0);
    CHECK(_CompareCand(&high, &low) < 0);

    if (g_failures) {
        std::printf("%d CE lint/completion assertion(s) failed\n", g_failures);
        return 1;
    }
    std::printf("CE lint/completion helpers: all tests passed\n");
    return 0;
}
