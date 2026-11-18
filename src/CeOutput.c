// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeOutput.c                                                                  *
*   Cheat Engine output is merged into the docked output/log panel            *
*   (CePanel.c): one window shows editor logs and CE output, so no separate    *
*   top-level window is created anymore.  These wrappers keep the existing     *
*   call sites unchanged.                                                     *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <string.h>
#include <strsafe.h>
#include <wchar.h>

#include "TypeDefs.h"
#include "Notepad3.h"
#include "CeBridge.h"
#include "CeOutput.h"
#include "CePanel.h"

void CeOutput_Show(HWND hwndParent)
{
    CePanel_Show(IsWindow(hwndParent) ? hwndParent : Globals.hwndMain);
}

void CeOutput_Append(HWND hwndParent, int kind, LPCWSTR text)
{
    UNREFERENCED_PARAMETER(hwndParent);
    if (!text) {
        return;
    }

    int level = 0;                 // 0 plain, 1 error, 2 warning, 3 info, 4 debug
    LPCWSTR prefix = L"[OUT] ";
    switch (kind) {
    case CE_BRIDGE_ERROR:
        level = 1;
        prefix = L"[ERROR] ";
        break;
    case CE_BRIDGE_RESULT:
        level = 0;
        prefix = L"[RESULT] ";
        break;
    case CE_BRIDGE_INFO:
        level = 4;
        prefix = L"[INFO] ";
        break;
    case CE_BRIDGE_DONE:
        level = 0;
        prefix = L"[DONE] ";
        break;
    default:
        break;
    }

    size_t const prefixLen = wcslen(prefix);
    size_t const textLen = wcslen(text);
    WCHAR* const buf = (WCHAR*)AllocMem((prefixLen + textLen + 1) * sizeof(WCHAR),
                                        HEAP_ZERO_MEMORY);
    if (!buf) {
        return;
    }
    memcpy(buf, prefix, prefixLen * sizeof(WCHAR));
    memcpy(buf + prefixLen, text, (textLen + 1) * sizeof(WCHAR));
    CePanel_LogText(level, buf);
    FreeMem(buf);

    // The log window stays hidden while nothing is printed: it only opens
    // itself for real script output (not for our own status lines).
    switch (kind) {
    case CE_BRIDGE_OUTPUT:
    case CE_BRIDGE_RESULT:
    case CE_BRIDGE_ERROR:
        CePanel_AutoShow();
        break;
    default:
        break;
    }
}

void CeOutput_Destroy(void)
{
    // Nothing to tear down: the log panel is owned by the main window.
}
