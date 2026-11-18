// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspClient.h                                                                *
*   LSP client for the built-in CE language server.                            *
*                                                                             *
*   Process lifetime, stdio transport (worker threads), JSON-RPC request      *
*   bookkeeping, full-document synchronisation and capability negotiation.     *
*   Nothing in this module touches Scintilla from a worker thread: results    *
*   are posted to the main window and rendered there by CeLspFeatures.c.      *
*                                                                             *
*******************************************************************************/

#pragma once

#include "TypeDefs.h"
#include "CeLspJson.h"

// Posted from the reader thread.  Ownership rules:
//   WM_CELSP_MESSAGE : lParam is a malloc'd UTF-8 JSON-RPC document
//   WM_CELSP_STATUS  : lParam is a malloc'd UTF-8 message (may be NULL)
#define CELSP_STATUS_EXITED 1
#define CELSP_STATUS_ERROR  2
#define CELSP_STATUS_READY  3
#define CELSP_STATUS_TEARDOWN_DONE 4

typedef enum CELSP_REQ_KIND
{
    CELSP_REQ_INIT = 1,
    CELSP_REQ_COMPLETION,
    CELSP_REQ_SIGNATURE,
    CELSP_REQ_HOVER,
    CELSP_REQ_DEFINITION,
    CELSP_REQ_DECLARATION,
    CELSP_REQ_REFERENCES,
    CELSP_REQ_DOCSYMBOLS,
    CELSP_REQ_WSSYMBOLS,
    CELSP_REQ_FORMATTING,
    CELSP_REQ_SEMTOKENS,
    CELSP_REQ_INLAY,
    CELSP_REQ_CODELENS,
    CELSP_REQ_DOCLINKS,
    CELSP_REQ_DOCHIGHLIGHT,
    CELSP_REQ_SHUTDOWN
} CELSP_REQ_KIND;

// Capability bits parsed from the initialize result (one per provider the
// built-in server may advertise).  CeLspClient_HasCap() queries them.
typedef enum CELSP_CAP
{
    CELSP_CAP_DEFINITION = 0,
    CELSP_CAP_DECLARATION,
    CELSP_CAP_REFERENCES,
    CELSP_CAP_DOCSYMBOLS,
    CELSP_CAP_WSSYMBOLS,
    CELSP_CAP_FORMATTING,
    CELSP_CAP_SEMTOKENS,
    CELSP_CAP_INLAY,
    CELSP_CAP_CODELENS,
    CELSP_CAP_DOCLINKS,
    CELSP_CAP_DOCHIGHLIGHT,
    CELSP_CAP_CNT
} CELSP_CAP;

typedef struct CELSP_PENDING
{
    int        id;
    int        kind;
    int        version;
    ULONGLONG  deadline;
    UINT       generation;
    DocPos     wordStart;    // byte offset of the typed prefix
    char       root[128];
} CELSP_PENDING;

bool CeLspClient_Start(HWND hwndMain);
void CeLspClient_Stop(void);
bool CeLspClient_IsReady(void);
bool CeLspClient_IsStarting(void);

// Main-thread handlers for the messages above.
void CeLspClient_OnMessage(WPARAM generation, LPARAM payload);

// Clear the failed-start cooldown after a settings change or an explicit retry.
void CeLspClient_ResetStartFailure(void);
void CeLspClient_OnStatus(WPARAM status, LPARAM detail);

// Debounced didChange flush (IDT_TIMER_CELSP_SYNC).
void CeLspClient_OnSyncTimer(void);

// Expire outstanding requests and send $/cancelRequest (IDT_TIMER_CELSP_TIMEOUT).
void CeLspClient_OnRequestTimeoutTimer(void);

// UI fallback when a teardown worker has not posted CELSP_STATUS_TEARDOWN_DONE.
void CeLspClient_OnTeardownTimer(void);

// Debounced completion request (IDT_TIMER_CELSP_COMPLETION).
void CeLspClient_OnCompletionTimer(void);
void CeLspClient_CancelCompletionDebounce(void);

// Document lifecycle.  All of these run on the UI thread.
void CeLspClient_OnDocumentOpened(void);
void CeLspClient_OnDocumentChanged(void);
void CeLspClient_OnDocumentSaved(void);
void CeLspClient_OnDocumentClosed(void);

// Accumulates an inserted range for incremental didChange.  Deletions,
// undo/redo and UTF-16 sessions fall back to full-document sync.
void CeLspClient_OnDocumentRangeChanged(DocPos position, DocPos length, int linesAdded, const char* text);

// Requests.  triggerKind follows LSP CompletionTriggerKind.
bool CeLspClient_RequestCompletion(const char* root, DocPos wordStart, int triggerKind, int triggerChar);
bool CeLspClient_RequestSignatureHelp(void);
bool CeLspClient_RequestHover(DocPos position);
void CeLspClient_CancelPending(int kind);

// Positional requests at the current caret (definition/declaration/documentSymbol/
// inlayHint/codeLens/documentLink use this; kind is a CELSP_REQ_* value).
bool CeLspClient_RequestPositional(const char* method, int kind);
// textDocument/references with includeDeclaration = true.
bool CeLspClient_RequestReferences(void);
// workspace/symbol with the given query.
bool CeLspClient_RequestWorkspaceSymbols(const char* query);
// textDocument/formatting (default tab size / insertSpaces).
bool CeLspClient_RequestFormatting(void);

// Current capability-negotiated position encoding.
bool CeLspClient_UsesUtf16(void);

// True when the server advertised the given CELSP_CAP_* provider.
bool CeLspClient_HasCap(int cap);

// True for diagnostic severities rendered as squiggles (1=error, 2=warning).
bool CeLspClient_DiagnosticSeverityVisible(int severity);

// LSP (line, character) -> document byte offset using the synced snapshot.
// False when no snapshot exists; the offset is clamped to the document.
bool CeLspClient_PosToOffset(int line, int character, DocPos* out);

// semanticTokens legend (tokenTypes names parsed at initialize).
int         CeLspClient_TokenTypeCount(void);
const char* CeLspClient_TokenType(int idx);
