// encoding: UTF-8
// Unit tests for the pure helpers behind CE user-symbol highlighting and
// inline diagnostics.  TestCeHighlight.ps1 extracts them verbatim from
// src/CeHighlight.c and src/CeDiagnostics.c into CeHighlightHelpers.inc;
// everything else is stubbed here so the test needs neither Scintilla nor a
// running editor.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

typedef long long DocPos;
typedef int       DocLn;

typedef struct _ce_def {
    char  name[64];
    char  kind[16];
    DocLn line;
} CE_DEF;

typedef struct _ce_diag_hint {
    DocLn line;
    int   startCol;
    int   endCol;
    bool  error;
    char  text[96];
} CE_DIAG_HINT;

#include "CeHighlightHelpers.inc"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void CopyStr(char* dst, size_t cch, const char* src) {
    size_t i = 0;
    for (; (i + 1 < cch) && src[i]; ++i) { dst[i] = src[i]; }
    dst[i] = '\0';
}

static void AddDef(CE_DEF* defs, int* count, const char* name) {
    CopyStr(defs[*count].name, COUNTOF(defs[*count].name), name);
    CopyStr(defs[*count].kind, COUNTOF(defs[*count].kind), "alloc");
    defs[*count].line = *count + 1;
    ++(*count);
}

int main()
{
    // --- Identifier rules must match LexCe.cxx IsIdentStart/IsIdentChar -----
    // The host used to fold '.' into a word (the lexer treats it as an
    // operator) and split on '@' (the lexer keeps it), so the indicator was
    // applied to a different token than the keyword classifier saw.
    CHECK(_IsIdentStart('a'));
    CHECK(_IsIdentStart('Z'));
    CHECK(_IsIdentStart('_'));
    CHECK(!_IsIdentStart('0'));
    CHECK(!_IsIdentStart('.'));
    CHECK(!_IsIdentStart('$'));
    CHECK(!_IsIdentStart('@'));
    CHECK(!_IsIdentStart(0x80));
    CHECK(_IsIdentChar('9'));
    CHECK(_IsIdentChar('$'));
    CHECK(_IsIdentChar('@'));
    CHECK(_IsIdentChar('_'));
    CHECK(!_IsIdentChar('.'));
    CHECK(!_IsIdentChar(':'));
    CHECK(!_IsIdentChar(' '));
    CHECK(!_IsIdentChar('/'));

    // --- Case-insensitive, length-delimited name lookup ----------------------
    CHECK(_CompareNameToSpan("alloc", "alloc", 5) == 0);
    CHECK(_CompareNameToSpan("alloc", "ALLOC", 5) == 0);
    CHECK(_CompareNameToSpan("alloc", "allo", 4) > 0);   // name longer
    CHECK(_CompareNameToSpan("alloc", "allocation", 10) < 0); // name shorter
    CHECK(_CompareNameToSpan("alloc", "allox", 5) < 0);
    CHECK(_CompareNameToSpan("alloc", "allob", 5) > 0);

    CE_DEF defs[8] = {};
    int n = 0;
    AddDef(defs, &n, "playerBase");
    AddDef(defs, &n, "newmem");
    AddDef(defs, &n, "hookAddr");
    AddDef(defs, &n, "exit");
    qsort(defs, (size_t)n, sizeof(CE_DEF), _CompareDefByName);
    CHECK(_IsDefName(defs, n, "playerBase", 10));
    CHECK(_IsDefName(defs, n, "PLAYERBASE", 10));
    CHECK(_IsDefName(defs, n, "hookAddr", 8));
    CHECK(_IsDefName(defs, n, "newmem", 6));
    CHECK(_IsDefName(defs, n, "exit", 4));
    CHECK(!_IsDefName(defs, n, "playerBas", 9));
    // With '.' no longer part of a word, "playerBase.value" is scanned as
    // "playerBase" (10 bytes) and matches; the old 16-byte token did not.
    CHECK(!_IsDefName(defs, n, "playerBase.value", 16));
    CHECK(!_IsDefName(defs, n, "unknown", 7));
    CHECK(!_IsDefName(defs, n, "playerBase", 0)); // empty span never matches

    // --- Multi-line comment skipping ----------------------------------------
    {
        const char text[] = "--[[ alloc(foo) ]] label(bar)";
        DocPos const len = (DocPos)(sizeof(text) - 1);
        CHECK(_BlockCommentEnd(text, len, 0, 4, true) == 18);
        CHECK(std::strncmp(text + 18, " label(bar)", 11) == 0);
    }
    {
        const char text[] = "--[=[ alloc(foo) ]=] x";
        DocPos const len = (DocPos)(sizeof(text) - 1);
        CHECK(_BlockCommentEnd(text, len, 0, 5, true) == 20);
    }
    {
        const char text[] = "/* alloc(foo) */ label(bar)";
        DocPos const len = (DocPos)(sizeof(text) - 1);
        CHECK(_BlockCommentEnd(text, len, 0, 2, false) == 16);
        CHECK(std::strncmp(text + 16, " label(bar)", 11) == 0);
    }
    {
        const char text[] = "/* alloc(foo)";
        DocPos const len = (DocPos)(sizeof(text) - 1);
        CHECK(_BlockCommentEnd(text, len, 0, 2, false) == len);
    }

    // --- EOL ghost text: an error must win over a warning on the same line ---
    {
        CE_DIAG_HINT h[3] = {};
        h[0].line = 3; h[0].error = false;
        h[1].line = 3; h[1].error = true;
        h[2].line = 4; h[2].error = false;
        CHECK(_IsPrimaryHint(h, 3, 0));
        CHECK(_IsPrimaryHint(h, 3, 1)); // error overrides an earlier warning
        CHECK(_IsPrimaryHint(h, 3, 2));

        h[0].error = true;
        h[1].error = false;
        CHECK(_IsPrimaryHint(h, 3, 0));
        CHECK(!_IsPrimaryHint(h, 3, 1)); // warning after an error is suppressed
    }

    // --- Every finding on a line must contribute readable ghost text ---------
    {
        CHECK(std::strcmp(_FallbackText(true), "错误") == 0);
        CHECK(std::strcmp(_FallbackText(false), "警告") == 0);

        char line[64] = {};
        _AppendDiagText(line, COUNTOF(line), "第一条", true);
        CHECK(std::strcmp(line, "第一条") == 0);
        _AppendDiagText(line, COUNTOF(line), "第二条", false);
        CHECK(std::strcmp(line, "第一条 | 第二条") == 0);

        char narrow[6] = {}; // only fits "abc" + a partial UTF-8 sequence
        _AppendDiagText(narrow, COUNTOF(narrow), "abc\xE4\xB8\xAD", true);
        CHECK(std::strcmp(narrow, "abc") == 0); // no half-written UTF-8 char

        CE_DIAG_HINT h[4] = {};
        h[0].line = 3; h[0].error = false; CopyStr(h[0].text, COUNTOF(h[0].text), "警告甲");
        h[1].line = 3; h[1].error = true;  // empty message must get a fallback
        h[2].line = 3; h[2].error = false; CopyStr(h[2].text, COUNTOF(h[2].text), "警告丙");
        h[3].line = 4; h[3].error = false; CopyStr(h[3].text, COUNTOF(h[3].text), "别的行");
        char merged[COUNTOF(h[0].text) * 3];
        _MergeLineMessage(h, 4, 1, merged, COUNTOF(merged));
        CHECK(std::strstr(merged, "错误") != nullptr);
        CHECK(std::strstr(merged, "警告甲") != nullptr);
        CHECK(std::strstr(merged, "警告丙") != nullptr);
        CHECK(std::strstr(merged, "别的行") == nullptr);
        _MergeLineMessage(h, 4, 3, merged, COUNTOF(merged));
        CHECK(std::strcmp(merged, "别的行") == 0);
    }

    // --- Ghost text truncation must not split a UTF-8 sequence ---------------
    {
        char out[8] = {};
        _CopyShortText(out, COUNTOF(out), "hello world");
        CHECK(std::strcmp(out, "hello w") == 0);

        char utf8[6] = {};
        _CopyShortText(utf8, COUNTOF(utf8), "abc\xE4\xB8\xAD");
        CHECK(std::strcmp(utf8, "abc") == 0); // whole 3-byte char dropped

        char nl[16] = {};
        _CopyShortText(nl, COUNTOF(nl), "first\r\nsecond");
        CHECK(std::strcmp(nl, "first") == 0);

        char empty[4] = { 'x', 'x', 'x', 'x' };
        _CopyShortText(empty, COUNTOF(empty), nullptr);
        CHECK(empty[0] == '\0');
    }

    if (g_failures != 0) {
        std::printf("%d CE highlight/diagnostics test(s) failed\n", g_failures);
        return 1;
    }
    std::printf("CE highlight/diagnostics tests passed\n");
    return 0;
}
