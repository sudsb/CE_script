// encoding: UTF-8
/******************************************************************************
*                                                                             *
*                                                                             *
* Notepad3                                                                    *
*                                                                             *
* Notepad3.h                                                                  *
*   Global definitions and declarations                                       *
*   Based on code from Notepad2, (c) Florian Balmer 1996-2011                 *
*                                                                             *
*                                                  (c) Rizonesoft 2008-2026   *
*                                                    https://rizonesoft.com   *
*                                                                             *
*                                                                             *
*******************************************************************************/
#pragma once
#ifndef _NP3_NOTEPAD3_H_
#define _NP3_NOTEPAD3_H_

#include "TypeDefs.h"
#include "SciCall.h"

//==== Main Window ============================================================

#define ONLINE_HELP_WEBSITE L"https://rizonesoft.com/documents/notepad3"

//==== Data Type for WM_COPYDATA ==============================================
#define DATA_NOTEPAD3_PARAMS 0xFB10
typedef struct np3params {
    int                 flagFileSpecified;
    FILE_WATCHING_MODE  flagChangeNotify;
    int                 flagLexerSpecified;
    int                 iInitialLexer;
    int                 flagQuietCreate;
    int                 flagJumpTo;
    int                 iInitialLine;
    int                 iInitialColumn;
    cpi_enc_t           flagSetEncoding;
    int                 flagSetEOLMode;
    int                 flagTitleExcerpt;
    int                 flagMatchText;
    WCHAR               wchData;
}
np3params, *LPnp3params;


//==== Ids ====================================================================
#define IDC_STATUSBAR    (0xFB00)
#define IDC_TOOLBAR      (0xFB01)
#define IDC_REBAR        (0xFB02)
#define IDC_EDIT         (0xFB03)
#define IDC_EDITFRAME    (0xFB04)
#define IDC_FILENAME     (0xFB05)
#define IDC_REUSELOCK    (0xFB06)
#define IDC_MARGIN       (0xFB07)
#define IDC_NCAREA       (0xFB08)



//==== Notifications ==========================================================
#define WM_TRAYMESSAGE             (WM_USER + 1)       // Callback Message from System Tray
#define WM_FILECHANGEDNOTIFY       (WM_USER + 2)       // Change Notifications
#define IDC_FILEMRU_UPDATE_VIEW    (WM_USER + 4)
//#define WM_CHANGENOTIFYCLEAR     (WM_USER + 5)
#define WM_NEWWINDOWREQUEST        (WM_USER + 6)       // File > New Window, requested by a taskbar Shift+Click instance
#define WM_CEBRIDGE_OUTPUT         (WM_USER + 7)       // CE bridge: lParam owns a UTF-8 buffer, free it on the main thread
#define WM_CEBRIDGE_DONE           (WM_USER + 8)       // CE bridge: one script run finished
#define WM_CEBRIDGE_HINT           (WM_USER + 9)       // CE bridge: lParam owns a UTF-8 hint, freed on the main thread

// Deferred "add to CE address list": keeps the real work out of the toolbar
// WM_COMMAND call stack.  The result code lives in CeSession (atomic state).
#define WM_APP_CE_ADD_LIST         (WM_APP + 21)
#define WM_APP_CE_ADD_RESULT       (WM_APP + 22)
#define WM_APP_CE_CHECK_ONLINE     (WM_APP + 23)       // deferred CE online syntax check
#define WM_APP_CE_CHECK_RESULT     (WM_APP + 24)       // worker finished; reply in CeBridge state
#define WM_CELSP_MESSAGE           (WM_USER + 10)      // LSP client: lParam owns a UTF-8 JSON-RPC payload
#define WM_CELSP_STATUS            (WM_USER + 11)      // LSP client: wParam = CELSP_STATUS_*

//==== Timer ==================================================================
#define ID_WATCHTIMER       (0xA000)        // File Watching
#define ID_PASTEBOARDTIMER  (0xA001)        // Paste Board
#define ID_AUTOSAVETIMER    (0xA002)        // Auto Save Timer
#define ID_LOGROTATETIMER   (0xA003)        // Log Rotation Retry
#define ID_AUTOSCROLLTIMER  (0xA004)        // Middle-Click Auto-Scroll
#define ID_ATOMICSAVETIMER  (0xA005)        // Atomic Save Detection
#define ID_DEFERMINIMIZETIMER (0xA006)      // Deferred minimize on /B + /I startup
#define ID_TINYEXPRCOPYTIMER  (0xA007)      // TinyExpr status-bar single-click copy debounce
#define ID_FGLOCKRELEASETIMER (0xA008)      // Release LockSetForegroundWindow after background URL launch


//==== Reuse Window Lock Timeout ==============================================
#define REUSEWINDOWLOCKTIMEOUT  (1000)


//==== Function Declarations ==================================================
bool InitApplication(const HINSTANCE hInstance);
//~bool InitToolbarWndClass(const HINSTANCE hInstance);
HWND InitInstance(const HINSTANCE hInstance, int nCmdShow);
void CreateBars(HWND hwnd, HINSTANCE hInstance);
WININFO GetFactoryDefaultWndPos(HWND hwnd, const int flagsPos);
WININFO GetWinInfoByFlag(HWND hwnd, const int flagsPos);
int  CountRunningInstances();
bool ActivatePrevInst(const bool bSetForground);
bool LaunchNewInstance(HWND hwnd, LPCWSTR lpszParameter, LPCWSTR lpszFilePath);
bool RelaunchMultiInst();
bool RelaunchElevated(LPCWSTR lpNewCmdLnArgs);
bool DoElevatedRelaunch(EditFileIOStatus* pFioStatus, bool bAutoSaveOnRelaunch);
void ShowNotifyIcon(HWND hwnd, bool bAdd);
void SetNotifyIconTitle(HWND hwnd);
void SetSaveDone();

void ParseCommandLine();
bool CheckAutoLoadMostRecent();
void ShowZoomCallTip();
void ShowWrapAroundCallTip(bool forwardSearch);
void ShowBraceMatchCallTip(DocPos pos, UINT idsMsg);

void   NP3_ZoomIn();
void   NP3_ZoomOut();
void   NP3_ApplyZoom(int percent);
int    NP3_GetZoomPercent();

void MarkAllOccurrences(const LONG64 delay, const bool bForceClear);

void UpdateToolbar();
void UpdateToolbar_Now(const HWND hwnd);
void UpdateStatusbar(const bool bForceRedraw);
void UpdateMargins(const bool bForce);
void UpdateSaveSettingsCmds();
void UpdateTitlebar(const HWND hwnd);

void ResetMouseDWellTime();

void UndoRedoReset();
LONG BeginUndoActionSelection();
void EndUndoActionSelection(LONG token);

void HandleDWellStartEnd(const DocPos position, const UINT uid);
bool HandleHotSpotURLClicked(const DocPos position, const HYPERLINK_OPS operation);
void HandleColorDefClicked(HWND hwnd, const DocPos position);

bool    IsFindPatternEmpty();
LPCWSTR GetFindPattern();
void    GetFindPatternMB(LPSTR chPattern, int cch);
void    SetFindPattern(LPCWSTR wchFindPattern);
void    SetFindPatternMB(LPCSTR chFindPattern);
size_t  LengthOfFindPattern();
size_t  LengthOfFindPatternMB();

bool ConsistentIndentationCheck(EditFileIOStatus* status);

bool FileLoad(const HPATHL hfile_pth, const FileLoadFlags fLoadFlags, const DocPos curPos, const DocLn visLn);
bool FileSave(FileSaveFlags fSaveFlags);
bool FileRevert(const HPATHL hfile_pth, bool bIgnoreCmdLnEnc);
bool FileIO(bool fLoad, const HPATHL hfile_pth, EditFileIOStatus* status,
            FileLoadFlags fLoadFlags, FileSaveFlags fSaveFlags, bool bSetSavePoint);

void CALLBACK PasteBoardTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);
bool          IsPasteBoardActive(void);
void          InstallFileWatching(const bool bInstall);

void AutoSaveStart(bool bReset);
void AutoSaveStop();
void AutoSaveDoWork(FileSaveFlags fSaveFlags);
void CALLBACK AutoSaveTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);

//LPCWSTR BackupGetDefaultFolder(HPATHL hfile_pth_io);

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam);
LRESULT MsgCreate(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgEndSession(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam);
LRESULT MsgThemeChanged(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgDPIChanged(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgSize(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgDrawItem(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgDropFiles(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgCopyData(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgContextMenu(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam);
// Apply the CE-branch menu fixes (remove dead "Find in Files" entries, keep
// shortcut labels in sync with IDR_MAINWND, add runtime CE commands).  Must be
// re-applied after every menu reload, including a MUI language switch.
void    PatchMainMenuForCe(HMENU hmenu);
LRESULT MsgEnterMenuLoop(HWND hwnd, WPARAM wParam);
LRESULT MsgInitMenu(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgExitMenuLoop(HWND hwnd, WPARAM wParam);
LRESULT MsgNotify(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgFileChangeNotify(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgTrayMessage(HWND hwnd, WPARAM wParam, LPARAM lParam);
//~LRESULT MsgKeyDown(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT MsgCommand(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam);
LRESULT MsgSysCommand(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam);
LRESULT MsgUahMenuBar(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam);

//LRESULT MsgNonClientAreaPaint(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam);

// ----------------------------------------------------------------------------

int  DisableDocChangeNotification();
void EnableDocChangeNotification(const int evm);

#define LimitNotifyEvents()     { int _evm_ = 0; __try { _evm_ = DisableDocChangeNotification(); SendMessage(Globals.hwndEdit, WM_SETREDRAW, FALSE, 0);
#define RestoreNotifyEvents()   ;} __finally { EnableDocChangeNotification(_evm_); SendMessage(Globals.hwndEdit, WM_SETREDRAW, TRUE, 0); InvalidateRect(Globals.hwndEdit, NULL, TRUE); } }

// ----------------------------------------------------------------------------

// undo/redo transaction wrapper (selection history handled by Scintilla)
#define UndoTransActionBegin()  { LONG _token_ = 0L; __try { _token_ = BeginUndoActionSelection();
#define EndUndoTransAction()    ;} __finally { EndUndoActionSelection(_token_); } }

// ----------------------------------------------------------------------------

#define BeginWaitCursor(cond, text) {                               \
        if (cond) {                                                 \
            SciCall_SetCursor(SC_CURSORWAIT);                       \
            SciCall_SetVScrollbar(false);                           \
            SciCall_SetHScrollbar(false);                           \
            StatusSetText(Globals.hwndStatus, STATUS_HELP, (text)); \
        }                                                           \
        LimitNotifyEvents()

#define BeginWaitCursorUID(cond, uid) {                              \
        if (cond) {                                                  \
            SciCall_SetCursor(SC_CURSORWAIT);                        \
            SciCall_SetVScrollbar(false);                            \
            SciCall_SetHScrollbar(false);                            \
            StatusSetTextID(Globals.hwndStatus, STATUS_HELP, (uid)); \
        }                                                            \
        LimitNotifyEvents()

#define EndWaitCursor()                                        \
        RestoreNotifyEvents();                                 \
        SciCall_SetCursor(SC_CURSORNORMAL);                    \
        POINT pt; GetCursorPos(&pt);                           \
        SetCursorPos(pt.x, pt.y);                              \
        SciCall_SetHScrollbar(true);                           \
        SciCall_SetVScrollbar(true);                           \
        UpdateStatusbar(true);                                 \
    }

// ----------------------------------------------------------------------------

inline void UserMarkerDeleteAll(const DocLn ln)
{
    //~~~ SciCall_MarkerDelete(line, -1);
    int const bitmask = ALL_MARKERS_BITMASK();
    int       markers = SciCall_MarkerGet(ln);
    while (markers & bitmask) {
        for (int m = 0; m <= MARKER_NP3_BOOKMARK; ++m) {
            if (TEST_BIT(int, m, markers)) {
                SciCall_MarkerDelete(ln, m);
            }
        }
        markers = SciCall_MarkerGet(ln);
    }
}

// ----------------------------------------------------------------------------

#endif //_NP3_NOTEPAD3_H_
///   End of Notepad3.h   ///
