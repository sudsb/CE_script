// encoding: UTF-8
// End-to-end lint regression harness.  TestCeLintIntegration.ps1 compiles the
// real src/CeLint.c and src/CeScript.c against these fake editor services.

#include "Helpers.h"
#include "SciCall.h"
#include "Styles.h"
#include "CeScript.h"
#include "CeLint.h"
#include "CeDiagnostics.h"

// ---------------------------------------------------------------------------
// Fake editor
// ---------------------------------------------------------------------------

SETTINGS2 Settings2;
EDITLEXER lexCE;

static const char* g_text;
static DocPos g_len;
static DocPos g_caret;
static int g_ceInitialMode = 0;

EDITLEXER* Style_GetCurrentLexerPtr(void) { return &lexCE; }
DocPos Sci_GetDocEndPosition(void) { return g_len; }
const char* SciCall_GetRangePointer(DocPos start, DocPos len) {
    (void)len;
    return g_text + start;
}
DocLn SciCall_LineFromPosition(DocPos pos) {
    DocLn line = 0;
    for (DocPos i = 0; (i < pos) && (i < g_len); ++i) {
        if (g_text[i] == '\n') { ++line; }
    }
    return line;
}
DocPos SciCall_GetCurrentPos(void) { return g_caret; }
int SciCall_GetPropertyInt(const char* key, int defaultValue) {
    if (key && (strcmp(key, "lexer.ce.initial") == 0)) {
        return g_ceInitialMode;
    }
    return defaultValue;
}
int SciCall_GetEOLMode(void) { return SC_EOL_CRLF; }
void SciCall_BeginUndoAction(void) {}
void SciCall_EndUndoAction(void) {}
void SciCall_SetSel(DocPos anchor, DocPos caret) { (void)anchor; g_caret = caret; }
void SciCall_ReplaceSel(const char* text) { (void)text; }
void SciCall_SetEmptySelection(DocPos pos) { g_caret = pos; }
void SciCall_ScrollCaret(void) {}

void* AllocMem(size_t cb, DWORD flags) { (void)flags; return calloc(1, cb); }
void FreeMem(void* p) { free(p); }

void CeDiagnostics_Init(void) {}
void CeDiagnostics_Clear(void) {}
void CeDiagnostics_Begin(void) {}
void CeDiagnostics_Add(DocLn line, int startCol, int endCol, bool error, const char* text) {
    (void)line; (void)startCol; (void)endCol; (void)error; (void)text;
}
void CeDiagnostics_AddLine(DocLn line, bool error, const char* text) {
    (void)line; (void)error; (void)text;
}
void CeDiagnostics_End(void) {}
void CeDiagnostics_RefreshStyles(void) {}

// The real implementation lives in CeCompletion.c; the generated keyword
// tables are compiled into this harness, so the membership test is exact.
extern const char CE_KEYWORDS_LUA[];
extern const char CE_KEYWORDS_API[];
extern const char CE_KEYWORDS_AA[];
extern const char CE_KEYWORDS_ASM[];
extern const char CE_KEYWORDS_TYPES[];

static bool InList(const char* list, const char* word) {
    if (!list || !word || !*word) { return false; }
    const size_t len = strlen(word);
    const char* p = list;
    while (*p) {
        while (*p == ' ') { ++p; }
        const char* const start = p;
        while (*p && *p != ' ') { ++p; }
        if (((size_t)(p - start) == len) && (_strnicmp(start, word, len) == 0)) {
            return true;
        }
    }
    return false;
}

bool CeCompletion_IsBuiltinKeyword(const char* name) {
    return InList(CE_KEYWORDS_ASM, name) || InList(CE_KEYWORDS_AA, name) ||
           InList(CE_KEYWORDS_TYPES, name) || InList(CE_KEYWORDS_LUA, name) ||
           InList(CE_KEYWORDS_API, name);
}

// ---------------------------------------------------------------------------

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void Run(const char* text, CE_LINT* lint) {
    g_text = text;
    g_len = (DocPos)strlen(text);
    g_caret = 0;
    Settings2.CeLintSemantic = true;
    CeLint_Run(lint);
}

static void Dump(const char* title, const CE_LINT* lint) {
    printf("[%s] count=%d\n", title, lint->count);
    for (int i = 0; i < lint->count; ++i) {
        const CE_DIAG* const d = &lint->items[i];
        printf("  line=%d col=%d len=%d %s : %s\n",
               d->line, d->col, d->len, d->error ? "ERROR" : "warn ", d->message);
    }
}

static bool HasMessage(const CE_LINT* lint, const char* needle) {
    for (int i = 0; i < lint->count; ++i) {
        if (strstr(lint->items[i].message, needle)) { return true; }
    }
    return false;
}

static void RunModes(const char* text, unsigned char* modes, int count) {
    g_text = text;
    g_len = (DocPos)strlen(text);
    g_caret = 0;
    CE_DOC doc;
    if (!CeScript_InitDoc(&doc)) {
        return;
    }
    if (doc.lines < count) {
        count = (int)doc.lines;
    }
    memset(modes, 0xFF, (size_t)count);
    CeScript_LineModes(&doc, modes);
    CeScript_FreeDoc(&doc);
}

int main(void)
{
    CE_LINT lint;

    // The exact script from the screenshot: a plain Lua table constructor.
    // It is valid Lua and must not produce any finding.
    const char* const table =
        "local m = {\n"
        "    [1] = \"hello\",\n"
        "    [2] = \"world\"\n"
        "}\n";
    Run(table, &lint);
    Dump("lua table", &lint);
    CHECK(lint.count == 0);

    // A genuinely misspelled section marker is still reported.
    Run("[ENABLE]\n[ENABL]\n[DISABLE]\n", &lint);
    Dump("bad marker", &lint);
    CHECK(HasMessage(&lint, "未知的段标记"));

    // A real AA script with a missing 'end' and a bad bracket still reports.
    Run("{$lua}\nif x then\n  print(x)\n", &lint);
    Dump("missing end", &lint);
    CHECK(HasMessage(&lint, "缺少 end"));

    // CE writes the table header as an AA "{ ... }" block comment.  Its
    // contents are not Lua, so names such as "GraveyardKeeper2" must not be
    // reported as undefined variables.
    const char* const header =
        "{\n"
        "  游戏   : GraveyardKeeper2.exe\n"
        "  版本   : \n"
        "  日期   : 2026-11-09\n"
        "  作者   : Administrator\n"
        "}\n"
        "\n"
        "[ENABLE]\n"
        "nop\n"
        "[DISABLE]\n"
        "nop\n";
    Run(header, &lint);
    Dump("aa header comment", &lint);
    CHECK(!HasMessage(&lint, "未定义的变量"));
    CHECK(lint.count == 0);

    // CE's own aobscan output is unquoted; it is valid Auto Assembler.
    Run("[ENABLE]\n"
        "aobscan(INJECT,F3 0F 10 45 A0 F3 0F 5A C0 E9 ** ** ** **)\n"
        "aobscanmodule(MODULE,Trae CN.exe,CC CC CC 48 83 EC 28 E8 ?? ?? ?? ??)\n"
        "[DISABLE]\n", &lint);
    Dump("unquoted aob", &lint);
    CHECK(!HasMessage(&lint, "引号"));
    CHECK(!HasMessage(&lint, "无效的字节"));

    // CE 7.7+ hook/unhook and the default template's "label(return)".
    Run("[ENABLE]\n"
        "alloc(newmem,2048)\n"
        "label(originalcode)\n"
        "label(return)\n"
        "hook(\"Trae CN.exe\"+53C5CCD,newmem,originalcode)\n"
        "originalcode:\n"
        "  jmp \"Trae CN.exe\"+53C5B4C\n"
        "return:\n"
        "[DISABLE]\n"
        "unhook(\"Trae CN.exe\"+53C5CCD)\n"
        "dealloc(newmem)\n", &lint);
    Dump("ce 7.7 hook", &lint);
    CHECK(!HasMessage(&lint, "未知指令"));
    CHECK(!HasMessage(&lint, "与 CE 关键字冲突"));
    CHECK(lint.count == 0);

    // Without aobscan a lone hook(...) is valid: CE only pairs it with unhook
    // when the target is located by aobscan*, so no "missing unhook" is raised.
    Run("[ENABLE]\n"
        "alloc(newmem,2048)\n"
        "label(originalcode)\n"
        "hook(\"Trae CN.exe\"+53C5CCD,newmem,originalcode)\n"
        "originalcode:\n"
        "  jmp \"Trae CN.exe\"+53C5B4C\n"
        "[DISABLE]\n"
        "dealloc(newmem)\n", &lint);
    Dump("hook without unhook", &lint);
    CHECK(!HasMessage(&lint, "unhook"));
    CHECK(lint.count == 0);

    // The complete CE-generated template: unquoted aobscanmodule, alloc pair,
    // default labels and the trailing "{ original code }" comment block.
    Run("{\n  Game : Trae CN.exe\n}\n\n"
        "[ENABLE]\n\n"
        "aobscanmodule(INJECT,Trae CN.exe,CC CC CC 48 83 EC 28 E8 ** ** ** **) // should be unique\n"
        "alloc(newmem,$1000,INJECT)\n\n"
        "label(code)\n"
        "label(return)\n\n"
        "newmem:\n\n"
        "code:\n"
        "  jmp \"Trae CN.exe\"+53C5B4C\n"
        "  jmp return\n\n"
        "INJECT+10:\n"
        "  jmp newmem\n"
        "return:\n"
        "registersymbol(INJECT)\n\n"
        "[DISABLE]\n\n"
        "INJECT+10:\n"
        "  db E9 7A FE FF FF\n\n"
        "unregistersymbol(INJECT)\n"
        "dealloc(newmem)\n\n"
        "{\n"
        "// 原始代码 - 注入点位置:  Trae CN.exe+53C5CCD\n"
        "Trae CN.exe+53C5CB4: 90                             - nop\n"
        "Trae CN.exe+53C5CD4: 48 89 5C 24 18                 - mov [rsp+18],rbx\n"
        "}\n", &lint);
    Dump("ce template", &lint);
    CHECK(lint.count == 0);

    // Mixed inline blocks must be balanced independently: one {$asm} cannot
    // close both a Lua block and a C block.
    Run("{$lua}\n{$ccode}\n{$asm}\n", &lint);
    Dump("mixed blocks", &lint);
    CHECK(HasMessage(&lint, "缺少 {$asm}"));

    // CE's C-block aliases must participate in the same pairing check.
    Run("{$code}\nint x;\n", &lint);
    Dump("code alias", &lint);
    CHECK(HasMessage(&lint, "缺少 {$asm}"));

    // Directive-like text inside string literals and comments is inert.
    Run("local s = \"{$lua}\"\n-- {$lua}\n", &lint);
    Dump("directive text in comments", &lint);
    CHECK(!HasMessage(&lint, "缺少 {$asm}"));

    // Section markers inside an inline C block are C source, not AA section
    // switches; the host-side mode must follow the lexer's ModeC guard.
    {
        unsigned char modes[8] = { 0 };
        RunModes("{$ccode}\n[ENABLE]\nint x;\n", modes, 3);
        CHECK(modes[0] == CE_LANG_C);
        CHECK(modes[1] == CE_LANG_C);
        CHECK(modes[2] == CE_LANG_C);
    }

    // CE also spells inline C blocks {$code}/{$stdc}/{$stdcall}/{$fastcode}.
    {
        unsigned char modes[4] = { 0 };
        RunModes("{$code}\nint x;\n", modes, 2);
        CHECK(modes[0] == CE_LANG_C);
        CHECK(modes[1] == CE_LANG_C);
    }

    // Duplicate detection must not silently stop after a fixed 64 names.
    {
        char script[8192] = { 0 };
        char line[80] = { 0 };
        StringCchCatA(script, COUNTOF(script), "[ENABLE]\n");
        for (int i = 0; i <= 64; ++i) {
            StringCchPrintfA(line, COUNTOF(line), "alloc(a%d,8)\n", i);
            StringCchCatA(script, COUNTOF(script), line);
        }
        StringCchCatA(script, COUNTOF(script), "alloc(a64,8)\n[DISABLE]\n");
        Run(script, &lint);
        CHECK(HasMessage(&lint, "重复 alloc"));
    }

    // A short identifier beginning with 'j' is not necessarily a jump.
    Run("[ENABLE]\njob nowhere\n[DISABLE]\n", &lint);
    CHECK(!HasMessage(&lint, "跳转目标未定义"));

    // The extension default drives the first line's language: .aa/.cea start
    // as Auto Assembler, .lua starts as Lua (property 1 / 0 from the lexer).
    {
        unsigned char modes[4] = { 0 };
        g_ceInitialMode = 1;
        RunModes("alloc(x,8)\n", modes, 1);
        CHECK(modes[0] == (unsigned char)CE_LANG_AA);
        g_ceInitialMode = 0;
        RunModes("print('x')\n", modes, 1);
        CHECK(modes[0] == (unsigned char)CE_LANG_LUA);
    }

    if (g_failures != 0) {
        printf("%d CE lint integration test(s) failed\n", g_failures);
        return 1;
    }
    printf("CE lint integration tests passed\n");
    return 0;
}
