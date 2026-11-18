// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeSettings.c                                                                *
*   Cheat Engine editor options + code template editor.                       *
*                                                                             *
*   The window and its controls are created at runtime: the CE branch has no  *
*   per-language dialog resource, and every value is stored in the existing   *
*   Settings2 struct / [Settings2] INI section.  The dialog snapshots that    *
*   struct on open: OK persists, Cancel/ESC restores the snapshot, and the    *
*   Reload button re-reads only the CE entries from the INI file.             *
*                                                                             *
*   Both the settings window and the template editor are true modal dialogs   *
*   (DialogBoxIndirectParam) so the OS disables the owner and routes all      *
*   keyboard input (including Delete) to the dialog, never to the editor      *
*   behind it.                                                                *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <wchar.h>

#include "Config/Config.h"
#include "CeCompletion.h"
#include "CeHighlight.h"
#include "CeLint.h"
#include "CeLspClient.h"
#include "CeLspFeatures.h"
#include "CeSettings.h"
#include "CeTemplate.h"

// Set by CeSettings_Show and read by the settings dialog's WM_INITDIALOG so the
// initial focus can land on the template list when requested.
static bool focusTemplatesForShow = false;

// ----------------------------------------------------------------------------

enum CE_SETTINGS_CTRL_IDS {
    IDC_CE_LBL_COMPLETION = 2000,
    IDC_CE_LBL_FUNCS,
    IDC_CE_LBL_INDENT,
    IDC_CE_LBL_DICT,
    IDC_CE_LBL_TEMPLATES,

    IDC_CE_CHK_CLIPBOARD  = 2100,
    IDC_CE_CHK_AUTOARGS,
    IDC_CE_CHK_AUTOPAIR,
    IDC_CE_CHK_FORMAT,
    IDC_CE_CHK_DYNAMIC,
    IDC_CE_CHK_WRAPASM,
    IDC_CE_CHK_HIGHLIGHT,
    IDC_CE_EDIT_FUNCS,
    IDC_CE_EDIT_INDENT,
    IDC_CE_EDIT_DICT,
    IDC_CE_LIST_TEMPLATES,
    IDC_CE_BTN_NEW,
    IDC_CE_BTN_EDIT,
    IDC_CE_BTN_DELETE,
    IDC_CE_BTN_INSERT,
    IDC_CE_BTN_RELOAD,
    IDC_CE_BTN_DEFAULTS,
    IDC_CE_BTN_OK,
    IDC_CE_BTN_CANCEL
};

// ----------------------------------------------------------------------------
// Template editor dialog (modal; controls built in WM_INITDIALOG).

enum TE_CTRL_IDS {
    IDC_TE_LBL_NAME = 2600,
    IDC_TE_EDIT_NAME,
    IDC_TE_LBL_TRIGGER,
    IDC_TE_EDIT_TRIGGER,
    IDC_TE_LBL_SECTION,
    IDC_TE_CMB_SECTION,
    IDC_TE_CHK_AUTOPAIR,
    IDC_TE_LBL_BODY,
    IDC_TE_EDIT_BODY,
    IDC_TE_BTN_OK,
    IDC_TE_BTN_CANCEL
};

typedef struct _TE_DATA {
    int index;   // -1 for a new template
} TE_DATA;

static void _RefreshTemplateList(HWND hwnd);
static int  _S(int value);
static void _WindowSize(DWORD exStyle, DWORD style, int clientW, int clientH, int* w, int* h);
static void _FitDialogClient(HWND hwnd, int clientW, int clientH);
static HWND _Ctrl(HWND hwnd, LPCWSTR pszClass, LPCWSTR pszText, DWORD style,
                 int x, int y, int w, int h, int id);
static BYTE* _BuildEmptyDialogTemplate(DWORD style, LPCWSTR title,
                                      int pixW, int pixH, size_t* pSize);

// Build a classic DLGTEMPLATE with no predefined controls (the dialog proc
// creates every control itself in WM_INITDIALOG).  Coordinates are supplied in
// pixels and converted to dialog units via the system font base units, which is
// exactly what Windows uses when no DS_SETFONT is specified.
static BYTE* _BuildEmptyDialogTemplate(DWORD style, LPCWSTR title,
                                      int pixW, int pixH, size_t* pSize) {
    DWORD const base = GetDialogBaseUnits();
    int const baseX = (int)LOWORD(base);
    int const baseY = (int)HIWORD(base);
    short const cx = (short)MulDiv(pixW, 4, baseX);
    short const cy = (short)MulDiv(pixH, 8, baseY);

    size_t const titleLen = (wcslen(title) + 1) * sizeof(WCHAR);
    size_t total = sizeof(DLGTEMPLATE) + 2 /*menu*/ + 2 /*class*/ + titleLen;
    total = (total + 3) & ~(size_t)3;
    BYTE* const buf = AllocMem(total, HEAP_ZERO_MEMORY);
    if (!buf) {
        *pSize = 0;
        return NULL;
    }
    DLGTEMPLATE* const d = (DLGTEMPLATE*)buf;
    d->style = style;
    d->dwExtendedStyle = 0;
    d->cdit = 0;
    d->x = 0;
    d->y = 0;
    d->cx = cx;
    d->cy = cy;
    BYTE* p = buf + sizeof(DLGTEMPLATE);
    *(WORD*)p = 0; p += 2;   // no menu
    *(WORD*)p = 0; p += 2;   // default (dialog) class
    memcpy(p, title, titleLen);
    *pSize = total;
    return buf;
}

static INT_PTR CALLBACK _TeDlgProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) {
    switch (umsg) {
    case WM_INITDIALOG: {
        TE_DATA* const d = (TE_DATA*)lParam;
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)d);

        HFONT const hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        WCHAR name[CE_TEMPLATE_NAME_CCH]  = { L'\0' };
        WCHAR trigger[64]                 = { L'\0' };
        WCHAR section[32]                 = { L'\0' };
        WCHAR body[CE_TEMPLATE_BODY_CCH]   = { L'\0' };
        bool autoPair = false;
        bool const edit = (d->index >= 0);
        if (edit) {
            CeTemplate_Get(d->index, name, trigger, section, &autoPair, body);
        }

        #define TE_CTRL(cls, text, style, cx, cy, cw, ch, id)                     \
            do {                                                                  \
                HWND const c = _Ctrl(hwnd, cls, text, style, cx, cy, cw, ch, id); \
                if (c) { SendMessageW(c, WM_SETFONT, (WPARAM)hFont, TRUE); }       \
            } while (0)

        TE_CTRL(L"STATIC", L"名称：", SS_LEFT, 12, 14, 70, 20, IDC_TE_LBL_NAME);
        TE_CTRL(L"EDIT", L"", WS_BORDER | WS_TABSTOP, 90, 12, 300, 24, IDC_TE_EDIT_NAME);
        TE_CTRL(L"STATIC", L"触发函数：", SS_LEFT, 12, 46, 70, 20, IDC_TE_LBL_TRIGGER);
        TE_CTRL(L"EDIT", L"", WS_BORDER | WS_TABSTOP, 90, 44, 300, 24, IDC_TE_EDIT_TRIGGER);
        TE_CTRL(L"STATIC", L"分区：", SS_LEFT, 12, 78, 70, 20, IDC_TE_LBL_SECTION);
        // Dropdown (not a fixed list) so a custom [NAME] section can be typed.
        // The combo box and the checkbox must not overlap: 90..260 / 268..398.
        TE_CTRL(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
                90, 76, 170, 100, IDC_TE_CMB_SECTION);
        TE_CTRL(L"BUTTON", L"自动生成配套代码", BS_AUTOCHECKBOX,
                268, 78, 130, 20, IDC_TE_CHK_AUTOPAIR);
        TE_CTRL(L"STATIC", L"内容（支持换行，$ver$ 为变量占位符）：", SS_LEFT,
                12, 110, 380, 20, IDC_TE_LBL_BODY);
        TE_CTRL(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                 WS_BORDER | WS_VSCROLL | WS_TABSTOP,
                12, 132, 378, 150, IDC_TE_EDIT_BODY);
        TE_CTRL(L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP,
                200, 294, 90, 26, IDC_TE_BTN_OK);
        TE_CTRL(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP,
                300, 294, 90, 26, IDC_TE_BTN_CANCEL);

        #undef TE_CTRL

        HWND const hCombo = GetDlgItem(hwnd, IDC_TE_CMB_SECTION);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"CARET");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"ENABLE");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"DISABLE");
        SendMessageW(hCombo, CB_SETCURSEL, (WPARAM)-1, 0);
        // Show the current value (CARET / a custom [NAME] / one of the presets).
        SetWindowTextW(hCombo, section);

        SetDlgItemTextW(hwnd, IDC_TE_EDIT_NAME, name);
        SetDlgItemTextW(hwnd, IDC_TE_EDIT_TRIGGER, trigger);
        SetDlgItemTextW(hwnd, IDC_TE_EDIT_BODY, body);
        SendDlgItemMessageW(hwnd, IDC_TE_CHK_AUTOPAIR, BM_SETCHECK,
                            autoPair ? BST_CHECKED : BST_UNCHECKED, 0);
        _FitDialogClient(hwnd, _S(404), _S(334));
        SetFocus(GetDlgItem(hwnd, edit ? IDC_TE_EDIT_TRIGGER : IDC_TE_EDIT_NAME));
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_TE_BTN_OK: {
            TE_DATA* const d = (TE_DATA*)GetWindowLongPtrW(hwnd, DWLP_USER);
            WCHAR name[CE_TEMPLATE_NAME_CCH];
            WCHAR trigger[64];
            WCHAR section[32];
            WCHAR body[CE_TEMPLATE_BODY_CCH];
            GetDlgItemTextW(hwnd, IDC_TE_EDIT_NAME, name, COUNTOF(name));
            GetDlgItemTextW(hwnd, IDC_TE_EDIT_TRIGGER, trigger, COUNTOF(trigger));
            // The combobox is a dropdown, so GetDlgItemText returns the typed
            // value too, allowing custom [NAME] sections.
            GetDlgItemTextW(hwnd, IDC_TE_CMB_SECTION, section, COUNTOF(section));
            GetDlgItemTextW(hwnd, IDC_TE_EDIT_BODY, body, COUNTOF(body));
            bool const autoPair = (SendDlgItemMessageW(hwnd, IDC_TE_CHK_AUTOPAIR,
                                                       BM_GETCHECK, 0, 0) == BST_CHECKED);
            int const idx = (d->index >= 0) ? d->index : CeTemplate_Count();
            CeTemplate_Set(idx, name, trigger, section, autoPair, body);
            HWND const owner = GetWindow(hwnd, GW_OWNER);
            if (owner && IsWindow(owner)) {
                _RefreshTemplateList(owner);
            }
            EndDialog(hwnd, 1);
            return TRUE;
        }
        case IDC_TE_BTN_CANCEL:
            EndDialog(hwnd, 0);
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            EndDialog(hwnd, 0);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hwnd, 0);
        return TRUE;

    case WM_DESTROY: {
        TE_DATA* const d = (TE_DATA*)GetWindowLongPtrW(hwnd, DWLP_USER);
        if (d) {
            FreeMem(d);
        }
        return FALSE;
    }
    default:
        break;
    }
    return FALSE;
}

static void _EditTemplate(HWND hwndOwner, int index) {
    TE_DATA* const d = AllocMem(sizeof(TE_DATA), HEAP_ZERO_MEMORY);
    if (!d) {
        return;
    }
    d->index = index;

    int w = 0;
    int h = 0;
    _WindowSize(WS_EX_DLGMODALFRAME, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                _S(404), _S(334), &w, &h);
    size_t sz = 0;
    BYTE* const tmpl = _BuildEmptyDialogTemplate(
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | WS_VISIBLE | DS_CENTER,
        L"编辑模板", w, h, &sz);
    if (!tmpl) {
        FreeMem(d);
        return;
    }
    DialogBoxIndirectParam(GetModuleHandleW(NULL), (DLGTEMPLATE*)tmpl,
                           hwndOwner, _TeDlgProc, (LPARAM)d);
    FreeMem(tmpl);
}

// ----------------------------------------------------------------------------

static int _S(int value) {
    HDC const hdc = GetDC(NULL);
    int const dpi = hdc ? max_i(GetDeviceCaps(hdc, LOGPIXELSX), USER_DEFAULT_SCREEN_DPI) : USER_DEFAULT_SCREEN_DPI;
    if (hdc) {
        ReleaseDC(NULL, hdc);
    }
    return MulDiv(value, dpi, 96);
}

static void _WindowSize(DWORD exStyle, DWORD style, int clientW, int clientH, int* w, int* h) {
    RECT rc = { 0, 0, _S(clientW), _S(clientH) };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    *w = rc.right - rc.left;
    *h = rc.bottom - rc.top;
}

// The dialog template size is converted from pixels to dialog units, which
// Windows resolves with the font of the current monitor: on scaled displays the
// frame ends up much larger (or smaller) than the pixels the controls are laid
// out in, leaving a huge empty area or clipping the content.  Resizing to the
// intended client size once the dialog exists keeps frame and controls aligned
// at every DPI setting.
static void _FitDialogClient(HWND hwnd, int clientW, int clientH) {
    RECT rc = { 0, 0, max_i(clientW, 1), max_i(clientH, 1) };
    DWORD const style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD const exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    int const winW = rc.right - rc.left;
    int const winH = rc.bottom - rc.top;

    HWND const owner = GetWindow(hwnd, GW_OWNER);
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    HMONITOR const hmon = MonitorFromWindow((owner && IsWindow(owner)) ? owner : hwnd,
                                            MONITOR_DEFAULTTONEAREST);
    if (hmon && GetMonitorInfoW(hmon, &mi)) {
        work = mi.rcWork;
    }
    int const w = min_i(winW, max_i((int)(work.right - work.left), 200));
    int const h = min_i(winH, max_i((int)(work.bottom - work.top), 150));
    int x = 0;
    int y = 0;
    RECT own;
    if (owner && IsWindow(owner) && GetWindowRect(owner, &own)) {
        x = own.left + ((own.right - own.left) - w) / 2;
        y = own.top + ((own.bottom - own.top) - h) / 2;
    } else {
        x = work.left + ((work.right - work.left) - w) / 2;
        y = work.top + ((work.bottom - work.top) - h) / 2;
    }
    x = max_i(x, (int)work.left);
    y = max_i(y, (int)work.top);
    SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static HWND _Ctrl(HWND hwnd, LPCWSTR pszClass, LPCWSTR pszText, DWORD style,
                  int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, pszClass, pszText, WS_CHILD | WS_VISIBLE | style,
                           _S(x), _S(y), _S(w), _S(h), hwnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static bool _Checked(HWND hwnd, int id) {
    return (SendDlgItemMessageW(hwnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

static void _SetCheck(HWND hwnd, int id, bool value) {
    SendDlgItemMessageW(hwnd, id, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
}

// ----------------------------------------------------------------------------

void CeSettings_ResetDefaults(void) {
    Settings2.CeClipboardVariable = true;
    Settings2.CeAutoArguments = true;
    Settings2.CeAutoPairCode = true;
    Settings2.CeAutoFormatOnSave = true;
    Settings2.CeDynamicCompletions = true;
    Settings2.CeUseLsp = true;
    Settings2.CeLspFallback = true;
    Path_Reset(Settings2.CeLspServerPath, L"");
    Settings2.CeLspServerArgs[0] = L'\0';
    Settings2.CeLspDebounceMs = 120;
    Settings2.CeLspSemanticTokens = true;
    Settings2.CeLspInlayHints = false;
    Settings2.CeLspCodeLens = false;
    Settings2.CeLspDocDiagnostics = true;
    Settings2.CeWrapAsmSymbols = true;
    Settings2.CeHighlightUserSymbols = true;
    Settings2.CeLintSemantic = true;
    Settings2.CeDictionaryPath[0] = L'\0';
    StringCchCopyW(Settings2.CeAutoArgFunctions, COUNTOF(Settings2.CeAutoArgFunctions),
                   CE_DEFAULT_AUTO_ARG_FUNCTIONS);
    CeTemplate_ResetDefaults();
}

bool CeSettings_AllowClipboard(void) { return Settings2.CeClipboardVariable; }
bool CeSettings_AutoArguments(void) { return Settings2.CeAutoArguments; }
bool CeSettings_AutoPairCode(void) { return Settings2.CeAutoPairCode; }
bool CeSettings_AutoFormatOnSave(void) { return Settings2.CeAutoFormatOnSave; }
bool CeSettings_DynamicCompletions(void) { return Settings2.CeDynamicCompletions; }
bool CeSettings_WrapAsmSymbols(void) { return Settings2.CeWrapAsmSymbols; }
bool CeSettings_HighlightUserSymbols(void) { return Settings2.CeHighlightUserSymbols; }
const WCHAR* CeSettings_DictionaryPath(void) { return Settings2.CeDictionaryPath; }

// The CE formatter, templates and generated statements must indent exactly as
// a TAB press does.  Both therefore read the current file's editor settings.
int CeSettings_IndentWidth(void) {
    int width = Globals.fvCurFile.iIndentWidth;
    if (width <= 0) {
        width = Globals.fvCurFile.iTabWidth;
    }
    return clampi(width, 1, 32);
}

int CeSettings_IndentUnit(char* pszBuf, size_t cchBuf) {
    if (!pszBuf || cchBuf < 2) {
        return 0;
    }
    if (!Globals.fvCurFile.bTabsAsSpaces) {
        pszBuf[0] = '\t';
        pszBuf[1] = '\0';
        return 1;
    }
    int const width = min_i(CeSettings_IndentWidth(), (int)cchBuf - 1);
    for (int i = 0; i < width; ++i) {
        pszBuf[i] = ' ';
    }
    pszBuf[width] = '\0';
    return width;
}

bool CeSettings_IsAutoArgFunction(const char* utf8Name) {
    if (StrIsEmptyA(utf8Name)) {
        return false;
    }
    WCHAR wide[256];
    MultiByteToWideChar(CP_UTF8, 0, utf8Name, -1, wide, (int)COUNTOF(wide));

    WCHAR list[HUGE_BUFFER];
    StringCchCopyW(list, COUNTOF(list), Settings2.CeAutoArgFunctions);
    WCHAR* ctx = NULL;
    WCHAR* token = wcstok_s(list, L",;，、 \t\r\n", &ctx);
    while (token) {
        if (_wcsicmp(token, wide) == 0) {
            return true;
        }
        token = wcstok_s(NULL, L",;，、 \t\r\n", &ctx);
    }
    return false;
}

// ----------------------------------------------------------------------------

// SaveAllSettings() bails out when the INI file itself is open in the editor
// (Flags.bSettingsFileSoftLocked), and it also reports success when
// OpenSettingsFile() fails.  Both cases silently lose the dialog's choices.
// Always (re)write just the CE section through the INI cache as well, so the
// CE options survive a restart in those cases.
static void _PersistCeSettings(void) {
    SaveAllSettings(true); // general settings, best effort

    if (OpenSettingsFile("CeSettings")) {
        LPCWSTR const sec = Constants.Settings2_Section;
        IniSectionSetBool(sec, L"CeClipboardVariable", Settings2.CeClipboardVariable);
        IniSectionSetBool(sec, L"CeAutoArguments", Settings2.CeAutoArguments);
        IniSectionSetBool(sec, L"CeAutoPairCode", Settings2.CeAutoPairCode);
        IniSectionSetBool(sec, L"CeAutoFormatOnSave", Settings2.CeAutoFormatOnSave);
        IniSectionSetBool(sec, L"CeDynamicCompletions", Settings2.CeDynamicCompletions);
        IniSectionSetBool(sec, L"CeWrapAsmSymbols", Settings2.CeWrapAsmSymbols);
        IniSectionSetBool(sec, L"CeHighlightUserSymbols", Settings2.CeHighlightUserSymbols);
        IniSectionSetBool(sec, L"CeLintSemantic", Settings2.CeLintSemantic);
        IniSectionSetString(sec, L"CeDictionaryPath", Settings2.CeDictionaryPath);
        IniSectionSetString(sec, L"CeInstallPath", Settings2.CeInstallPath);
        IniSectionSetString(sec, L"CeAutoArgFunctions", Settings2.CeAutoArgFunctions);
        WCHAR szTemplates[XXHUGE_BUFFER] = { L'\0' };
        CeTemplate_Serialize(szTemplates, COUNTOF(szTemplates));
        IniSectionSetString(sec, L"CeTemplates", szTemplates);
        CloseSettingsFile("CeSettings", true);
    }
}

static void _RefreshTemplateList(HWND hwnd) {
    HWND const hList = GetDlgItem(hwnd, IDC_CE_LIST_TEMPLATES);
    if (!hList) {
        return;
    }
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < CeTemplate_Count(); ++i) {
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)CeTemplate_Name(i));
    }
    if (CeTemplate_Count() > 0) {
        SendMessageW(hList, LB_SETCURSEL, 0, 0);
    }
}

static void _LoadValues(HWND hwnd) {
    _SetCheck(hwnd, IDC_CE_CHK_CLIPBOARD, Settings2.CeClipboardVariable);
    _SetCheck(hwnd, IDC_CE_CHK_AUTOARGS, Settings2.CeAutoArguments);
    _SetCheck(hwnd, IDC_CE_CHK_AUTOPAIR, Settings2.CeAutoPairCode);
    _SetCheck(hwnd, IDC_CE_CHK_FORMAT, Settings2.CeAutoFormatOnSave);
    _SetCheck(hwnd, IDC_CE_CHK_DYNAMIC, Settings2.CeDynamicCompletions);
    _SetCheck(hwnd, IDC_CE_CHK_WRAPASM, Settings2.CeWrapAsmSymbols);
    _SetCheck(hwnd, IDC_CE_CHK_HIGHLIGHT, Settings2.CeHighlightUserSymbols);

    SetDlgItemTextW(hwnd, IDC_CE_EDIT_FUNCS, Settings2.CeAutoArgFunctions);
    WCHAR indent[64];
    StringCchPrintfW(indent, COUNTOF(indent), L"%d（跟随程序设置）", CeSettings_IndentWidth());
    SetDlgItemTextW(hwnd, IDC_CE_EDIT_INDENT, indent);
    SetDlgItemTextW(hwnd, IDC_CE_EDIT_DICT, Settings2.CeDictionaryPath[0] ?
                    Settings2.CeDictionaryPath : L"<程序目录>\\CeDictionary.txt");

    _RefreshTemplateList(hwnd);
}

static void _RestoreSettingsSnapshot(HWND hwnd) {
    SETTINGS2_T* const snap = (SETTINGS2_T*)GetWindowLongPtrW(hwnd, DWLP_USER);
    if (snap) {
        Settings2 = *snap;
    }
}

static void _SaveValues(HWND hwnd) {
    Settings2.CeClipboardVariable = _Checked(hwnd, IDC_CE_CHK_CLIPBOARD);
    Settings2.CeAutoArguments = _Checked(hwnd, IDC_CE_CHK_AUTOARGS);
    Settings2.CeAutoPairCode = _Checked(hwnd, IDC_CE_CHK_AUTOPAIR);
    Settings2.CeAutoFormatOnSave = _Checked(hwnd, IDC_CE_CHK_FORMAT);
    Settings2.CeDynamicCompletions = _Checked(hwnd, IDC_CE_CHK_DYNAMIC);
    Settings2.CeWrapAsmSymbols = _Checked(hwnd, IDC_CE_CHK_WRAPASM);
    Settings2.CeHighlightUserSymbols = _Checked(hwnd, IDC_CE_CHK_HIGHLIGHT);

    WCHAR dict[MAX_PATH];
    GetDlgItemTextW(hwnd, IDC_CE_EDIT_DICT, dict, COUNTOF(dict));
    if (wcsstr(dict, L"CeDictionary.txt") && dict[0] == L'<') {
        dict[0] = L'\0'; // the placeholder means "use the default path"
    }
    StringCchCopyW(Settings2.CeDictionaryPath, COUNTOF(Settings2.CeDictionaryPath), dict);

    GetDlgItemTextW(hwnd, IDC_CE_EDIT_FUNCS, Settings2.CeAutoArgFunctions,
                    COUNTOF(Settings2.CeAutoArgFunctions));

    // The template list is edited in place (via the popup editor), so it is
    // already reflected in Settings2.CeTemplates; nothing to parse here.
}

// ----------------------------------------------------------------------------

static INT_PTR CALLBACK _CeSettingsDlgProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (umsg) {
    case WM_INITDIALOG: {
        HFONT const hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        #define ADD_CTRL(cls, text, style, cx, cy, cw, ch, id)                       \
            do {                                                                      \
                HWND const c = _Ctrl(hwnd, cls, text, style, cx, cy, cw, ch, id);     \
                if (c) { SendMessageW(c, WM_SETFONT, (WPARAM)hFont, TRUE); }           \
            } while (0)

        ADD_CTRL(L"STATIC", L"代码补全", SS_LEFT, 12, 10, 200, 18, IDC_CE_LBL_COMPLETION);
        ADD_CTRL(L"BUTTON", L"允许从剪贴板读取已定义变量", BS_AUTOCHECKBOX,
                 12, 32, 500, 20, IDC_CE_CHK_CLIPBOARD);
        ADD_CTRL(L"BUTTON", L"函数补全时自动填入变量（registersymbol(axcc)）", BS_AUTOCHECKBOX,
                 12, 56, 500, 20, IDC_CE_CHK_AUTOARGS);
        ADD_CTRL(L"BUTTON", L"输入 alloc(...) 后回车自动生成配套代码", BS_AUTOCHECKBOX,
                 12, 80, 500, 20, IDC_CE_CHK_AUTOPAIR);
        ADD_CTRL(L"BUTTON", L"保存时自动格式化并缩进（跟随程序缩进设置）", BS_AUTOCHECKBOX,
                 12, 104, 500, 20, IDC_CE_CHK_FORMAT);
        ADD_CTRL(L"BUTTON", L"补全时包含当前文档已定义符号（支持局部匹配）", BS_AUTOCHECKBOX,
                 12, 128, 500, 20, IDC_CE_CHK_DYNAMIC);
        ADD_CTRL(L"BUTTON", L"汇编代码中键入或补全自定义变量名时自动包裹 []", BS_AUTOCHECKBOX,
                 12, 152, 500, 20, IDC_CE_CHK_WRAPASM);
        ADD_CTRL(L"BUTTON", L"高亮用户自定义变量名与 label 标签名", BS_AUTOCHECKBOX,
                 12, 176, 500, 20, IDC_CE_CHK_HIGHLIGHT);

        ADD_CTRL(L"STATIC", L"自动填参函数（逗号分隔）", SS_LEFT, 12, 204, 240, 18, IDC_CE_LBL_FUNCS);
        ADD_CTRL(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                 12, 224, 560, 24, IDC_CE_EDIT_FUNCS);
        ADD_CTRL(L"STATIC", L"缩进宽度（与程序设置共用）", SS_LEFT, 12, 254, 170, 18, IDC_CE_LBL_INDENT);
        ADD_CTRL(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER,
                 190, 252, 170, 24, IDC_CE_EDIT_INDENT);
        ADD_CTRL(L"STATIC", L"外部词典：每行 名称|签名|分类|说明（UTF-8；留空用程序目录 CeDictionary.txt）",
                 SS_LEFT, 12, 280, 570, 18, IDC_CE_LBL_DICT);
        ADD_CTRL(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                 12, 300, 440, 24, IDC_CE_EDIT_DICT);
        ADD_CTRL(L"BUTTON", L"重新载入设置", BS_PUSHBUTTON | WS_TABSTOP,
                 462, 300, 110, 24, IDC_CE_BTN_RELOAD);

        ADD_CTRL(L"STATIC", L"代码模板（双击或点“编辑”修改；内容支持换行，$ver$ 为变量占位符）",
                 SS_LEFT, 12, 328, 560, 18, IDC_CE_LBL_TEMPLATES);
        ADD_CTRL(L"LISTBOX", L"", LBS_NOTIFY | LBS_HASSTRINGS | WS_BORDER | WS_VSCROLL | WS_TABSTOP,
                 12, 348, 340, 150, IDC_CE_LIST_TEMPLATES);
        ADD_CTRL(L"BUTTON", L"新增", BS_PUSHBUTTON | WS_TABSTOP,
                 366, 348, 110, 26, IDC_CE_BTN_NEW);
        ADD_CTRL(L"BUTTON", L"编辑", BS_PUSHBUTTON | WS_TABSTOP,
                 366, 380, 110, 26, IDC_CE_BTN_EDIT);
        ADD_CTRL(L"BUTTON", L"删除", BS_PUSHBUTTON | WS_TABSTOP,
                 366, 412, 110, 26, IDC_CE_BTN_DELETE);
        ADD_CTRL(L"BUTTON", L"插入选中模板", BS_PUSHBUTTON | WS_TABSTOP,
                 366, 470, 110, 26, IDC_CE_BTN_INSERT);

        ADD_CTRL(L"BUTTON", L"恢复默认", BS_PUSHBUTTON | WS_TABSTOP,
                 12, 538, 110, 26, IDC_CE_BTN_DEFAULTS);
        ADD_CTRL(L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP,
                 360, 538, 100, 26, IDC_CE_BTN_OK);
        ADD_CTRL(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP,
                 472, 538, 100, 26, IDC_CE_BTN_CANCEL);

        #undef ADD_CTRL

        SETTINGS2_T* const snap = (SETTINGS2_T*)AllocMem(sizeof(SETTINGS2_T), HEAP_ZERO_MEMORY);
        if (snap) {
            *snap = Settings2;   // Cancel restores scalars and templates together
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)snap);
        }
        _LoadValues(hwnd);
        _FitDialogClient(hwnd, _S(596), _S(580));
        SetFocus(focusTemplatesForShow
                     ? GetDlgItem(hwnd, IDC_CE_LIST_TEMPLATES)
                     : GetDlgItem(hwnd, IDC_CE_EDIT_FUNCS));
        return TRUE;
    }

    case WM_COMMAND:
        if ((LOWORD(wParam) == IDC_CE_LIST_TEMPLATES) &&
            (HIWORD(wParam) == LBN_DBLCLK)) {
            int const idx = (int)SendDlgItemMessageW(hwnd, IDC_CE_LIST_TEMPLATES,
                                                     LB_GETCURSEL, 0, 0);
            if (idx >= 0) {
                _EditTemplate(hwnd, idx);
            }
            return TRUE;
        }
        switch (LOWORD(wParam)) {
        case IDC_CE_BTN_OK:
            _SaveValues(hwnd);
            _PersistCeSettings();
            CeLspClient_ResetStartFailure();
            CeLspFeatures_RefreshDecorations();
            CeCompletion_ReloadDictionary();
            CeLint_Invalidate(); // CeLintSemantic etc. may have changed
            CeHighlight_Refresh(true);
            EndDialog(hwnd, 1);
            return TRUE;

        case IDC_CE_BTN_RELOAD:
            // Discard the edits in this dialog and re-read what is on disk.
            LoadCeSettings();
            CeLspClient_ResetStartFailure();
            CeLspFeatures_RefreshDecorations();
            CeCompletion_ReloadDictionary();
            CeLint_Invalidate();
            CeHighlight_Refresh(true);
            _LoadValues(hwnd);
            return TRUE;

        case IDC_CE_BTN_CANCEL:
        case IDCANCEL:   // ESC / dialog close button routed by the dialog manager
            _RestoreSettingsSnapshot(hwnd);
            EndDialog(hwnd, 0);
            return TRUE;

        case IDC_CE_BTN_DEFAULTS:
            CeSettings_ResetDefaults();
            _LoadValues(hwnd);
            return TRUE;

        case IDC_CE_BTN_NEW:
            _EditTemplate(hwnd, -1);
            return TRUE;

        case IDC_CE_BTN_EDIT: {
            int const idx = (int)SendDlgItemMessageW(hwnd, IDC_CE_LIST_TEMPLATES,
                                                     LB_GETCURSEL, 0, 0);
            if (idx >= 0) {
                _EditTemplate(hwnd, idx);
            }
            return TRUE;
        }

        case IDC_CE_BTN_DELETE: {
            int const idx = (int)SendDlgItemMessageW(hwnd, IDC_CE_LIST_TEMPLATES,
                                                     LB_GETCURSEL, 0, 0);
            if (idx >= 0) {
                CeTemplate_Remove(idx);
                _RefreshTemplateList(hwnd);
            }
            return TRUE;
        }

        case IDC_CE_BTN_INSERT: {
            int const index = (int)SendDlgItemMessageW(hwnd, IDC_CE_LIST_TEMPLATES,
                                                     LB_GETCURSEL, 0, 0);
            _SaveValues(hwnd); // keep edits made just before inserting
            _PersistCeSettings();
            if (index >= 0) {
                CeTemplate_Insert(index, Globals.hwndMain);
            }
            if (IsWindow(Globals.hwndEdit)) {
                SetFocus(Globals.hwndEdit);
            }
            EndDialog(hwnd, 1);
            return TRUE;
        }
        default:
            break;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            _RestoreSettingsSnapshot(hwnd);
            EndDialog(hwnd, 0);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        _RestoreSettingsSnapshot(hwnd);
        EndDialog(hwnd, 0);
        return TRUE;

    case WM_DESTROY: {
        SETTINGS2_T* const snap = (SETTINGS2_T*)GetWindowLongPtrW(hwnd, DWLP_USER);
        if (snap) {
            FreeMem(snap);
        }
        return FALSE;
    }

    default:
        break;
    }
    return FALSE;
}

void CeSettings_Show(HWND hwndParent, bool focusTemplates) {
    int w = 0;
    int h = 0;
    _WindowSize(WS_EX_DLGMODALFRAME, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                _S(596), _S(580), &w, &h);
    size_t sz = 0;
    BYTE* const tmpl = _BuildEmptyDialogTemplate(
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | WS_VISIBLE | DS_CENTER,
        L"CE 编辑器设置 / 代码模板", w, h, &sz);
    if (!tmpl) {
        return;
    }
    // Stash the initial focus preference where WM_INITDIALOG can read it.
    focusTemplatesForShow = focusTemplates;
    DialogBoxIndirectParam(GetModuleHandleW(NULL), (DLGTEMPLATE*)tmpl,
                           hwndParent, _CeSettingsDlgProc, 0);
    FreeMem(tmpl);
}

// ----------------------------------------------------------------------------
// Small modal input box (used by the CE symbol rename command).  Reusing the
// runtime dialog template keeps every language resource untouched.

enum CE_INPUTBOX_CTRL_IDS {
    IDC_CE_IB_LBL = 3000,
    IDC_CE_IB_EDIT
};

typedef struct _CE_INPUTBOX_DATA {
    LPCWSTR prompt;
    WCHAR*  buffer;
    size_t  cch;
} CE_INPUTBOX_DATA;

static INT_PTR CALLBACK _CeInputBoxDlgProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) {
    switch (umsg) {
    case WM_INITDIALOG: {
        CE_INPUTBOX_DATA* const d = (CE_INPUTBOX_DATA*)lParam;
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)d);
        HFONT const hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        #define IB_CTRL(cls, text, style, cx, cy, cw, ch, id)                     \
            do {                                                                  \
                HWND const c = _Ctrl(hwnd, cls, text, style, cx, cy, cw, ch, id);  \
                if (c) { SendMessageW(c, WM_SETFONT, (WPARAM)hFont, TRUE); }        \
            } while (0)

        IB_CTRL(L"STATIC", d ? d->prompt : L"", SS_LEFT, 12, 12, 330, 18, IDC_CE_IB_LBL);
        IB_CTRL(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                12, 34, 336, 24, IDC_CE_IB_EDIT);
        IB_CTRL(L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP,
                164, 72, 88, 26, IDOK);
        IB_CTRL(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP,
                260, 72, 88, 26, IDCANCEL);

        #undef IB_CTRL

        if (d) {
            SetDlgItemTextW(hwnd, IDC_CE_IB_EDIT, d->buffer);
        }
        SendDlgItemMessageW(hwnd, IDC_CE_IB_EDIT, EM_SETSEL, 0, -1);
        _FitDialogClient(hwnd, _S(360), _S(112));
        SetFocus(GetDlgItem(hwnd, IDC_CE_IB_EDIT));
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            CE_INPUTBOX_DATA* const d = (CE_INPUTBOX_DATA*)GetWindowLongPtrW(hwnd, DWLP_USER);
            if (d) {
                GetDlgItemTextW(hwnd, IDC_CE_IB_EDIT, d->buffer, (int)d->cch);
            }
            EndDialog(hwnd, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hwnd, 0);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hwnd, 0);
        return TRUE;

    default:
        break;
    }
    return FALSE;
}

bool CeSettings_InputBox(HWND hwndParent, LPCWSTR title, LPCWSTR prompt,
                         WCHAR* buffer, size_t cch) {
    if (!buffer || (cch == 0)) {
        return false;
    }
    CE_INPUTBOX_DATA data = { prompt ? prompt : L"", buffer, cch };
    int w = 0;
    int h = 0;
    _WindowSize(WS_EX_DLGMODALFRAME, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                _S(360), _S(112), &w, &h);
    size_t sz = 0;
    BYTE* const tmpl = _BuildEmptyDialogTemplate(
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | WS_VISIBLE | DS_CENTER,
        title ? title : L"", w, h, &sz);
    if (!tmpl) {
        return false;
    }
    INT_PTR const res = DialogBoxIndirectParam(GetModuleHandleW(NULL), (DLGTEMPLATE*)tmpl,
                                              hwndParent, _CeInputBoxDlgProc, (LPARAM)&data);
    FreeMem(tmpl);
    return (res == 1);
}

// ----------------------------------------------------------------------------
