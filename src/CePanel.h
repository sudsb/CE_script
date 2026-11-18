// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CePanel.h                                                                   *
*   CE output / log panel: a coloured, read-only log viewer plus a small       *
*   terminal.  It is the single output window of the CE editor: bridge output  *
*   is appended here as well.  The panel either docks at the bottom of the     *
*   main frame or floats as an independent top-level window.  Font size is     *
*   adjustable (A-/A+) and persisted in Settings2.CeOutputFontSize, the docked *
*   height can be dragged and is persisted in Settings2.CePanelHeight.        *
*                                                                             *
******************************************************************************/

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void  CePanel_Create(HWND hwndMain);
void  CePanel_Layout(HWND hwndMain, int x, int y, int cx, int cy);
bool  CePanel_IsVisible(void);
void  CePanel_Show(HWND hwndMain);
void  CePanel_AutoShow(void);
void  CePanel_Toggle(void);
int   CePanel_Height(void);
bool  CePanel_IsFloating(void);
void  CePanel_ToggleFloat(void);
void  CePanel_RefreshTheme(void);

// level: 0 plain, 1 error, 2 warning, 3 info, 4 debug
void  CePanel_Log(int level, LPCWSTR msg);
// Same, but embedded CR/LF are kept as separate (equally coloured) lines.
void  CePanel_LogText(int level, LPCWSTR text);
void  CePanel_AppendConsole(LPCWSTR text);
void  CePanel_Clear(void);

#ifdef __cplusplus
}
#endif
