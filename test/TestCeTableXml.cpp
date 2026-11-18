// encoding: UTF-8
// Regression harness for CE table XML tag extraction.
// TestCeTableXml.ps1 extracts the real tag helpers from src/CeTableXml.c.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
#define HEAP_ZERO_MEMORY 0x00000008u

using DWORD = unsigned long;

static char* AllocMem(size_t bytes, DWORD)
{
    return static_cast<char*>(std::calloc(1, bytes));
}

static void FreeMem(void* mem)
{
    std::free(mem);
}

static int HarnessStrNICmp(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        unsigned char const ca = (unsigned char)a[i];
        unsigned char const cb = (unsigned char)b[i];
        int const la = (ca >= 'A' && ca <= 'Z') ? (ca + 32) : ca;
        int const lb = (cb >= 'A' && cb <= 'Z') ? (cb + 32) : cb;
        if (la != lb) {
            return la - lb;
        }
        if (ca == '\0') {
            return 0;
        }
    }
    return 0;
}

#define _strnicmp HarnessStrNICmp

#include "CeTableXmlHelpers.inc"

static int g_failures = 0;
#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                 \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static void ExpectBody(const char* xml, const char* want)
{
    char* const body = _ExtractTag(xml, std::strlen(xml), "AssemblerScript");
    if (want == nullptr) {
        CHECK(body == nullptr);
    }
    else {
        CHECK(body != nullptr);
        if (body != nullptr) {
            CHECK(std::strcmp(body, want) == 0);
        }
    }
    FreeMem(body);
}

int main()
{
    ExpectBody("<AssemblerScript>mov eax,1</AssemblerScript>", "mov eax,1");
    ExpectBody("<ASSEMBLERSCRIPT CASE=\"1\">x</assemblerscript>", "x");

    // A longer tag name must not satisfy the AssemblerScript tag.
    ExpectBody("<AssemblerScriptEx>wrong</AssemblerScriptEx>", nullptr);
    ExpectBody("<AssemblerScript>real</AssemblerScriptEx>", nullptr);

    if (g_failures != 0) {
        std::printf("%d CE table XML test(s) failed\n", g_failures);
        return 1;
    }
    std::printf("CE table XML tests passed\n");
    return 0;
}
