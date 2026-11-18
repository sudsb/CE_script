// LSP protocol layer regression harness.
//
// The functions under test are the real product sources compiled as C
// (src/CeLspJson.c, CeLspFraming.c, CeLspPos.c) by TestCeLsp.ps1.  This file
// must not include any Notepad3 or Win32 header, so the harness runs on any
// machine with the Visual C++ build tools.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using ULONGLONG = unsigned long long;
using UINT = unsigned int;

#define CELSP_RETRY_COOLDOWN_MS 2000u
#define CELSP_RETRY_MAX_BACKOFF_MS 30000u

#include "CeLspJson.h"
#include "CeLspFraming.h"
#include "CeLspPos.h"

#include "CeLspSessionHelpers.inc"

static int g_failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static void TestJson()
{
    const char text[] =
        "{\"name\":\"CE\",\"count\":2426,\"ok\":true,\"nil\":null,"
        "\"items\":[1,2.5,\"\\u4F60\\u597D\",\"a\\nb\"],"
        "\"emoji\":\"\\uD83D\\uDE00\"}";
    CELSP_JSON* const root = CeLspJson_Parse(text, 0u);
    CHECK(root != nullptr);
    if (!root) { return; }
    CHECK(std::strcmp(CeLspJson_GetString(root, "name"), "CE") == 0);
    CHECK(CeLspJson_GetInt(root, "count") == 2426);
    CHECK(CeLspJson_GetBool(root, "ok"));
    CHECK(CeLspJson_Get(root, "nil") != nullptr);
    CHECK(CeLspJson_Type(CeLspJson_Get(root, "nil")) == CELSP_JSON_NULL);
    const CELSP_JSON* const items = CeLspJson_Get(root, "items");
    CHECK(CeLspJson_Count(items) == 4u);
    CHECK(CeLspJson_Number(CeLspJson_At(items, 0)) == 1.0);
    CHECK(CeLspJson_Number(CeLspJson_At(items, 1)) == 2.5);
    CHECK(std::strcmp(CeLspJson_String(CeLspJson_At(items, 2)), "\xE4\xBD\xA0\xE5\xA5\xBD") == 0);
    CHECK(std::strcmp(CeLspJson_String(CeLspJson_At(items, 3)), "a\nb") == 0);
    const char* const emoji = CeLspJson_GetString(root, "emoji");
    CHECK(emoji && (std::strcmp(emoji, "\xF0\x9F\x98\x80") == 0));
    CeLspJson_Free(root);

    CHECK(CeLspJson_Parse("{bad", 0u) == nullptr);
    CHECK(CeLspJson_Parse("{\"a\":1} trailing", 0u) == nullptr);
    CHECK(CeLspJson_Parse("[1,2,", 0u) == nullptr);
    CHECK(CeLspJson_Parse(nullptr, 0u) == nullptr);

    // Build + serialize round trip, including escaping.
    CELSP_JSON* const obj = CeLspJson_NewObject();
    CELSP_JSON* const arr = CeLspJson_NewArray();
    CHECK(obj && arr);
    CHECK(CeLspJson_Set(obj, "text", CeLspJson_NewString("quote\" back\\ tab\t")));
    CHECK(CeLspJson_Set(obj, "flag", CeLspJson_NewBool(false)));
    CHECK(CeLspJson_Add(arr, CeLspJson_NewNumber(7.0)));
    CHECK(CeLspJson_Add(arr, CeLspJson_NewString("\xE4\xB8\xAD\xE6\x96\x87")));
    CHECK(CeLspJson_Set(obj, "arr", arr));
    char* const serialized = CeLspJson_Write(obj);
    CHECK(serialized != nullptr);
    if (serialized) {
        CELSP_JSON* const again = CeLspJson_Parse(serialized, 0u);
        CHECK(again != nullptr);
        if (again) {
            CHECK(std::strcmp(CeLspJson_GetString(again, "text"), "quote\" back\\ tab\t") == 0);
            CHECK(CeLspJson_Int(CeLspJson_Get(CeLspJson_Get(again, "arr"), "")) == 0);
            CHECK(CeLspJson_Int(CeLspJson_At(CeLspJson_Get(again, "arr"), 0)) == 7);
            CHECK(std::strcmp(CeLspJson_String(CeLspJson_At(CeLspJson_Get(again, "arr"), 1)),
                              "\xE4\xB8\xAD\xE6\x96\x87") == 0);
            CeLspJson_Free(again);
        }
        std::free(serialized);
    }
    CeLspJson_Free(obj);

    char* const quoted = CeLspJson_Quote("a\001b");
    CHECK(quoted && (std::strstr(quoted, "\\u0001") != nullptr));
    std::free(quoted);
}

static void TestFraming()
{
    const std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"hover\",\"params\":{\"t\":\"\xE4\xB8\xAD\"}}";
    size_t frameLen = 0u;
    char* const frame = CeLspFraming_Encode(payload.c_str(), payload.size(), &frameLen);
    CHECK(frame != nullptr);
    if (!frame) { return; }
    CHECK(frameLen > payload.size());
    CHECK(std::strncmp(frame, "Content-Length: ", 16u) == 0);
    const int hdr = CeLspFraming_HeaderEnd(frame, frameLen);
    CHECK(hdr > 0);
    size_t contentLen = 0u;
    CHECK(CeLspFraming_ContentLength(frame, (size_t)hdr, &contentLen));
    CHECK(contentLen == payload.size()); // byte length, not character count
    CHECK(std::memcmp(frame + hdr, payload.data(), payload.size()) == 0);
    CHECK(CeLspFraming_HeaderEnd("Content-Length: 1\r\n", 19u) < 0);
    CHECK(CeLspFraming_ContentLength("Content-Type: x\r\n\r\n", 19u, &contentLen) == false);
    for (const char* bad : {"Content-Length: 10abc\r\n\r\n", "Content-Length: +10\r\n\r\n",
                            "Content-Length: -1\r\n\r\n"}) {
        int const badHdr = CeLspFraming_HeaderEnd(bad, std::strlen(bad));
        CHECK(badHdr > 0);
        CHECK(!CeLspFraming_ContentLength(bad, static_cast<size_t>(badHdr), &contentLen));
    }
    std::free(frame);

    char* const req = CeLspFraming_Request(42, "textDocument/completion", nullptr);
    CHECK(req != nullptr);
    if (req) {
        CELSP_JSON* const parsed = CeLspJson_Parse(req, 0u);
        CHECK(parsed != nullptr);
        if (parsed) {
            CHECK(CeLspJson_GetInt(parsed, "id") == 42);
            CHECK(std::strcmp(CeLspJson_GetString(parsed, "method"), "textDocument/completion") == 0);
            CHECK(std::strcmp(CeLspJson_GetString(parsed, "jsonrpc"), "2.0") == 0);
            CeLspJson_Free(parsed);
        }
        std::free(req);
    }
    char* const notification = CeLspFraming_Notification("exit", nullptr);
    CHECK(notification && (CeLspJson_Get(CeLspJson_Parse(notification, 0u), "id") == nullptr));
    std::free(notification);

    char* const err = CeLspFraming_Error(7, -32601, "Method not found");
    CHECK(err != nullptr);
    if (err) {
        CELSP_JSON* const parsed = CeLspJson_Parse(err, 0u);
        CHECK(parsed && (CeLspJson_GetInt(CeLspJson_Get(parsed, "error"), "code") == -32601));
        CeLspJson_Free(parsed);
        std::free(err);
    }
    size_t encodedLen = 0u;
    char* const encoded = CeLspFraming_EncodeRequest(9, "initialize", nullptr, &encodedLen);
    CHECK(encoded && (encodedLen > 20u));
    std::free(encoded);
}

static void TestPositions()
{
    const char text[] = "local a = 1\n-- \xE4\xB8\xAD\xE6\x96\x87\r\nprint(a)\n";
    const size_t len = std::strlen(text);

    int line = 0, character = 0;
    CeLspPos_FromOffset(text, len, 0u, false, &line, &character);
    CHECK((line == 0) && (character == 0));
    const size_t secondLine = (size_t)(std::strchr(text, '\n') - text) + 1u;
    CeLspPos_FromOffset(text, len, secondLine, false, &line, &character);
    CHECK((line == 1) && (character == 0));
    CeLspPos_FromOffset(text, len, secondLine + 9u, false, &line, &character);
    CHECK((line == 1) && (character == 5)); // "-- " + 2 Chinese characters
    CHECK(CeLspPos_ToOffset(text, len, 1, 5, false) == secondLine + 9u);

    // UTF-16 code units: a supplementary character counts as two.
    const char supplementary[] = "a\xF0\x9F\x98\x80" "b";
    CeLspPos_FromOffset(supplementary, std::strlen(supplementary), 5u, false, &line, &character);
    CHECK(character == 2);
    CeLspPos_FromOffset(supplementary, std::strlen(supplementary), 5u, true, &line, &character);
    CHECK(character == 3);
    CHECK(CeLspPos_ToOffset(supplementary, std::strlen(supplementary), 0, 3, true) == 5u);

    CHECK(CeLspPos_WordStart("abc.def", 7u, 7u) == 0u);
    CHECK(CeLspPos_WordStart("abc def", 7u, 7u) == 4u);
    CHECK(CeLspPos_WordEnd("abc def", 7u, 0u) == 3u);
    CHECK(!CeLspPos_IsIdentChar(' '));
    CHECK(CeLspPos_IsIdentChar('_'));
}

static void TestSectionOpener()
{
    // WordStart() stops right after "{$" when '$' is not a word character...
    CHECK(CeLspPos_SectionOpenerWidth('{', '$', 'l') == 2);
    // ...and between '{' and '$' when '$' is a word character (our default).
    CHECK(CeLspPos_SectionOpenerWidth('\n', '{', '$') == 1);
    CHECK(CeLspPos_SectionOpenerWidth('\0', '{', '$') == 1);
    // Ordinary words, Lua table constructors and stray '{' are left alone.
    CHECK(CeLspPos_SectionOpenerWidth(' ', 'a', 'b') == 0);
    CHECK(CeLspPos_SectionOpenerWidth('=', ' ', '{') == 0);
    CHECK(CeLspPos_SectionOpenerWidth('\0', '\0', 'x') == 0);

    // End to end: with '$' inside the word, the normaliser must yield "{$l".
    const char text[] = "[ENABLE]\n{$l";
    const size_t len = std::strlen(text);
    size_t start = CeLspPos_WordStart(text, len, len);
    CHECK(start == 10u);
    char const left2 = (start >= 2u) ? text[start - 2u] : '\0';
    char const left1 = (start >= 1u) ? text[start - 1u] : '\0';
    char const first = (start < len) ? text[start] : '\0';
    int const width = CeLspPos_SectionOpenerWidth(left2, left1, first);
    CHECK(width == 1);
    start -= (size_t)width;
    CHECK(std::strncmp(text + start, "{$l", 3u) == 0);
}

static void TestUri()
{
    char* const uri = CeLspPos_UriFromPath("C:/my dir/\xE4\xB8\xAD\xE6\x96\x87/sample.cea");
    CHECK(uri != nullptr);
    if (uri) {
        CHECK(std::strncmp(uri, "file:///C:/my%20dir/", 20u) == 0);
        CHECK(std::strstr(uri, "%E4%B8%AD%E6%96%87") != nullptr);
        char path[512];
        CHECK(CeLspPos_PathFromUri(uri, path, sizeof(path)));
        CHECK(std::strstr(path, "my dir") != nullptr);
        CHECK(std::strstr(path, "\xE4\xB8\xAD\xE6\x96\x87") != nullptr);
        std::free(uri);
    }
    char* const unc = CeLspPos_UriFromPath("\\\\server\\share\\x.cea");
    CHECK(unc && (std::strncmp(unc, "file://server/share/x.cea", 25u) == 0));
    if (unc) {
        char uncPath[512];
        CHECK(CeLspPos_PathFromUri(unc, uncPath, sizeof(uncPath)));
        CHECK(std::strcmp(uncPath, "\\\\server\\share\\x.cea") == 0);
    }
    std::free(unc);
    char out[16];
    CHECK(!CeLspPos_PathFromUri("http://example.com", out, sizeof(out)));
}

static void TestInsertionPosition()
{
    int line = -1;
    int character = -1;
    _Celsp_InsertionPosition("ab\n\tcd", 6u, 4u, &line, &character);
    CHECK(line == 1);
    CHECK(character == 1); // tab is one LSP code point, not an expanded column

    _Celsp_InsertionPosition("ab\n\tcd", 6u, 6u, &line, &character);
    CHECK(line == 1);
    CHECK(character == 3);
    CHECK(_Celsp_AllowIncremental(true, false, false, 0, true));
    CHECK(!_Celsp_AllowIncremental(true, false, false, 1, true));
    CHECK(!_Celsp_AllowIncremental(true, true, false, 0, true));
    CHECK(!_Celsp_AllowIncremental(true, false, true, 0, true));
    CHECK(!_Celsp_AllowIncremental(true, false, false, 0, false));
}

static void TestSessionPolicy()
{
    // A fresh client may start; a failed one must cool down before retrying.
    CHECK(_Celsp_ShouldRetryStart(1000ull, 0ull, 0));
    CHECK(!_Celsp_ShouldRetryStart(1500ull, 1000ull, 0));
    CHECK(_Celsp_ShouldRetryStart(3000ull, 1000ull, 0));
    CHECK(!_Celsp_ShouldRetryStart(5000ull, 1000ull, 3));
    CHECK(!_Celsp_ShouldRetryStart(9000ull, 1000ull, 3));   // 3 restarts -> 16s backoff
    CHECK(_Celsp_ShouldRetryStart(17000ull, 1000ull, 3));

    // Diagnostics belong to one document and one server generation.
    const char uri[] = "file:///c%3A/scripts/test.lua";
    const char other[] = "file:///c%3A/scripts/other.lua";
    CHECK(_Celsp_DiagIsCurrent(uri, 3, 2, uri, 3, 2));
    CHECK(!_Celsp_DiagIsCurrent(uri, 3, 1, uri, 3, 2));
    CHECK(!_Celsp_DiagIsCurrent(other, 3, 2, uri, 3, 2));
    CHECK(!_Celsp_DiagIsCurrent(uri, 2, 2, uri, 3, 2));
    CHECK(!_Celsp_DiagIsCurrent(nullptr, 3, 2, uri, 3, 2));

    CHECK(_Celsp_MessageIsCurrent(7u, 7u));
    CHECK(!_Celsp_MessageIsCurrent(7u, 8u));
    CHECK(_Celsp_RequestExpired(1000ull, 999ull));
    CHECK(!_Celsp_RequestExpired(1000ull, 1000ull));
    CHECK(!_Celsp_RequestExpired(1000ull, 1001ull));
    {
        const int kinds[] = { 1, 2, 2 };
        const ULONGLONG deadlines[] = { 500ull, 100ull, 200ull };
        CHECK(_Celsp_PickPendingDrop(3, kinds, deadlines, 2) == 1);
        CHECK(_Celsp_PickPendingDrop(3, kinds, deadlines, 9) == 1);
    }
    {
        int s = 0;
        int e = 0;
        CHECK(_Celsp_MergeChangeRange(&s, &e, 2, 5, 0));
        CHECK(s == 2 && e == 5);
        CHECK(_Celsp_MergeChangeRange(&s, &e, 6, 10, 0));
        CHECK(s == 2 && e == 10);
        CHECK(!_Celsp_MergeChangeRange(&s, &e, 20, 22, 0));
        CHECK(s == 2 && e == 10);
    }
    CHECK(_Celsp_DiagSeverityForSquiggle(1));
    CHECK(_Celsp_DiagSeverityForSquiggle(2));
    CHECK(!_Celsp_DiagSeverityForSquiggle(3));
    CHECK(!_Celsp_DiagSeverityForSquiggle(4));
}

int main()
{
    TestJson();
    TestFraming();
    TestPositions();
    TestSectionOpener();
    TestUri();
    TestInsertionPosition();
    TestSessionPolicy();
    if (g_failures != 0) {
        std::printf("TestCeLsp: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("TestCeLsp: all checks passed\n");
    return 0;
}
