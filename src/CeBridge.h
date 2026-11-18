// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeBridge.h                                                                  *
*   Named-pipe client for the Cheat Engine Lua bridge (np3_ce_bridge.lua).    *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_BRIDGE_H_
#define _CE_BRIDGE_H_

#include "TypeDefs.h"

// Request commands, must match ce_bridge/np3_ce_bridge.lua.
#define CE_BRIDGE_CMD_PING             0
#define CE_BRIDGE_CMD_RUN              1
#define CE_BRIDGE_CMD_CLOSE            2
#define CE_BRIDGE_CMD_APPLY_SCRIPT     3  // sessionId|memrecId\n<script> -> CE address list
#define CE_BRIDGE_CMD_CLOSE_SESSION    4  // sessionId -> release memrec edit
#define CE_BRIDGE_CMD_CHECK_SCRIPT     5  // <script> -> CE autoAssembleCheck findings
#define CE_BRIDGE_CMD_SYNC_SCRIPT      6  // sessionId|memrecId\n<script> -> update saved script in CE record
#define CE_BRIDGE_MAX_FRAME            (8 * 1024 * 1024)

// Send one request and return the RESULT/ERROR text (owned by the caller,
// release with FreeMem).  Response reads are bounded: if CE's main thread is
// busy, this returns false instead of blocking forever.  Call from a worker
// thread for anything on the UI path.
bool CeBridge_SendRequest(BYTE cmd, const char* payload, DWORD len,
                          char** replyOut, DWORD* replyLenOut);

// Same as CeBridge_SendRequest with caller-supplied timeouts.  Used while
// shutting down: the fire-and-forget variant runs on a detached thread that
// the exiting process can kill before the frame is written, so the frame has
// to be sent synchronously - with short bounds, because the process is about
// to exit.
bool CeBridge_SendRequestTimed(BYTE cmd, const char* payload, DWORD len,
                                char** replyOut, DWORD* replyLenOut,
                                DWORD pipeWaitMs, DWORD replyTimeoutMs);

// Fire-and-forget request (session close).  Never waits for a reply, so
// switching documents or exiting Notepad3 cannot hang on CE.
void CeBridge_SendNoReply(BYTE cmd, const char* payload, DWORD len);

// Online syntax check: send the current script to CE's own autoAssembleCheck
// on a worker thread; CeBridge_ReportCheck merges the findings into CeLint as
// external diagnostics.  Both run on the UI thread (deferred by WM_APP_*).
// CheckScriptOnline returns false when no check could be started (busy, empty
// or too large document), so the caller may fall back to the local report.
bool CeBridge_CheckScriptOnline(HWND hwnd);
void CeBridge_ReportCheck(HWND hwnd);

// Merged "Code Review" command: local analyzer plus, when the bridge is
// connected, CE's own autoAssembleCheck.  The findings are shown in one report.
void CeBridge_ReviewScript(HWND hwnd);

// Response kinds, must match the values in ce_bridge/np3_ce_bridge.lua.
typedef enum _CE_BRIDGE_KIND {
    CE_BRIDGE_OUTPUT = 0,   // print() output
    CE_BRIDGE_RESULT,       // return value of the chunk
    CE_BRIDGE_ERROR,        // load/runtime error
    CE_BRIDGE_DONE,         // end of one run
    CE_BRIDGE_INFO          // connection banner / status
} CE_BRIDGE_KIND;

// How a run request is routed.  AUTO inspects the document: pure Lua runs in
// the local Lua runtime, CE-related scripts go through the named-pipe bridge.
// Only the latter ever deploys/connects the CE link.
typedef enum _CE_RUN_MODE {
    CE_RUN_AUTO = 0,
    CE_RUN_LOCAL,
    CE_RUN_CE
} CE_RUN_MODE;

// Discover a Cheat Engine installation by its directory layout, deploy the
// bridge script into <CE>\autorun, and remember the path in [Settings2].
bool CeBridge_EnsureDeployed(HWND hwnd, bool interactive);

// Locate a CE installation without deploying anything (used to find CE's Lua DLL).
bool CeBridge_GetCeDir(WCHAR out[MAX_PATH]);

// Open <CE>\autorun\np3_user_templates.tpl for editing (creating the default
// sample first when the file is missing).
void CeBridge_EditUserTemplates(HWND hwnd);

// Run the current document.  The Ex variant forces local / CE execution.
bool CeBridge_RunCurrentScript(HWND hwnd);
bool CeBridge_RunCurrentScriptEx(HWND hwnd, CE_RUN_MODE mode);

// Stop the AA value watch and, if the script is still enabled, run its
// [DISABLE] section (cancel the script).  Safe to call when nothing is active.
void CeBridge_CancelRun(HWND hwnd);

// True while an AA script executed from Notepad3 is enabled in Cheat Engine.
bool CeBridge_IsAaRunning(void);

// Called while the main window is closing: synchronously (bounded wait) stop
// the AA watch and run the [DISABLE] section, so the target process gets its
// original code back before Notepad3 exits.  No-op when no AA script is active.
void CeBridge_ShutdownAa(HWND hwnd);

// Called before the current document is replaced by New/Open.  Only the
// document that enabled an AA script is rolled back; switching to another
// document leaves the script enabled in Cheat Engine.
void CeBridge_OnDocumentClosed(HWND hwnd);

// True when |line| is a np3watch/np3unwatch/np3nowatch directive comment.
bool CeBridge_IsWatchDirectiveLine(const char* line, size_t len);

// Remove all watch directive comment lines in the selection (whole document
// when there is no selection) as a single undoable action.
void CeBridge_ClearWatchLines(void);

// Post one UTF-8 line from a worker thread to the main-thread output pane.
// PostLine transfers ownership of |textUtf8|; PostConstLine copies it.
void CeBridge_PostLine(HWND hwnd, BYTE kind, char* textUtf8);
void CeBridge_PostConstLine(HWND hwnd, BYTE kind, const char* textUtf8);

// Ask the main thread to show a modal hint (e.g. a missing Lua runtime).
void CeBridge_PostHint(HWND hwnd, const char* textUtf8);

// Synchronize the CE menu: label every entry from the language resources,
// create the popup when the MUI language DLL has none, and drop the merged
// commands (separate "run locally"/"run in CE", the old separate check rows).
void CeBridge_EnsureRunCommands(HMENU hmenu);

// Synchronize the Help menu: keep the caret-aware "Query Function Doc" entry,
// drop the Notepad3-only rows and relabel the command-line help as shortcut
// help.  Runs at startup and after every UI-language switch.
void CeBridge_EnsureLocalDocCommand(HMENU hmenu);

// Open the full keyword/function reference (本地文档.txt).  The file is embedded
// as the IDR_CE_LOCALDOC resource and extracted to the program directory when
// it is missing, so it is always available.
void CeBridge_ShowLocalDoc(HWND hwnd);

// Same, but jump to the entry for the given keyword (UTF-8).  Used by F1 over
// a completion item.  Opens at the top when the keyword has no entry.
void CeBridge_ShowLocalDocAtKeyword(HWND hwnd, const char* keyword);

// Menu command: look up the identifier under the caret in the local reference.
void CeBridge_QueryLocalDoc(HWND hwnd);

// Main-thread handlers for WM_CEBRIDGE_OUTPUT / WM_CEBRIDGE_DONE /
// WM_CEBRIDGE_HINT.  WM_CEBRIDGE_OUTPUT and WM_CEBRIDGE_HINT transfer
// ownership of the UTF-8 buffer in lParam.
void CeBridge_OnOutput(WPARAM kind, LPARAM textUtf8);
void CeBridge_OnDone(WPARAM status);
void CeBridge_OnHint(const char* textUtf8);

void CeBridge_Destroy(void);

#endif // _CE_BRIDGE_H_
