// encoding: UTF-8
// Regression harness for CE output-panel color selection and log normalization.
// TestCePanel.ps1 extracts the real functions from src/CePanel.c; Win32 window
// calls and the heap allocator are replaced by small observing stubs.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <process.h>
#include <string>

using WCHAR = wchar_t;
using LPCWSTR = const wchar_t*;
using HWND = void*;
using DWORD = unsigned long;
using COLORREF = unsigned int;

#define HEAP_ZERO_MEMORY 0x00000008u
#define RGB(r, g, b) ((COLORREF)(((r) | ((g) << 8)) | ((b) << 16)))

static bool g_dark = false;
static std::wstring g_appended;
static COLORREF g_lastColor = 0;

static bool g_guardOk = true;
static void* g_lastBase = nullptr;
static size_t g_lastCb = 0;

static HWND s_hwndLog = reinterpret_cast<HWND>(1);

bool UseDarkMode()
{
    return g_dark;
}

COLORREF GetModeTextColor(bool dark)
{
    return dark ? RGB(0xDD, 0xDD, 0xDD) : RGB(0x11, 0x11, 0x11);
}

void _AppendColored(HWND, LPCWSTR text, COLORREF color)
{
    g_appended = text ? text : L"";
    g_lastColor = color;
}

void* AllocMem(size_t cb, DWORD)
{
    constexpr size_t guardSize = 256u;
    void* const base = std::calloc(1u, sizeof(size_t) + cb + guardSize);
    if (!base) {
        return nullptr;
    }
    *static_cast<size_t*>(base) = cb;
    auto* const bytes = static_cast<unsigned char*>(base);
    std::memset(bytes + sizeof(size_t) + cb, 0xA5, guardSize);
    g_lastBase = base;
    g_lastCb = cb;
    return bytes + sizeof(size_t);
}

void FreeMem(void* p)
{
    if (!p) {
        return;
    }
    auto* const bytes = static_cast<unsigned char*>(p) - sizeof(size_t);
    size_t const cb = *reinterpret_cast<size_t*>(bytes);
    for (size_t i = 0; i < 256u; ++i) {
        if (bytes[sizeof(size_t) + cb + i] != 0xA5) {
            g_guardOk = false;
            break;
        }
    }
    std::free(bytes);
    g_lastBase = nullptr;
}

#include "CePanelHelpers.inc"

static int g_failures = 0;
#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                 \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

int main(int argc, char** argv)
{
    if ((argc == 2) && (std::strcmp(argv[1], "--level-child") == 0)) {
        g_dark = false;
        (void)_LevelColor(1);
        return 0;
    }

    {
        int const status = static_cast<int>(_spawnl(_P_WAIT, argv[0], argv[0], "--level-child", nullptr));
        CHECK(status == 0); // recursion in the light-theme error branch crashes
    }

    g_dark = true;
    g_appended.clear();
    g_guardOk = true;
    CePanel_LogText(0, L"a\nb\nc\nd\nx");
    CHECK(g_guardOk);
    CHECK(g_appended == L"a\r\nb\r\nc\r\nd\r\nx\r\n");

    g_appended.clear();
    g_guardOk = true;
    CePanel_LogText(0, L"a\r\nb\r\n");
    CHECK(g_guardOk);
    CHECK(g_appended == L"a\r\nb\r\n");

    g_appended.clear();
    g_guardOk = true;
    CePanel_LogText(0, L"");
    CHECK(g_guardOk);
    CHECK(g_appended == L"\r\n");

    if (g_failures != 0) {
        std::printf("%d CE panel test(s) failed\n", g_failures);
        return 1;
    }
    std::printf("CE panel tests passed\n");
    return 0;
}
