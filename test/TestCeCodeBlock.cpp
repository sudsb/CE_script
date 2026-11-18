// encoding: UTF-8
// Regression tests for CE code-block completions: the block table stays
// separate from (and therefore preserves) the generated keyword catalogue,
// bodies render into valid multi-line skeletons, and surrounding-code
// context decides whether a block should be offered.

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

#define CE_LANG_LUA 0
#define CE_LANG_AA  1
#define CE_LANG_C   2

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

typedef struct CE_COMPLETION_ENTRY {
    const char* name;
    const char* signature;
    const char* category;
    int isCall;
    int kind;
} CE_COMPLETION_ENTRY;

static inline bool StrIsEmptyA(const char* s) { return !s || !*s; }

#include "CeCodeBlockHelpers.inc"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool IdentifierLike(const char* name) {
    if (!name || !*name) { return false; }
    if (!((*name >= 'A' && *name <= 'Z') || (*name >= 'a' && *name <= 'z') || *name == '_')) {
        return false;
    }
    for (const char* p = name + 1; *p; ++p) {
        char const ch = *p;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) {
            return false;
        }
    }
    return true;
}

// The {$lua}/{$asm} skeletons are deliberately not identifier shaped: their
// name must start with the directive opener so typing "{$" matches them.
static bool ValidBlockName(const char* name) {
    if (!name || !*name) { return false; }
    if (name[0] == '{' && name[1] == '$') {
        return (std::strchr(name, ' ') == nullptr) && (std::strchr(name, '?') == nullptr);
    }
    return IdentifierLike(name);
}

int main()
{
    // --- The original keywords are untouched --------------------------------
    CHECK(_FindCodeBlock("if") == nullptr);
    CHECK(_FindCodeBlock("for") == nullptr);
    CHECK(_FindCodeBlock("function") == nullptr);
    CHECK(_FindCodeBlock("[ENABLE]") == nullptr);
    CHECK(_FindCodeBlock("ifBlock") != nullptr);
    CHECK(_FindCodeBlock("IFBLOCK") != nullptr); // case-insensitive lookup
    CHECK(_FindCodeBlock("nope") == nullptr);

    // --- Every block is a valid AutoC item ----------------------------------
    int luaBlocks = 0;
    int aaBlocks = 0;
    for (int i = 0; i < (int)COUNTOF(s_codeBlocks); ++i) {
        const CE_CODE_BLOCK* const b = &s_codeBlocks[i];
        CHECK(ValidBlockName(b->entry.name));
        CHECK(std::strchr(b->entry.name, '?') == nullptr);
        CHECK(b->body && b->body[0]);
        CHECK(b->entry.signature && b->entry.signature[0]);
        CHECK(b->entry.kind >= 0 && b->entry.kind < CE_KIND_COUNT);
        CHECK((b->modeMask & (CE_BLOCK_LUA | CE_BLOCK_AA)) != 0);
        if (b->modeMask & CE_BLOCK_LUA) { ++luaBlocks; }
        if (b->modeMask & CE_BLOCK_AA) { ++aaBlocks; }
        for (int j = 0; j < i; ++j) {
            CHECK(_stricmp(s_codeBlocks[j].entry.name, b->entry.name) != 0);
        }
    }
    CHECK(luaBlocks >= 8);
    CHECK(aaBlocks >= 2);

    // --- {$lua} / {$asm} paired skeletons ---------------------------------
    // They must be reachable from both directive prefixes and from every CE
    // language region (the directive is how one leaves a Lua / inline-C block).
    for (const char* name : {"{$lua}Block", "{$asm}Block"}) {
        const CE_CODE_BLOCK* const b = _FindCodeBlock(name);
        CHECK(b != nullptr);
        if (!b) { continue; }
        CHECK(std::strstr(b->body, "{$lua}") != nullptr);
        CHECK(std::strstr(b->body, "{$asm}") != nullptr);
        CHECK((b->modeMask & CE_BLOCK_LUA) != 0);
        CHECK((b->modeMask & CE_BLOCK_AA) != 0);
        CHECK((b->modeMask & CE_BLOCK_C) != 0);
    }

    // --- Rendering --------------------------------------------------------
    {
        char out[512];
        int selStart = -1, selEnd = -1;
        _RenderCodeBlock(_FindCodeBlock("ifBlock")->body, "    ", "\n", "    ",
                         out, COUNTOF(out), &selStart, &selEnd);
        CHECK(std::strncmp(out, "if condition then", 17) == 0);
        CHECK(selStart == 3);
        CHECK(selEnd == 12); // "condition" is selected for overwrite
        CHECK(std::strstr(out, "\n        ") != nullptr); // indent + one step
        CHECK(std::strstr(out, "\n    end") != nullptr);  // base indent on "end"
        CHECK(std::strcmp(out + std::strlen(out) - 3, "end") == 0);
    }
    {
        char out[512];
        int selStart = -1, selEnd = -1;
        _RenderCodeBlock("a\n\tb", "", "\r\n", "  ", out, COUNTOF(out), &selStart, &selEnd);
        CHECK(std::strcmp(out, "a\r\n  b") == 0);
        CHECK(selStart == -1); // no placeholder -> no forced selection
    }
    {
        char out[512];
        int selStart = -1, selEnd = -1;
        _RenderCodeBlock("x$0y", "", "\n", "  ", out, COUNTOF(out), &selStart, &selEnd);
        CHECK(std::strcmp(out, "xy") == 0);
        CHECK(selStart == 1 && selEnd == 1); // collapsed caret stop
    }
    {
        char out[8];
        int selStart = -1, selEnd = -1;
        _RenderCodeBlock("local ${1:averylongname} = 1", "", "\n", "  ",
                         out, COUNTOF(out), &selStart, &selEnd);
        CHECK(std::strlen(out) < COUNTOF(out)); // buffer bound respected
        CHECK(std::strlen(out) > 0);
        CHECK(selStart >= 0);
    }
    {
        char out[512];
        _RenderCodeBlock(nullptr, "", "\n", "  ", out, COUNTOF(out), nullptr, nullptr);
        CHECK(out[0] == '\0');
    }

    // --- Surrounding-code context ------------------------------------------
    {
        CE_STMT_CTX c = _StatementContext("", 0);
        CHECK(c.atStatementStart && !c.afterMember && !c.inTable);
        c = _StatementContext("    ", 4);
        CHECK(c.atStatementStart && !c.inTable);
        c = _StatementContext("print(fi", 8);
        CHECK(!c.atStatementStart);
        c = _StatementContext("obj.", 4);
        CHECK(c.afterMember && !c.atStatementStart);
        c = _StatementContext("local t = { ", 12);
        CHECK(c.inTable);
        c = _StatementContext("x = ", 4);
        CHECK(c.atStatementStart);
        c = _StatementContext("local m = {\n    ", 16);
        CHECK(c.inTable); // still inside the table on the continuation line
    }

    // --- Block-closing keyword awareness -----------------------------------
    CHECK(_IsLuaBlockCloser("end"));
    CHECK(_IsLuaBlockCloser("else"));
    CHECK(_IsLuaBlockCloser("elseif"));
    CHECK(_IsLuaBlockCloser("until"));
    CHECK(!_IsLuaBlockCloser("if"));
    CHECK(!_IsLuaBlockCloser("function"));

    if (g_failures != 0) {
        std::printf("%d CE code block test(s) failed\n", g_failures);
        return 1;
    }
    std::printf("CE code block tests passed\n");
    return 0;
}
