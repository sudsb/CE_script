// encoding: UTF-8
// Regression tests for the shared CE section-marker grammar and the Lua
// lexical scope model.  TestCeLuaScope.ps1 extracts the declarations and the
// pure helpers verbatim from src/CeScript.h / src/CeScript.c into
// CeLuaScopeHelpers.inc; no Scintilla, no editor state, no document needed.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

typedef long long DocPos;
typedef int       DocLn;

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

#include "CeLuaScopeHelpers.inc"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static CE_LUA_MODEL g_model;

static bool Marker(const char* text, char* name, size_t cch) {
    return CeScript_SectionMarker(text, strlen(text), name, cch);
}

static void Analyze(const char* text) {
    CeLuaScope_Init(&g_model);
    const char* p = text;
    size_t base = 0;
    DocLn line = 1;
    for (;;) {
        const char* const nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if ((len > 0) && (p[len - 1] == '\r')) {
            --len;
        }
        CeLuaScope_Line(&g_model, p, p + len, (DocPos)base, line);
        if (!nl) {
            break;
        }
        base += (size_t)(nl - p) + 1;
        p = nl + 1;
        ++line;
    }
    CeLuaScope_Finish(&g_model, (DocPos)strlen(text));
}

static DocPos Pos(const char* text, const char* needle) {
    const char* const hit = strstr(text, needle);
    return hit ? (DocPos)(hit - text) : (DocPos)-1;
}

static int FindSym(const char* name) {
    for (int i = 0; i < g_model.count; ++i) {
        if (_stricmp(g_model.syms[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int KindOf(const char* name) {
    int const i = FindSym(name);
    return (i < 0) ? CE_LUA_SYM_NONE : g_model.syms[i].kind;
}

int main()
{
    // --- Section markers ---------------------------------------------------
    // The linter used to accept any line starting with '[' that also contained
    // a ']', which turned the Lua table entries "[1] = 'hello'" / "[2] = ..."
    // into "unknown section marker" warnings (the screenshot regression).
    char name[64];
    CHECK(Marker("[ENABLE]", name, COUNTOF(name)));
    CHECK(strcmp(name, "ENABLE") == 0);
    CHECK(Marker("[ disable ]", name, COUNTOF(name)));
    CHECK(strcmp(name, "disable") == 0);
    CHECK(Marker("[DISABLE] -- note", name, COUNTOF(name)));
    CHECK(strcmp(name, "DISABLE") == 0);
    CHECK(Marker("[FOO]", name, COUNTOF(name)));
    CHECK(strcmp(name, "FOO") == 0);
    CHECK(Marker("[FOO] ; trailing comment", name, COUNTOF(name)));
    CHECK(Marker("[FOO]\r\n", name, COUNTOF(name)));
    CHECK(strcmp(name, "FOO") == 0);
    CHECK(!Marker("    [1] = 'hello'", name, COUNTOF(name)));
    CHECK(!Marker("    [2] = 'hello'", name, COUNTOF(name)));
    CHECK(!Marker("[key] = value", name, COUNTOF(name)));
    CHECK(!Marker("[FOO] = 1", name, COUNTOF(name)));
    CHECK(!Marker("t[foo] = 1", name, COUNTOF(name)));
    CHECK(!Marker("[ENABLE] alloc(x,8)", name, COUNTOF(name)));
    CHECK(!Marker("", name, COUNTOF(name)));
    CHECK(!Marker("[", name, COUNTOF(name)));
    CHECK(!Marker("-- [COMMENT]", name, COUNTOF(name)));

    // --- The script from the screenshot ------------------------------------
    {
        const char* const script =
            "local m = {\n"
            "    [1] = 'hello'\n"
            "    [2] = 'hello'\n"
            "}\n";
        Analyze(script);
        CHECK(KindOf("m") == CE_LUA_SYM_LOCAL);
        CHECK(FindSym("1") < 0);
        CHECK(FindSym("2") < 0);
        CHECK(FindSym("hello") < 0);       // string contents are inert
        CHECK(CeLuaScope_Resolve(&g_model, "m", Pos(script, "}")) == CE_LUA_SYM_LOCAL);
        CHECK(!g_model.truncated);
    }

    // --- Block scope: locals are invisible before declaration and after end --
    {
        const char* const script =
            "local outer = 1\n"
            "do\n"
            "  local inner = 2\n"
            "  print(inner)\n"
            "end\n"
            "print(outer)\n";
        Analyze(script);
        DocPos const innerDecl = Pos(script, "inner = 2");
        DocPos const innerUse = Pos(script, "print(inner)") + 6;
        DocPos const afterEnd = Pos(script, "print(outer)") + 6;
        CHECK(CeLuaScope_Resolve(&g_model, "inner", innerUse) == CE_LUA_SYM_LOCAL);
        CHECK(CeLuaScope_Resolve(&g_model, "inner", innerDecl - 1) == CE_LUA_SYM_NONE);
        CHECK(CeLuaScope_Resolve(&g_model, "inner", afterEnd) == CE_LUA_SYM_NONE);
        CHECK(CeLuaScope_Resolve(&g_model, "outer", afterEnd) == CE_LUA_SYM_LOCAL);
        CHECK(CeLuaScope_DeclaredAnywhere(&g_model, "inner"));
        CHECK(!CeLuaScope_DeclaredAnywhere(&g_model, "print"));
    }

    // --- Functions, parameters and local body ------------------------------
    {
        const char* const script =
            "function foo(a, b)\n"
            "  local c = a\n"
            "  return c\n"
            "end\n"
            "print(foo)\n";
        Analyze(script);
        CHECK(KindOf("foo") == CE_LUA_SYM_FUNCTION);
        CHECK(KindOf("a") == CE_LUA_SYM_PARAM);
        CHECK(KindOf("b") == CE_LUA_SYM_PARAM);
        CHECK(KindOf("c") == CE_LUA_SYM_LOCAL);
        DocPos const inBody = Pos(script, "return c") + 7;
        DocPos const afterFn = Pos(script, "print(foo)") + 6;
        CHECK(CeLuaScope_Resolve(&g_model, "a", inBody) == CE_LUA_SYM_PARAM);
        CHECK(CeLuaScope_Resolve(&g_model, "c", inBody) == CE_LUA_SYM_LOCAL);
        CHECK(CeLuaScope_Resolve(&g_model, "a", afterFn) == CE_LUA_SYM_NONE);
        CHECK(CeLuaScope_Resolve(&g_model, "c", afterFn) == CE_LUA_SYM_NONE);
        CHECK(CeLuaScope_Resolve(&g_model, "foo", afterFn) == CE_LUA_SYM_FUNCTION);
    }

    // --- local function and dotted/colon method names -----------------------
    {
        const char* const script =
            "local function helper(x)\n"
            "  return x\n"
            "end\n"
            "function obj.method:call(y)\n"
            "  return y\n"
            "end\n";
        Analyze(script);
        CHECK(KindOf("helper") == CE_LUA_SYM_LOCALFN);
        CHECK(KindOf("call") == CE_LUA_SYM_FUNCTION);
        CHECK(KindOf("x") == CE_LUA_SYM_PARAM);
        CHECK(KindOf("y") == CE_LUA_SYM_PARAM);
        CHECK(FindSym("obj") < 0);          // only the last segment is a name
        CHECK(FindSym("method") < 0);
    }

    // --- for loop variables -------------------------------------------------
    {
        const char* const script =
            "for i = 1, 3 do\n"
            "  print(i)\n"
            "end\n"
            "print(i)\n";
        Analyze(script);
        DocPos const inLoop = Pos(script, "print(i)") + 6;
        DocPos const afterLoop = Pos(script, "end\nprint(i)") + 10;
        CHECK(KindOf("i") == CE_LUA_SYM_LOCAL);
        CHECK(CeLuaScope_Resolve(&g_model, "i", inLoop) == CE_LUA_SYM_LOCAL);
        CHECK(CeLuaScope_Resolve(&g_model, "i", afterLoop) == CE_LUA_SYM_NONE);
    }

    // --- Globals are document-wide -----------------------------------------
    {
        const char* const script =
            "function setup()\n"
            "  globalVar = 1\n"
            "end\n"
            "print(globalVar)\n";
        Analyze(script);
        CHECK(KindOf("globalVar") == CE_LUA_SYM_GLOBAL);
        CHECK(CeLuaScope_Resolve(&g_model, "globalVar",
              Pos(script, "print(globalVar)") + 6) == CE_LUA_SYM_GLOBAL);
        CHECK(!CeLuaScope_DeclaredAnywhere(&g_model, "globalVar"));
    }

    // --- Table fields are not variables -------------------------------------
    {
        const char* const script =
            "local t = {\n"
            "  name = 'x',\n"
            "  [key] = 1,\n"
            "}\n";
        Analyze(script);
        CHECK(KindOf("t") == CE_LUA_SYM_LOCAL);
        CHECK(KindOf("name") == CE_LUA_SYM_FIELD);
        CHECK(KindOf("key") == CE_LUA_SYM_FIELD);
        CHECK(CeLuaScope_Resolve(&g_model, "name", Pos(script, "}")) == CE_LUA_SYM_NONE);
        CHECK(CeLuaScope_Resolve(&g_model, "key", Pos(script, "}")) == CE_LUA_SYM_NONE);
    }

    // --- Multi-line signatures ---------------------------------------------
    {
        const char* const script =
            "function foo(\n"
            "  first,\n"
            "  second)\n"
            "  return first\n"
            "end\n";
        Analyze(script);
        CHECK(KindOf("foo") == CE_LUA_SYM_FUNCTION);
        CHECK(KindOf("first") == CE_LUA_SYM_PARAM);
        CHECK(KindOf("second") == CE_LUA_SYM_PARAM);
    }

    // --- Inline {$lua} directives -------------------------------------------
    {
        const char* const script =
            "{$lua}\n"
            "local afterDirective = 1\n"
            "{$lua} local inlineVar = 2\n";
        Analyze(script);
        CHECK(KindOf("afterDirective") == CE_LUA_SYM_LOCAL);
        CHECK(KindOf("inlineVar") == CE_LUA_SYM_LOCAL);
    }

    // --- Block depth at a position (scope-aware completion ranking) --------
    {
        const char* const script =
            "local a = 1\n"
            "if a then\n"
            "  do\n"
            "    local c = 3\n"
            "    print(c)\n"
            "  end\n"
            "end\n";
        Analyze(script);
        CHECK(CeLuaScope_BlockDepthAt(&g_model, Pos(script, "local a = 1")) == 0);
        CHECK(CeLuaScope_BlockDepthAt(&g_model, Pos(script, "print(c)")) == 2);
        CHECK(CeLuaScope_BlockDepthAt(&g_model, Pos(script, "end\nend") + 3) == 1);
        CHECK(CeLuaScope_BlockDepthAt(&g_model, (DocPos)strlen(script) - 1) == 0);
        CHECK(CeLuaScope_BlockDepthAt(nullptr, 10) == 0);
    }

    // --- Long comments / long strings ---------------------------------------
    {
        const char* const script =
            "--[=[\n"
            "local ghost = 1\n"
            "local alsoGhost = [[hidden]]\n"
            "]=]\n"
            "local real = 2\n";
        Analyze(script);
        CHECK(FindSym("ghost") < 0);
        CHECK(FindSym("alsoGhost") < 0);
        CHECK(KindOf("real") == CE_LUA_SYM_LOCAL);
        CHECK(CeLuaScope_Resolve(&g_model, "real", (DocPos)strlen(script) - 1) == CE_LUA_SYM_LOCAL);
    }

    if (g_failures != 0) {
        std::printf("%d CE Lua scope test(s) failed\n", g_failures);
        return 1;
    }
    std::printf("CE Lua scope tests passed\n");
    return 0;
}
