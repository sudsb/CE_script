// encoding: UTF-8
/******************************************************************************
*                                                                             *
* Notepad3                                                                    *
*                                                                             *
* CeReference.c                                                               *
*   Modeless Cheat Engine API reference panel.                                *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <commctrl.h>
#include <shlwapi.h>

#include "Dialogs.h"
#include "DarkMode/DarkMode.h"
#include "CeCompletion.h"
#include "..\language\common_res.h"

// ----------------------------------------------------------------------------

// Rebuilding the 2146-row list on every keystroke is wasteful while the user is
// still typing a filter; coalesce EN_CHANGE into one refresh.
#define IDT_CEREF_FILTER 1
#define CEREF_FILTER_DELAY_MS 150

static void _Utf8ToWide(const char* pszText, wchar_t* pszWide, size_t cchWide)
{
    if (!pszText || !pszWide || (cchWide == 0)) {
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, pszText, -1, pszWide, (int)cchWide);
    pszWide[cchWide - 1] = L'\0';
}

static int _SelectedCategory(HWND hwnd)
{
    LRESULT const sel = SendDlgItemMessageW(hwnd, IDC_CEREF_CATEGORY, CB_GETCURSEL, 0, 0);
    if (sel <= 0) {
        return -1;
    }
    return (int)SendDlgItemMessageW(hwnd, IDC_CEREF_CATEGORY, CB_GETITEMDATA, (WPARAM)sel, 0);
}

static int _SelectedEntry(HWND hwnd)
{
    HWND const hList = GetDlgItem(hwnd, IDC_CEREF_LIST);
    int const item = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (item < 0) {
        return -1;
    }
    LVITEMW lvItem = { 0 };
    lvItem.mask = LVIF_PARAM;
    lvItem.iItem = item;
    if (!ListView_GetItem(hList, &lvItem)) {
        return -1;
    }
    return (int)lvItem.lParam;
}

static void _UpdateDetail(HWND hwnd)
{
    int const index = _SelectedEntry(hwnd);
    if ((index < 0) || (index >= g_ceCompletionCount)) {
        SetDlgItemTextW(hwnd, IDC_CEREF_DETAIL, L"");
        return;
    }

    const CE_COMPLETION_ENTRY* const pEntry = &g_ceCompletions[index];
    wchar_t wname[512];
    wchar_t wsig[1024];
    wchar_t wcat[256];
    wchar_t wdoc[4096];
    wchar_t wdetail[6144];
    _Utf8ToWide(pEntry->name, wname, COUNTOF(wname));
    _Utf8ToWide(pEntry->signature, wsig, COUNTOF(wsig));
    _Utf8ToWide(pEntry->category, wcat, COUNTOF(wcat));

    // Usage notes are the same multiline UTF-8 text the AutoC detail pane
    // shows; keywords without a note keep the old "category + signature" view.
    const char* const pszDoc = CeCompletion_FindDoc(pEntry->name);
    if (pszDoc && pszDoc[0]) {
        _Utf8ToWide(pszDoc, wdoc, COUNTOF(wdoc));
        StringCchPrintfW(wdetail, COUNTOF(wdetail), L"%s  \u00B7  %s\n%s\n%s",
                         wcat, wname, wsig, wdoc);
    }
    else {
        StringCchPrintfW(wdetail, COUNTOF(wdetail), L"%s\n%s", wcat, wsig);
    }
    SetDlgItemTextW(hwnd, IDC_CEREF_DETAIL, wdetail);
}

// The shipped dialog templates reserve two lines for the detail text.  Give
// the multiline usage note room at runtime so every MUI language template and
// DPI scaling stay untouched; SS_EDITCONTROL makes the static wrap long lines.
static void _EnlargeDetailPane(HWND hwnd)
{
    HWND const hList = GetDlgItem(hwnd, IDC_CEREF_LIST);
    HWND const hDetail = GetDlgItem(hwnd, IDC_CEREF_DETAIL);
    if (!hList || !hDetail) {
        return;
    }
    LONG_PTR const style = GetWindowLongPtrW(hDetail, GWL_STYLE);
    if (!(style & SS_EDITCONTROL)) {
        SetWindowLongPtrW(hDetail, GWL_STYLE, style | SS_EDITCONTROL);
    }

    RECT rcList = { 0 };
    RECT rcDetail = { 0 };
    GetWindowRect(hList, &rcList);
    GetWindowRect(hDetail, &rcDetail);
    MapWindowPoints(NULL, hwnd, (LPPOINT)&rcList, 2);
    MapWindowPoints(NULL, hwnd, (LPPOINT)&rcDetail, 2);

    int const detailH = rcDetail.bottom - rcDetail.top;
    int const listH = rcList.bottom - rcList.top;
    int const grow = detailH * 3;
    if ((grow <= 0) || (listH <= grow + detailH)) {
        return;
    }
    SetWindowPos(hDetail, NULL, rcDetail.left, rcDetail.top - grow,
                 rcDetail.right - rcDetail.left, detailH + grow,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(hList, NULL, rcList.left, rcList.top,
                 rcList.right - rcList.left, listH - grow,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static void _RefreshList(HWND hwnd)
{
    HWND const hList = GetDlgItem(hwnd, IDC_CEREF_LIST);

    wchar_t wfilter[256] = { 0 };
    GetDlgItemTextW(hwnd, IDC_CEREF_FILTER, wfilter, COUNTOF(wfilter));
    char filter[1024] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, wfilter, -1, filter, (int)COUNTOF(filter), NULL, NULL);

    int const category = _SelectedCategory(hwnd);

    SendMessage(hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hList);

    int shown = 0;
    for (int i = 0; i < g_ceCompletionCount; ++i) {
        const CE_COMPLETION_ENTRY* const pEntry = &g_ceCompletions[i];
        if ((category >= 0) && (strcmp(pEntry->category, g_ceCategories[category]) != 0)) {
            continue;
        }
        // A reader who does not know the exact keyword can search the Chinese
        // usage note instead (e.g. "拷贝" finds mov).  Only look the note up
        // when a filter is active, so an unfiltered refresh stays cheap.
        if (filter[0] && !StrStrIA(pEntry->name, filter) &&
            !StrStrIA(pEntry->signature, filter) && !StrStrIA(pEntry->category, filter)) {
            const char* const pszDoc = CeCompletion_FindDoc(pEntry->name);
            if (!(pszDoc && StrStrIA(pszDoc, filter))) {
                continue;
            }
        }

        wchar_t wname[512];
        wchar_t wsig[1024];
        _Utf8ToWide(pEntry->name, wname, COUNTOF(wname));
        _Utf8ToWide(pEntry->signature, wsig, COUNTOF(wsig));

        LVITEMW lvItem = { 0 };
        lvItem.mask = LVIF_TEXT | LVIF_PARAM;
        lvItem.iItem = shown;
        lvItem.pszText = wname;
        lvItem.lParam = (LPARAM)i;
        if (ListView_InsertItem(hList, &lvItem) < 0) {
            continue;
        }
        ListView_SetItemText(hList, shown, 1, wsig);
        ++shown;
    }

    wchar_t wcount[64];
    StringCchPrintfW(wcount, COUNTOF(wcount), L"%d of %d entries", shown, g_ceCompletionCount);
    SetDlgItemTextW(hwnd, IDC_CEREF_COUNT, wcount);

    if (shown > 0) {
        ListView_SetItemState(hList, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, 0, FALSE);
    }
    _UpdateDetail(hwnd);

    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, TRUE);
}

static void _InsertSelected(HWND hwnd)
{
    int const index = _SelectedEntry(hwnd);
    if ((index < 0) || (index >= g_ceCompletionCount)) {
        return;
    }
    CeCompletion_InsertEntry(&g_ceCompletions[index]);
    if (IsWindow(Globals.hwndEdit)) {
        SetFocus(Globals.hwndEdit);
    }
}

// ----------------------------------------------------------------------------

static INT_PTR CALLBACK _CeReferenceDlgProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam)
{
    switch (umsg) {
    case WM_INITDIALOG: {
        SetDialogIconNP3(hwnd);
        InitWindowCommon(hwnd, true);

        HWND const hList = GetDlgItem(hwnd, IDC_CEREF_LIST);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW col = { 0 };
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = (LPWSTR)L"Name";
        col.cx = 150;
        col.iSubItem = 0;
        ListView_InsertColumn(hList, 0, &col);
        col.pszText = (LPWSTR)L"Signature";
        col.cx = 250;
        col.iSubItem = 1;
        ListView_InsertColumn(hList, 1, &col);

        SendDlgItemMessageW(hwnd, IDC_CEREF_CATEGORY, CB_ADDSTRING, 0, (LPARAM)L"All categories");
        SendDlgItemMessageW(hwnd, IDC_CEREF_CATEGORY, CB_SETITEMDATA, 0, (LPARAM)-1);
        for (int i = 0; i < g_ceCategoryCount; ++i) {
            wchar_t wcat[256];
            _Utf8ToWide(g_ceCategories[i], wcat, COUNTOF(wcat));
            LRESULT const idx = SendDlgItemMessageW(hwnd, IDC_CEREF_CATEGORY, CB_ADDSTRING, 0, (LPARAM)wcat);
            if (idx >= 0) {
                SendDlgItemMessageW(hwnd, IDC_CEREF_CATEGORY, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
            }
        }
        SendDlgItemMessageW(hwnd, IDC_CEREF_CATEGORY, CB_SETCURSEL, 0, 0);

        _RefreshList(hwnd);
        _EnlargeDetailPane(hwnd);
        CenterDlgInParent(hwnd, false);
        SetFocus(GetDlgItem(hwnd, IDC_CEREF_FILTER));
    }
    return TRUE;

#ifdef D_NP3_WIN10_DARK_MODE

    CASE_WM_CTLCOLOR_SET:
        return SetDarkModeCtlColors((HDC)wParam, UseDarkMode());

    case WM_SETTINGCHANGE:
        if (IsDarkModeSupported() && IsColorSchemeChangeMessage(lParam)) {
            SendMessage(hwnd, WM_THEMECHANGED, 0, 0);
        }
        break;

    case WM_THEMECHANGED:
        if (IsDarkModeSupported()) {
            bool const darkModeEnabled = CheckDarkModeEnabled();
            AllowDarkModeForWindowEx(hwnd, darkModeEnabled);
            RefreshTitleBarThemeColor(hwnd);
            AllowDarkModeForWindowEx(GetDlgItem(hwnd, IDC_CEREF_LIST), darkModeEnabled);
            AllowDarkModeForWindowEx(GetDlgItem(hwnd, IDC_CEREF_FILTER), darkModeEnabled);
            AllowDarkModeForWindowEx(GetDlgItem(hwnd, IDC_CEREF_CATEGORY), darkModeEnabled);
            AllowDarkModeForWindowEx(GetDlgItem(hwnd, IDC_CEREF_INSERT), darkModeEnabled);
            AllowDarkModeForWindowEx(GetDlgItem(hwnd, IDCANCEL), darkModeEnabled);
            UpdateWindowEx(hwnd);
        }
        break;

#endif

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CEREF_FILTER:
            if (HIWORD(wParam) == EN_CHANGE) {
                SetTimer(hwnd, IDT_CEREF_FILTER, CEREF_FILTER_DELAY_MS, NULL);
            }
            break;

        case IDC_CEREF_CATEGORY:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                _RefreshList(hwnd);
            }
            break;

        case IDC_CEREF_INSERT:
            _InsertSelected(hwnd);
            break;

        case IDCANCEL:
            DestroyWindow(hwnd);
            break;

        default:
            break;
        }
        return TRUE;

    case WM_NOTIFY: {
        NMHDR const* const pnmh = (NMHDR*)lParam;
        if (pnmh->idFrom == IDC_CEREF_LIST) {
            if (pnmh->code == LVN_ITEMCHANGED) {
                _UpdateDetail(hwnd);
            } else if (pnmh->code == NM_DBLCLK) {
                _InsertSelected(hwnd);
            }
        }
    }
    return FALSE;

    case WM_TIMER:
        if (wParam == IDT_CEREF_FILTER) {
            KillTimer(hwnd, IDT_CEREF_FILTER);
            _RefreshList(hwnd);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return TRUE;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_CEREF_FILTER);
        Globals.hwndCeRef = NULL;
        break;

    default:
        break;
    }
    return FALSE;
}

// ----------------------------------------------------------------------------

void CeReference_Show(HWND hwndParent)
{
    if (IsWindow(Globals.hwndCeRef)) {
        ShowWindow(Globals.hwndCeRef, SW_SHOW);
        SetForegroundWindow(Globals.hwndCeRef);
        return;
    }

    Globals.hwndCeRef = CreateThemedDialogParam(Globals.hLngResContainer,
                                                MAKEINTRESOURCE(IDD_MUI_CEREF),
                                                hwndParent, _CeReferenceDlgProc, 0);
    if (IsWindow(Globals.hwndCeRef)) {
        ShowWindow(Globals.hwndCeRef, SW_SHOW);
        UpdateWindow(Globals.hwndCeRef);
    }
}
