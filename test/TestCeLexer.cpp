// encoding: UTF-8
// Regression harness for the CE lexer's inline-language directive parser.
// TestCeLexer.ps1 extracts DirectiveAt() verbatim from lexilla/lexers_x/LexCe.cxx
// and supplies only the LexAccessor surface that function uses.

#include <cstdio>
#include <cstring>
#include <string>

using Sci_PositionU = unsigned int;

constexpr int ModeLua = 0;
constexpr int ModeAA = 1;
constexpr int ModeC = 2;
constexpr int SCE_CE_WORD6 = 15;
constexpr int SCE_CE_PREPROCESSOR = 16;

struct LexAccessor {
    const std::string text;
    const std::string styles;
    explicit LexAccessor(const char* s, const char* st = nullptr)
        : text(s), styles(st ? st : std::string(text.size(), '\0')) {}
    int SafeGetCharAt(Sci_PositionU pos, int fallback) const
    {
        return (pos < text.size()) ? static_cast<unsigned char>(text[pos]) : fallback;
    }
    int StyleAt(Sci_PositionU pos) const
    {
        return (pos < styles.size()) ? static_cast<unsigned char>(styles[pos]) : 0;
    }
};

#include "CeLexerHelpers.inc"

static int g_failures = 0;
#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                 \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static void CheckDirective(const char* text, int expectedMode)
{
    LexAccessor styler(text);
    int mode = -1;
    int const len = DirectiveAt(styler, 0u, mode);
    CHECK(len == static_cast<int>(std::strlen(text)));
    CHECK(mode == expectedMode);
}

static void CheckFoldMarker(const char* text, const char* styles, bool expected)
{
    LexAccessor styler(text, styles);
    CHECK(IsCeFoldMarker(styler, 0u, static_cast<Sci_PositionU>(std::strlen(text))) == expected);
}

int main()
{
    CheckDirective("{$lua}", ModeLua);
    CheckDirective("{$asm}", ModeAA);
    CheckDirective("{$c}", ModeC);
    CheckDirective("{$ccode}", ModeC);
    CheckDirective("{$CODE}", ModeC);
    CheckDirective("{$code}", ModeC);
    CheckDirective("{$STDCALL}", ModeC);
    CheckDirective("{$fastcode}", ModeC);

    CheckFoldMarker("[ENABLE]", "\x0F.......", true);
    CheckFoldMarker("  {$lua}", "..\x10....", true);
    CheckFoldMarker("alloc(x,8)", "...........", false);
    CheckFoldMarker("-- [ENABLE]", "..\x0F.......", false);

    if (g_failures != 0) {
        std::printf("%d CE lexer test(s) failed\n", g_failures);
        return 1;
    }
    std::printf("CE lexer tests passed\n");
    return 0;
}
