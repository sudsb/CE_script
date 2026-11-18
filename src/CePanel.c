// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CePanel.c                                                                   *
*   CE output / log panel (terminal + coloured log).                          *
*                                                                             *
*   The same panel is either docked at the bottom of the main frame or popped *
*   out as an independent top-level window.  It holds a read-only RichEdit log *
*   whose colours follow the current Notepad3 theme, a small command entry    *
*   (a minimal terminal), and header buttons for font size, clearing the log  *
*   and docking/floating.  Font size is persisted in Settings2.CeOutputSize,  *
*   the docked height in Settings2.CePanelHeight.                             *
*                                                                             *
******************************************************************************/

#include "Helpers.h"

#define _RICHEDIT_VER 0x0500
#include <richedit.h>
#include <strsafe.h>

#include "Config/Config.h"
#include "DarkMode/DarkMode.h"
#include "CePanel.h"

// ----------------------------------------------------------------------------

#define NP3_CE_PANEL_CLASS L"NP3_CE_PANEL"
#define WM_CEP_RUN (WM_USER + 1)
#define CEP_HEADER_H   30
#define CEP_BTN_W      24
#define CEP_BTN_CLEAR_W 48
#define CEP_BTN_FLOAT_W 48
#define CEP_BTN_GAP    4
#define CEP_MARGIN     6

enum CE_PANEL_ID {
    IDC_CEP_LBL_TITLE = 5000,
    IDC_CEP_BTN_MINUS,
    IDC_CEP_BTN_PLUS,
    IDC_CEP_BTN_CLEAR,
    IDC_CEP_BTN_FLOAT,
    IDC_CEP_BTN_CLOSE,
    IDC_CEP_LOG,
    IDC_CEP_CMD
};

static HWND s_hwndMain  = NULL;
static HWND s_hwndPanel = NULL;
static HWND s_hwndLog   = NULL;
static HWND s_hwndCmd   = NULL;
static HWND s_hwndFloat = NULL;  // header float/dock button
static HFONT s_hLogFont = NULL;
static HFONT s_hUiFont  = NULL;
static HBRUSH s_hPanelBkBrush = NULL;
static WNDPROC s_cmdEditProc = NULL;
static bool s_visible   = false;
static bool s_userHidden = false;   // user closed it: stay closed until opened again
static bool s_floating  = false;
static bool s_classRegistered = false;
static HINSTANCE s_hInst = NULL;
static RECT s_floatRect = { 0, 0, 0, 0 };  // last independent-window placement

// Dragging the header resizes the panel; the height is persisted so the user
// keeps the window size they chose.
static bool s_dragging = false;
static int  s_dragStartCursorY = 0;
static int  s_dragStartHeight  = 0;

// ----------------------------------------------------------------------------

static bool _UseDark(void) {
    return UseDarkMode();
}

static COLORREF _NormalColor(void) {
    return GetModeTextColor(_UseDark());
}

static COLORREF _LevelColor(int level) {
    bool const dark = _UseDark();
    switch (level) {
    case 1: return dark ? RGB(0xFF, 0x7A, 0x7A) : RGB(0xC0, 0x00, 0x00); // error
    case 2: return dark ? RGB(0xF0, 0xC0, 0x6A) : RGB(0xB8, 0x6A, 0x00); // warning
    case 4: return dark ? RGB(0x9E, 0x9E, 0x9E) : RGB(0x80, 0x80, 0x80); // debug
    default: return _NormalColor();                                      // info/plain
    }
}

static void _AppendColored(HWND rich, LPCWSTR text, COLORREF color) {
    if (!rich || !text) {
        return;
    }
    int const len = GetWindowTextLengthW(rich);
    SendMessageW(rich, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(rich, EM_SCROLLCARET, 0, 0);
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = color;
    SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(rich, EM_REPLACESEL, FALSE, (LPARAM)text);
    cf.crTextColor = _NormalColor();
    SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

static bool _ContainsI(LPCWSTR haystack, LPCWSTR needle) {
    if (!haystack || !needle || !*needle) {
        return false;
    }
    int const nLen = (int)wcslen(needle);
    int const hLen = (int)wcslen(haystack);
    if (nLen > hLen) {
        return false;
    }
    for (int i = 0; i + nLen <= hLen; ++i) {
        bool match = true;
        for (int j = 0; j < nLen; ++j) {
            WCHAR a = haystack[i + j];
            WCHAR b = needle[j];
            if (a >= L'A' && a <= L'Z') {
                a = (WCHAR)(a - L'A' + L'a');
            }
            if (b >= L'A' && b <= L'Z') {
                b = (WCHAR)(b - L'A' + L'a');
            }
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static bool _IsErrorLine(LPCWSTR line) {
    static const WCHAR* const ascii[] = {
        L"error", L"fail", L"exception", L"warning", NULL
    };
    for (int i = 0; ascii[i]; ++i) {
        if (_ContainsI(line, ascii[i])) {
            return true;
        }
    }
    if (wcsstr(line, L"失败") || wcsstr(line, L"错误") || wcsstr(line, L"异常") ||
        wcsstr(line, L"报错") || wcsstr(line, L"警告")) {
        return true;
    }
    return false;
}

void CePanel_LogText(int level, LPCWSTR text) {
    if (!s_hwndLog || !text) {
        return;
    }
    size_t const len = wcslen(text);
    // Normalise to CRLF, keep the message in one coloured block (a Lua error
    // with a traceback is one logical message).  Each bare LF grows by one
    // WCHAR, so size the buffer from the normalised length, not the input.
    size_t cap = 1u; // terminating NUL
    bool trailingLf = false;
    for (size_t i = 0; i < len; ++i) {
        WCHAR const ch = text[i];
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            cap += 2u;
            trailingLf = true;
        }
        else {
            ++cap;
            trailingLf = false;
        }
    }
    if (!trailingLf) {
        cap += 2u;
    }
    if (cap > (((size_t)-1) / sizeof(WCHAR))) {
        return;
    }
    WCHAR* const buf = (WCHAR*)AllocMem(cap * sizeof(WCHAR), HEAP_ZERO_MEMORY);
    if (!buf) {
        return;
    }
    size_t k = 0;
    for (size_t i = 0; i < len; ++i) {
        WCHAR const ch = text[i];
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            buf[k++] = L'\r';
        }
        buf[k++] = ch;
    }
    if (!trailingLf) {
        buf[k++] = L'\r';
        buf[k++] = L'\n';
    }
    buf[k] = L'\0';
    _AppendColored(s_hwndLog, buf, _LevelColor(level));
    FreeMem(buf);
}

void CePanel_Log(int level, LPCWSTR msg) {
    CePanel_LogText(level, msg);
}

void CePanel_AppendConsole(LPCWSTR text) {
    if (!s_hwndLog || !text) {
        return;
    }
    const WCHAR* p = text;
    const WCHAR* lineStart = p;
    while (*p) {
        if (*p == L'\n' || *p == L'\r') {
            int const lineLen = (int)(p - lineStart);
            if (lineLen > 0) {
                WCHAR line[2048];
                if (SUCCEEDED(StringCchCopyNW(line, COUNTOF(line), lineStart, lineLen))) {
                    bool const err = _IsErrorLine(line);
                    _AppendColored(s_hwndLog, line, err ? _LevelColor(1) : _NormalColor());
                }
            }
            _AppendColored(s_hwndLog, L"\r\n", _NormalColor());
            if (*p == L'\r' && *(p + 1) == L'\n') {
                p += 2;
            } else {
                ++p;
            }
            lineStart = p;
        } else {
            ++p;
        }
    }
    if (p > lineStart) {
        WCHAR line[2048];
        if (SUCCEEDED(StringCchCopyNW(line, COUNTOF(line), lineStart, (int)(p - lineStart)))) {
            bool const err = _IsErrorLine(line);
            _AppendColored(s_hwndLog, line, err ? _LevelColor(1) : _NormalColor());
        }
        _AppendColored(s_hwndLog, L"\r\n", _NormalColor());
    }
}

void CePanel_Clear(void) {
    if (s_hwndLog) {
        SetWindowTextW(s_hwndLog, L"");
    }
}

// ----------------------------------------------------------------------------

static void _ApplyLogFont(void) {
    if (s_hLogFont) {
        DeleteObject(s_hLogFont);
        s_hLogFont = NULL;
    }
    int const size = clampi(Settings2.CeOutputFontSize, 8, 30);
    HDC const hdc = GetDC(NULL);
    int const dpiY = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) {
        ReleaseDC(NULL, hdc);
    }
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(size, dpiY, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, COUNTOF(lf.lfFaceName), L"Consolas");
    s_hLogFont = CreateFontIndirectW(&lf);
    if (s_hLogFont) {
        if (s_hwndLog) {
            SendMessageW(s_hwndLog, WM_SETFONT, (WPARAM)s_hLogFont, TRUE);
        }
        if (s_hwndCmd) {
            SendMessageW(s_hwndCmd, WM_SETFONT, (WPARAM)s_hLogFont, TRUE);
        }
    }
}

static void _ApplyUiFont(void) {
    if (s_hUiFont) {
        DeleteObject(s_hUiFont);
        s_hUiFont = NULL;
    }
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0)) {
        lf = ncm.lfMessageFont;
    } else {
        HFONT const hStock = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        if (hStock) {
            GetObjectW(hStock, sizeof(lf), &lf);
        }
    }
    s_hUiFont = CreateFontIndirectW(&lf);
    if (!s_hUiFont) {
        return;
    }
    static const UINT ids[] = {
        IDC_CEP_LBL_TITLE, IDC_CEP_BTN_MINUS, IDC_CEP_BTN_PLUS,
        IDC_CEP_BTN_CLEAR, IDC_CEP_BTN_FLOAT, IDC_CEP_BTN_CLOSE
    };
    for (size_t i = 0; i < COUNTOF(ids); ++i) {
        HWND const h = GetDlgItem(s_hwndPanel, ids[i]);
        if (h) {
            SendMessageW(h, WM_SETFONT, (WPARAM)s_hUiFont, TRUE);
        }
    }
}

void CePanel_RefreshTheme(void) {
    if (s_hPanelBkBrush) {
        DeleteObject(s_hPanelBkBrush);
        s_hPanelBkBrush = NULL;
    }
    bool const dark = _UseDark();
    s_hPanelBkBrush = CreateSolidBrush(GetModeBtnfaceColor(dark));
    static const UINT ids[] = {
        IDC_CEP_BTN_MINUS, IDC_CEP_BTN_PLUS, IDC_CEP_BTN_CLEAR,
        IDC_CEP_BTN_FLOAT, IDC_CEP_BTN_CLOSE
    };
    for (size_t i = 0; i < COUNTOF(ids); ++i) {
        HWND const h = GetDlgItem(s_hwndPanel, ids[i]);
        if (!h) {
            continue;
        }
#ifdef D_NP3_WIN10_DARK_MODE
        if (IsDarkModeSupported()) {
            AllowDarkModeForWindowEx(h, dark);
        }
#endif
        InvalidateRect(h, NULL, TRUE);
    }
    if (s_hwndPanel) {
#ifdef D_NP3_WIN10_DARK_MODE
        if (IsDarkModeSupported() && s_floating) {
            AllowDarkModeForWindowEx(s_hwndPanel, dark);
            RefreshTitleBarThemeColor(s_hwndPanel);
        }
#endif
        InvalidateRect(s_hwndPanel, NULL, TRUE);
    }
    if (s_hwndLog) {
        SendMessageW(s_hwndLog, EM_SETBKGNDCOLOR, 0, (LPARAM)GetModeBkColor(dark));
        InvalidateRect(s_hwndLog, NULL, TRUE);
    }
    if (s_hwndCmd) {
        InvalidateRect(s_hwndCmd, NULL, TRUE);
    }
}

static void _LayoutChildren(int cx, int cy) {
    int const headerH = CEP_HEADER_H;
    int const cmdH = 26;
    int const btnH = headerH - 8;
    int const btnY = 4;
    int x = cx - CEP_MARGIN - CEP_BTN_W;
    SetWindowPos(GetDlgItem(s_hwndPanel, IDC_CEP_BTN_CLOSE), NULL, x, btnY, CEP_BTN_W, btnH, SWP_NOZORDER);
    x -= CEP_BTN_FLOAT_W + CEP_BTN_GAP;
    SetWindowPos(GetDlgItem(s_hwndPanel, IDC_CEP_BTN_FLOAT), NULL, x, btnY, CEP_BTN_FLOAT_W, btnH, SWP_NOZORDER);
    x -= CEP_BTN_CLEAR_W + CEP_BTN_GAP;
    SetWindowPos(GetDlgItem(s_hwndPanel, IDC_CEP_BTN_CLEAR), NULL, x, btnY, CEP_BTN_CLEAR_W, btnH, SWP_NOZORDER);
    x -= CEP_BTN_W + CEP_BTN_GAP;
    SetWindowPos(GetDlgItem(s_hwndPanel, IDC_CEP_BTN_PLUS), NULL, x, btnY, CEP_BTN_W, btnH, SWP_NOZORDER);
    x -= CEP_BTN_W + CEP_BTN_GAP;
    SetWindowPos(GetDlgItem(s_hwndPanel, IDC_CEP_BTN_MINUS), NULL, x, btnY, CEP_BTN_W, btnH, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(s_hwndPanel, IDC_CEP_LBL_TITLE), NULL, CEP_MARGIN, btnY,
                 max_i(0, x - CEP_MARGIN - CEP_BTN_GAP), btnH, SWP_NOZORDER);
    int const logH = max_i(0, cy - headerH - cmdH - 6);
    if (s_hwndLog) {
        SetWindowPos(s_hwndLog, NULL, CEP_MARGIN, headerH, max_i(0, cx - 2 * CEP_MARGIN), logH, SWP_NOZORDER);
    }
    if (s_hwndCmd) {
        SetWindowPos(s_hwndCmd, NULL, CEP_MARGIN, cy - cmdH - 4, max_i(0, cx - 2 * CEP_MARGIN), cmdH, SWP_NOZORDER);
    }
}

// ----------------------------------------------------------------------------
// Size limits / relayout / persistence

static int _Dpi(void) {
    HDC const hdc = GetDC(NULL);
    int const dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : USER_DEFAULT_SCREEN_DPI;
    if (hdc) {
        ReleaseDC(NULL, hdc);
    }
    return max_i(dpi, USER_DEFAULT_SCREEN_DPI);
}

static int _Scale(int value) {
    return MulDiv(value, _Dpi(), 96);
}

// The panel may never eat the whole editor: it is clamped to a share of the
// main window, and to a DPI-scaled minimum so it stays readable.
static void _HeightLimits(int* minH, int* maxH) {
    RECT rc = { 0, 0, 0, 0 };
    int cy = 0;
    if (IsWindow(s_hwndMain) && GetClientRect(s_hwndMain, &rc)) {
        cy = rc.bottom;
    }
    if (cy <= 0) {
        cy = _Scale(600);
    }
    *minH = _Scale(96);
    *maxH = max_i(*minH + _Scale(40), (cy * 3) / 5);
}

static void _RelayoutMain(void) {
    if (!IsWindow(s_hwndMain)) {
        return;
    }
    RECT rc;
    GetClientRect(s_hwndMain, &rc);
    SendMessageW(s_hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
}

static void _PersistPanelHeight(void) {
    if (OpenSettingsFile("CePanelHeight")) {
        IniSectionSetInt(Constants.Settings2_Section, L"CePanelHeight", Settings2.CePanelHeight);
        CloseSettingsFile("CePanelHeight", true);
    }
}

// The header holds a static title; forward its mouse input so the whole header
// band can be used to drag-resize the panel.
static WNDPROC s_headerProc = NULL;

static LRESULT CALLBACK _HeaderProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) {
    switch (umsg) {
    case WM_SETCURSOR:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
        return SendMessageW(GetParent(hwnd), umsg, wParam, lParam);
    default:
        break;
    }
    return CallWindowProcW(s_headerProc, hwnd, umsg, wParam, lParam);
}

static void _RunCommand(HWND hwnd) {
    UNREFERENCED_PARAMETER(hwnd);
    if (!s_hwndCmd) {
        return;
    }
    WCHAR cmd[1024];
    GetWindowTextW(s_hwndCmd, cmd, COUNTOF(cmd));
    if (!cmd[0]) {
        return;
    }
    WCHAR line[1100];
    StringCchPrintfW(line, COUNTOF(line), L"> %s", cmd);
    CePanel_Log(0, line);

    WCHAR cmdline[1200];
    StringCchPrintfW(cmdline, COUNTOF(cmdline), L"cmd.exe /c %s", cmd);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE hOutR = NULL;
    HANDLE hOutW = NULL;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hOutR, &hOutW, &sa, 0)) {
        CePanel_Log(1, L"无法创建管道");
        return;
    }
    si.hStdOutput = hOutW;
    si.hStdError = hOutW;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(hOutW);
        hOutW = NULL;
        DWORD const wait = WaitForSingleObject(pi.hProcess, 15000);
        if (wait == WAIT_TIMEOUT) {
            CePanel_Log(2, L"命令执行超时");
            TerminateProcess(pi.hProcess, 1);
        } else {
            char buf[4096];
            DWORD read = 0;
            while (ReadFile(hOutR, buf, sizeof(buf) - 1, &read, NULL) && read > 0) {
                buf[read] = '\0';
                int const need = MultiByteToWideChar(CP_OEMCP, 0, buf, -1, NULL, 0);
                if (need > 0) {
                    WCHAR* w = AllocMem((size_t)need * sizeof(WCHAR), HEAP_ZERO_MEMORY);
                    if (w) {
                        MultiByteToWideChar(CP_OEMCP, 0, buf, -1, w, need);
                        CePanel_AppendConsole(w);
                        FreeMem(w);
                    }
                }
            }
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CePanel_Log(1, L"命令执行失败");
    }
    if (hOutW) {
        CloseHandle(hOutW);
    }
    if (hOutR) {
        CloseHandle(hOutR);
    }
    SetWindowTextW(s_hwndCmd, L"");
    SetFocus(s_hwndCmd);
}

static LRESULT CALLBACK _CmdEditProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) {
    if (umsg == WM_KEYDOWN && wParam == VK_RETURN) {
        SendMessageW(GetParent(hwnd), WM_CEP_RUN, 0, 0);
        return 0;
    }
    return CallWindowProcW(s_cmdEditProc, hwnd, umsg, wParam, lParam);
}

static LRESULT CALLBACK _CePanelWndProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) {
    switch (umsg) {
    case WM_CREATE: {
        HWND const hTitle = CreateWindowExW(0, L"STATIC", L"日志 / CE 输出（拖动标题栏调整高度）",
            WS_CHILD | WS_VISIBLE | SS_LEFT, CEP_MARGIN, 4, 100, 20, hwnd,
            (HMENU)(INT_PTR)IDC_CEP_LBL_TITLE, s_hInst, NULL);
        DWORD const btnStyle = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT;
        HWND const hMinus = CreateWindowExW(0, L"BUTTON", L"A-",
            btnStyle, 0, 4, CEP_BTN_W, 22, hwnd,
            (HMENU)(INT_PTR)IDC_CEP_BTN_MINUS, s_hInst, NULL);
        HWND const hPlus = CreateWindowExW(0, L"BUTTON", L"A+",
            btnStyle, 0, 4, CEP_BTN_W, 22, hwnd,
            (HMENU)(INT_PTR)IDC_CEP_BTN_PLUS, s_hInst, NULL);
        HWND const hClear = CreateWindowExW(0, L"BUTTON", L"清空",
            btnStyle, 0, 4, CEP_BTN_CLEAR_W, 22, hwnd,
            (HMENU)(INT_PTR)IDC_CEP_BTN_CLEAR, s_hInst, NULL);
        HWND const hFloat = CreateWindowExW(0, L"BUTTON", L"浮出",
            btnStyle, 0, 4, CEP_BTN_FLOAT_W, 22, hwnd,
            (HMENU)(INT_PTR)IDC_CEP_BTN_FLOAT, s_hInst, NULL);
        HWND const hClose = CreateWindowExW(0, L"BUTTON", L"×",
            btnStyle, 0, 4, CEP_BTN_W, 22, hwnd,
            (HMENU)(INT_PTR)IDC_CEP_BTN_CLOSE, s_hInst, NULL);
        s_hwndFloat = hFloat;
        if (hTitle) {
            s_headerProc = (WNDPROC)SetWindowLongPtrW(hTitle, GWLP_WNDPROC,
                                                     (LONG_PTR)_HeaderProc);
        }
        UNREFERENCED_PARAMETER(hMinus);
        UNREFERENCED_PARAMETER(hPlus);
        UNREFERENCED_PARAMETER(hClear);
        UNREFERENCED_PARAMETER(hClose);

        s_hwndLog = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            CEP_MARGIN, CEP_HEADER_H, 100, 100, hwnd,
            (HMENU)(INT_PTR)IDC_CEP_LOG, s_hInst, NULL);
        s_hwndCmd = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            CEP_MARGIN, 0, 100, 26, hwnd, (HMENU)(INT_PTR)IDC_CEP_CMD, s_hInst, NULL);
        if (s_hwndCmd) {
            s_cmdEditProc = (WNDPROC)SetWindowLongPtrW(s_hwndCmd, GWLP_WNDPROC,
                                                       (LONG_PTR)_CmdEditProc);
        }
        _ApplyUiFont();
        _ApplyLogFont();
        CePanel_RefreshTheme();
        return 0;
    }
    case WM_SIZE:
        _LayoutChildren(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_SETCURSOR: {
        if (s_floating) {
            break;  // popped out: the system title bar handles the hit test
        }
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (pt.y < CEP_HEADER_H) {
            SetCursor(LoadCursorW(NULL, IDC_SIZENS));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (pt.y < CEP_HEADER_H) {
            if (s_floating) {
                POINT ptScreen;
                GetCursorPos(&ptScreen);
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                             MAKELPARAM(ptScreen.x, ptScreen.y));
                return 0;
            }
            s_dragging = true;
            s_dragStartCursorY = pt.y;
            s_dragStartHeight = CePanel_Height();
            SetCapture(hwnd);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (s_dragging) {
            POINT pt;
            int minH = 0;
            int maxH = 0;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            _HeightLimits(&minH, &maxH);
            int const height = s_dragStartHeight + (s_dragStartCursorY - pt.y);
            Settings2.CePanelHeight = clampi(height, minH, maxH);
            _RelayoutMain();
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (s_dragging) {
            s_dragging = false;
            ReleaseCapture();
            _PersistPanelHeight();
            return 0;
        }
        break;
    case WM_CEP_RUN:
        _RunCommand(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CEP_BTN_MINUS:
            Settings2.CeOutputFontSize = clampi(Settings2.CeOutputFontSize - 1, 8, 30);
            _ApplyLogFont();
            return 0;
        case IDC_CEP_BTN_PLUS:
            Settings2.CeOutputFontSize = clampi(Settings2.CeOutputFontSize + 1, 8, 30);
            _ApplyLogFont();
            return 0;
        case IDC_CEP_BTN_CLEAR:
            CePanel_Clear();
            return 0;
        case IDC_CEP_BTN_FLOAT:
            CePanel_ToggleFloat();
            return 0;
        case IDC_CEP_BTN_CLOSE:
            CePanel_Toggle();
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        if (s_hPanelBkBrush) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wParam, &rc, s_hPanelBkBrush);
            return TRUE;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC const hdc = (HDC)wParam;
        bool const dark = _UseDark();
        if ((HWND)lParam == GetDlgItem(hwnd, IDC_CEP_LBL_TITLE)) {
            SetTextColor(hdc, GetModeTextColor(dark));
            SetBkColor(hdc, GetModeBtnfaceColor(dark));
            return (LRESULT)(s_hPanelBkBrush ? s_hPanelBkBrush
                                             : GetSysColorBrush(COLOR_BTNFACE));
        }
        SetTextColor(hdc, _NormalColor());
        SetBkColor(hdc, GetModeBkColor(dark));
#ifdef D_NP3_WIN10_DARK_MODE
        if (dark && Globals.hbrDarkModeBkgBrush) {
            return (LRESULT)Globals.hbrDarkModeBkgBrush;
        }
#endif
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_THEMECHANGED:
        CePanel_RefreshTheme();
        return 0;
    case WM_CLOSE:
        // A popped-out window is closed by hiding it, not by destroying it:
        // the log and the command entry must survive for the next script run.
        CePanel_Toggle();
        return 0;
    case WM_DESTROY:
        if (s_dragging) {
            s_dragging = false;
            ReleaseCapture();
        }
        if (s_hUiFont) {
            DeleteObject(s_hUiFont);
            s_hUiFont = NULL;
        }
        if (s_hPanelBkBrush) {
            DeleteObject(s_hPanelBkBrush);
            s_hPanelBkBrush = NULL;
        }
        s_hwndFloat = NULL;
        s_hwndLog = NULL;
        s_hwndCmd = NULL;
        s_cmdEditProc = NULL;
        s_headerProc = NULL;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, umsg, wParam, lParam);
}

static void _RegisterClass(void) {
    if (s_classRegistered) {
        return;
    }
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = _CePanelWndProc;
    wc.hInstance = s_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = NP3_CE_PANEL_CLASS;
    RegisterClassExW(&wc);
    s_classRegistered = true;
}

void CePanel_Create(HWND hwndMain) {
    s_hwndMain = hwndMain;
    if (s_hwndPanel) {
        return;
    }
    s_hInst = GetModuleHandleW(NULL);
    LoadLibraryW(L"msftedit.dll"); // RICHEDIT50W
    _RegisterClass();
    s_hwndPanel = CreateWindowExW(0, NP3_CE_PANEL_CLASS, L"",
        WS_CHILD | WS_CLIPCHILDREN, 0, 0, 100, 100, hwndMain,
        NULL, s_hInst, NULL);
    s_visible = false;
    if (s_hwndPanel) {
        ShowWindow(s_hwndPanel, SW_HIDE);
    }
}

void CePanel_Layout(HWND hwndMain, int x, int y, int cx, int cy) {
    UNREFERENCED_PARAMETER(hwndMain);
    if (!s_hwndPanel || s_floating) {
        return;
    }
    SetWindowPos(s_hwndPanel, NULL, x, y, cx, cy,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

int CePanel_Height(void) {
    int minH = 0;
    int maxH = 0;
    _HeightLimits(&minH, &maxH);
    int height = Settings2.CePanelHeight;
    if (height <= 0) {
        height = _Scale(180);   // compact default instead of a fixed 200 px
    }
    return clampi(height, minH, maxH);
}

bool CePanel_IsVisible(void) {
    return s_visible;
}

bool CePanel_IsFloating(void) {
    return s_floating;
}

// Move the very same panel (and therefore the same log/command controls) between
// the docked bottom band and an independent top-level window.  No log state is
// touched, so output written while floating is visible again after docking.
static void _SetFloating(bool floating) {
    if (!s_hwndPanel || s_floating == floating) {
        return;
    }
    HWND const hTitle = GetDlgItem(s_hwndPanel, IDC_CEP_LBL_TITLE);
    if (floating) {
        int w = s_floatRect.right - s_floatRect.left;
        int h = s_floatRect.bottom - s_floatRect.top;
        if (w <= 0 || h <= 0) {
            w = _Scale(560);
            h = _Scale(380);
        }
        int x = s_floatRect.left;
        int y = s_floatRect.top;
        if (x == 0 && y == 0) {
            RECT rcMain = { 0, 0, 0, 0 };
            if (IsWindow(s_hwndMain)) {
                GetWindowRect(s_hwndMain, &rcMain);
            }
            x = rcMain.left + max_i(0, ((rcMain.right - rcMain.left) - w) / 2);
            y = rcMain.top + max_i(0, ((rcMain.bottom - rcMain.top) - h) / 2);
        }
        SetParent(s_hwndPanel, NULL);
        LONG_PTR const style = GetWindowLongPtrW(s_hwndPanel, GWL_STYLE);
        SetWindowLongPtrW(s_hwndPanel, GWL_STYLE,
                          (style & ~(LONG_PTR)WS_CHILD) | WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN);
        SetWindowLongPtrW(s_hwndPanel, GWLP_HWNDPARENT, (LONG_PTR)s_hwndMain);
        SetWindowTextW(s_hwndPanel, L"CE 日志输出 - 独立窗口");
        if (hTitle) {
            SetWindowTextW(hTitle, L"日志 / CE 输出  ·  拖动标题栏可移动窗口");
        }
        if (s_hwndFloat) {
            SetWindowTextW(s_hwndFloat, L"停靠");
        }
        s_floating = true;
        SetWindowPos(s_hwndPanel, HWND_TOP, x, y, w, h,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE |
                     (s_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        // Re-layout the main frame: the docked band must be given back to the
        // editor, otherwise an empty gap stays where the panel used to be.
        _RelayoutMain();
    } else {
        GetWindowRect(s_hwndPanel, &s_floatRect);
        SetWindowLongPtrW(s_hwndPanel, GWLP_HWNDPARENT, 0);
        SetParent(s_hwndPanel, s_hwndMain);
        LONG_PTR const style = GetWindowLongPtrW(s_hwndPanel, GWL_STYLE);
        SetWindowLongPtrW(s_hwndPanel, GWL_STYLE,
                          ((style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_CHILD | WS_CLIPCHILDREN)
                          & ~(LONG_PTR)WS_VISIBLE);
        SetWindowTextW(s_hwndPanel, L"");
        if (hTitle) {
            SetWindowTextW(hTitle, L"日志 / CE 输出  ·  拖动标题栏可调整高度");
        }
        if (s_hwndFloat) {
            SetWindowTextW(s_hwndFloat, L"浮出");
        }
        s_floating = false;
        if (s_visible) {
            ShowWindow(s_hwndPanel, SW_SHOW);
        }
        _RelayoutMain();
    }
    CePanel_RefreshTheme();
}

void CePanel_Show(HWND hwndMain) {
    if (!s_hwndPanel) {
        CePanel_Create(IsWindow(hwndMain) ? hwndMain : s_hwndMain);
    }
    if (!s_hwndPanel) {
        return;
    }
    if (!s_hLogFont) {
        _ApplyLogFont();
    }
    s_userHidden = false;
    if (!s_visible) {
        s_visible = true;
        ShowWindow(s_hwndPanel, SW_SHOW);
        _RelayoutMain();
    }
}

// Show the panel because something was printed.  A panel the user closed by
// hand stays closed until it is opened again.
void CePanel_AutoShow(void) {
    if (s_userHidden || s_visible) {
        return;
    }
    CePanel_Show(s_hwndMain);
}

void CePanel_Toggle(void) {
    if (s_visible) {
        s_visible = false;
        s_userHidden = true;
        ShowWindow(s_hwndPanel, SW_HIDE);
        _RelayoutMain();
        return;
    }
    CePanel_Show(s_hwndMain);
}

void CePanel_ToggleFloat(void) {
    if (!s_hwndPanel) {
        CePanel_Create(s_hwndMain);
    }
    if (!s_hwndPanel) {
        return;
    }
    if (!s_visible) {
        CePanel_Show(s_hwndMain);
    }
    _SetFloating(!s_floating);
}
