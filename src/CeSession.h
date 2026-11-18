// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeSession.h                                                                 *
*   Cheat Engine editor sessions: a .cea document opened from CE's            *
*   disassembler right-click menu carries a <file>.cea.np3ce sidecar with    *
*   the session id and target memory record.  Generated template code also   *
*   carries a format request, consumed once after the load.  Saving back to  *
*   the CE address list is always an explicit user action.                   *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_SESSION_H_
#define _CE_SESSION_H_

#include "TypeDefs.h"

// Called after a document has been loaded.  Loads the sidecar if present.
void CeSession_OnOpened(LPCWSTR scriptPath);

// True once when the just opened session was created by CE's right-click
// "自动汇编" templates: their code is generated on the fly, so the caller runs
// the CE formatter on it right away.  Opening an existing address-list record
// never requests this, and the flag is cleared by the first call.
bool CeSession_ConsumeFormatRequest(void);

// Called before switching to another document / creating an empty one.
// Sends CMD_CLOSE_SESSION to CE so it can release the memory record edit.
void CeSession_OnClosed(void);

// Explicit "add to current CE address list" action (toolbar / CE menu).
// Never calls saveTable(); the user decides when the CT file is written.
void CeSession_AddCurrentToAddressList(HWND hwnd);

// Called after a successful save of a CE session document.  Pushes the saved
// buffer back into the existing CE address-list record (CMD_SYNC_SCRIPT) so
// re-opening from CE shows the new text and the record's beginEdit lock is
// released.  No-op for non-session documents or scripts with no record yet.
void CeSession_SyncSavedScript(HWND hwnd);

// Main-thread handler for WM_APP_CE_ADD_RESULT: report the worker's result
// (stored as an atomic code, so the worker never touches the UI).
void CeSession_ReportResult(HWND hwnd);

void CeSession_Destroy(void);
bool CeSession_IsActive(void);

#endif // _CE_SESSION_H_
