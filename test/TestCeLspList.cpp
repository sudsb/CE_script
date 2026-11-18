// Regression harness for the AutoC list builder in src/CeLspFeatures.c.
//
// _ListAdd() must write at the length it already knows.  Recomputing
// strlen() after the memcpy scans into stale/uninitialised bytes (the copied
// text has no NUL) and places the terminator past the reserved space, which
// corrupted the heap once a long completion list was followed by a short one.
//
// TestCeLspList.ps1 extracts _ListEnsure() and _ListAdd() verbatim.

#include <cstdio>
#include <cstdlib>
#include <cstring>

static char*  s_list = NULL;
static size_t s_listCap = 0u;
static size_t s_listLen = 0u;

#include "CeLspListHelpers.inc"

static int g_failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static void ResetList(void)
{
    s_listLen = 0u;
    if (!_ListEnsure(2u)) {
        return;
    }
    s_list[0] = ' ';
    s_list[1] = '\0';
    s_listLen = 1u;
}

int main()
{
    // A fresh list starts with the separator AutoCShow skips.
    s_list = NULL;
    s_listCap = 0u;
    s_listLen = 0u;
    CHECK(_ListAdd("alloc", 5u));
    CHECK(std::strcmp(s_list, " alloc") == 0);
    CHECK(_ListAdd("?3 ", 3u));
    CHECK(std::strcmp(s_list, " alloc?3 ") == 0);
    CHECK(std::strlen(s_list) == s_listLen);

    // Grow a long list the way a '.' trigger does (1200 items, long text).
    s_list = NULL;
    s_listCap = 0u;
    s_listLen = 0u;
    ResetList();
    char item[200];
    std::memset(item, 'x', sizeof(item) - 1u);
    item[sizeof(item) - 1u] = '\0';
    for (int i = 0; i < 90; ++i) {
        CHECK(_ListAdd(item, sizeof(item) - 1u));
    }
    const size_t longLen = std::strlen(s_list);
    CHECK(longLen > 8192u); // the buffer has been grown at least once
    CHECK(longLen == s_listLen); // tracked length stayed in sync

    // Simulate the uninitialised tail that made the old code read wild: every
    // byte after the current text is non-zero up to the allocation end.
    if (longLen + 1u < s_listCap) {
        std::memset(s_list + longLen + 1u, 0xCD, s_listCap - longLen - 1u);
    }

    // Next keystroke rebuilds with a short list: the first item must land at
    // offset 1 and the string must end exactly after it.
    ResetList();
    CHECK(std::strlen(s_list) == 1u);
    CHECK(_ListAdd("label", 5u));
    CHECK(std::strlen(s_list) == 6u); // " label"
    CHECK(std::strcmp(s_list, " label") == 0);
    CHECK(_ListAdd("?3 ", 3u));
    CHECK(std::strlen(s_list) == 9u); // " label?3 "
    CHECK(std::strcmp(s_list, " label?3 ") == 0);

    // A third cycle must stay consistent as well (no frontier creep).
    ResetList();
    CHECK(_ListAdd("x", 1u));
    CHECK(std::strlen(s_list) == 2u);
    CHECK(std::strcmp(s_list, " x") == 0);

    std::free(s_list);
    s_list = NULL;
    s_listCap = 0u;
    s_listLen = 0u;

    if (g_failures != 0) {
        std::printf("TestCeLspList: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("TestCeLspList: all checks passed\n");
    return 0;
}
