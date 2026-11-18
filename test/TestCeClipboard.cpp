// encoding: UTF-8
// Regression harness for CE template clipboard-variable resolution.
// TestCeClipboard.ps1 extracts the real _GetClipboardVariable() from
// src/CeCompletion.c; Win32 clipboard/conversion services are narrow stubs.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
#define CP_UTF8 65001u
#define CF_UNICODETEXT 13u
#define S_OK 0L
#define S_FALSE 1L
#define FAILED(hr) ((hr) < 0L)

using WCHAR = wchar_t;
using CHAR = char;
using HWND = void*;
using HANDLE = void*;
using HRESULT = long;
using UINT = unsigned int;
using LPCSTR = const char*;
using LPSTR = char*;

struct GLOBALS { HWND hwndMain; } Globals;

static bool g_clipboardOpen = false;
static std::vector<WCHAR> g_clipboard;
static int g_validationCalls = 0;
static bool g_validationResult = true;

bool OpenClipboard(HWND)
{
    g_clipboardOpen = true;
    return true;
}

bool CloseClipboard()
{
    g_clipboardOpen = false;
    return true;
}

HANDLE GetClipboardData(UINT)
{
    return g_clipboard.empty() ? nullptr : reinterpret_cast<HANDLE>(g_clipboard.data());
}

WCHAR* GlobalLock(HANDLE h)
{
    return static_cast<WCHAR*>(h);
}

bool GlobalUnlock(HANDLE)
{
    return true;
}

HRESULT StringCchCopyNW(WCHAR* dst, size_t cch, const WCHAR* src, size_t cchSrc)
{
    if (!dst || !src || cch == 0) {
        return -1L;
    }
    size_t n = 0;
    while ((n < cch - 1) && (n < cchSrc) && (src[n] != L'\0')) {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = L'\0';
    return (src[n] != L'\0' || n == cchSrc) ? S_FALSE : S_OK;
}

HRESULT StringCchCopyA(CHAR* dst, size_t cch, LPCSTR src)
{
    if (!dst || !src || cch == 0) {
        return -1L;
    }
    size_t n = 0;
    while ((n < cch - 1) && (src[n] != '\0')) {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = '\0';
    return S_OK;
}

int WideCharToMultiByte(UINT, UINT, const WCHAR* src, int, LPSTR dst, int cbDst,
                        LPCSTR, bool*)
{
    size_t needed = 1;
    for (const WCHAR* p = src; *p; ++p) {
        needed += (*p < 0x80) ? 1u : 3u;
    }
    if ((cbDst <= 0) || (needed > static_cast<size_t>(cbDst))) {
        if (dst && (cbDst > 0)) {
            std::memset(dst, 0x7F, static_cast<size_t>(cbDst));
        }
        return 0;
    }
    size_t n = 0;
    for (const WCHAR* p = src; *p; ++p) {
        if (*p < 0x80) {
            dst[n++] = static_cast<CHAR>(*p);
        }
        else {
            dst[n++] = static_cast<CHAR>(0xE4);
            dst[n++] = static_cast<CHAR>(0xB8);
            dst[n++] = static_cast<CHAR>(0xAD);
        }
    }
    dst[n] = '\0';
    return static_cast<int>(n + 1);
}

static bool _IsDefinedVariable(char* text)
{
    ++g_validationCalls;
    (void)text;
    return g_validationResult;
}

#include "CeClipboardHelpers.inc"

static int g_failures = 0;
#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                 \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static void SetClipboard(const std::wstring& text)
{
    g_clipboard.assign(text.begin(), text.end());
    g_clipboard.push_back(L'\0');
    g_validationCalls = 0;
    g_validationResult = true;
}

int main()
{
    Globals.hwndMain = reinterpret_cast<HWND>(1);

    SetClipboard(L"hp");
    char out[240] = { 0 };
    CHECK(_GetClipboardVariable(out, COUNTOF(out)));
    CHECK(g_validationCalls == 1);
    CHECK(std::strcmp(out, "hp") == 0);

    SetClipboard(std::wstring(200, L'\u4E2D'));
    out[0] = '\0';
    CHECK(!_GetClipboardVariable(out, COUNTOF(out)));
    CHECK(g_validationCalls == 0); // failed UTF-8 conversion must not be validated
    CHECK(out[0] == '\0');

    SetClipboard(std::wstring(300, L'a'));
    out[0] = '\0';
    CHECK(!_GetClipboardVariable(out, COUNTOF(out)));
    CHECK(g_validationCalls == 0); // truncated clipboard text must not be used

    if (g_failures != 0) {
        std::printf("%d CE clipboard test(s) failed\n", g_failures);
        return 1;
    }
    std::printf("CE clipboard tests passed\n");
    return 0;
}
