// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspClient.c                                                                *
*   LSP client: built-in server process, stdio transport, document sync.       *
*   See CeLspClient.h.                                                         *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <limits.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Notepad3.h"
#include "resource.h"
#include "SciCall.h"
#include "PathLib.h"
#include "CeLspJson.h"
#include "CeLspFraming.h"
#include "CeLspPos.h"
#include "CeLspFeatures.h"
#include "CeLspClient.h"

#define CELSP_MAX_PENDING   32
#define CELSP_MAX_RESTARTS  3
// Teardown runs on the UI thread.  The built-in server exits on its own when
// stdin closes, so a short grace period plus TerminateProcess keeps a crashed
// server from freezing the editor for seconds.
#define CELSP_STOP_TIMEOUT  300
#define CELSP_THREAD_TIMEOUT 500
#define CELSP_RETRY_COOLDOWN_MS 2000u
#define CELSP_RETRY_MAX_BACKOFF_MS 30000u
#define CELSP_REQUEST_TIMEOUT_MS 8000u
#define CELSP_FAST_REQUEST_TIMEOUT_MS 3000u
#define CELSP_REQUEST_TIMEOUT_TIMER_MS 1000u
#define CELSP_TEARDOWN_TIMER_MS 3000u
#define CELSP_MAX_CHANGE_RANGES 256
#define CELSP_COMPLETION_DEBOUNCE_MS 80u

// Pure session-policy helpers (also exercised by TestCeLsp.ps1).
static bool _Celsp_ShouldRetryStart(ULONGLONG now, ULONGLONG failedAt, int restarts)
{
    if (failedAt == 0u) {
        return true;
    }
    if (now < failedAt) {
        return false;
    }
    int const step = (restarts < 0) ? 0 : ((restarts > 5) ? 5 : restarts);
    ULONGLONG delay = (ULONGLONG)CELSP_RETRY_COOLDOWN_MS << step;
    if (delay > CELSP_RETRY_MAX_BACKOFF_MS) {
        delay = CELSP_RETRY_MAX_BACKOFF_MS;
    }
    return (now - failedAt) >= delay;
}

static bool _Celsp_DiagIsCurrent(const char* uri, int version, UINT generation,
                                 const char* currentUri, int currentVersion, UINT currentGeneration)
{
    return (uri != NULL) && (currentUri != NULL) && (generation == currentGeneration) &&
           (version >= currentVersion) && (strcmp(uri, currentUri) == 0);
}

static bool _Celsp_MessageIsCurrent(UINT msgGeneration, UINT currentGeneration)
{
    return msgGeneration == currentGeneration;
}

static bool _Celsp_RequestExpired(ULONGLONG now, ULONGLONG deadline)
{
    return now > deadline;
}

static int _Celsp_PickPendingDrop(int count, const int* kinds, const ULONGLONG* deadlines, int newKind)
{
    if ((count <= 0) || !kinds || !deadlines) {
        return -1;
    }
    int best = 0;
    bool sameKind = false;
    for (int i = 0; i < count; ++i) {
        if (kinds[i] == newKind) {
            if (!sameKind || (deadlines[i] < deadlines[best])) {
                best = i;
            }
            sameKind = true;
        }
    }
    if (!sameKind) {
        best = 0;
        for (int i = 1; i < count; ++i) {
            if (deadlines[i] < deadlines[best]) {
                best = i;
            }
        }
    }
    return best;
}

static bool _Celsp_MergeChangeRange(int* start, int* end, int newStart, int newEnd, int docLen)
{
    if (!start || !end || (newEnd < newStart)) {
        return false;
    }
    if (docLen > 0) {
        if (newStart < 0) { newStart = 0; }
        if (newEnd > docLen) { newEnd = docLen; }
    }
    if ((*start == *end) && (*start <= 0)) {
        *start = newStart;
        *end = newEnd;
        return true;
    }
    if ((newStart > (*end + 1)) || (newEnd < *start)) {
        return false;
    }
    if (newStart < *start) { *start = newStart; }
    if (newEnd > *end) { *end = newEnd; }
    return true;
}

static bool _Celsp_DiagSeverityForSquiggle(int severity)
{
    return (severity == 1) || (severity == 2);
}

static bool _Celsp_AllowIncremental(bool enabled, bool utf16, bool fullSync,
                                    int rangeCount, bool hasSnapshot)
{
    return enabled && !utf16 && !fullSync && (rangeCount == 0) && hasSnapshot;
}

static void _Celsp_InsertionPosition(const char* text, size_t length, size_t offset,
                                     int* line, int* character)
{
    if (!line || !character) {
        return;
    }
    if (!text || (offset > length)) {
        *line = 0;
        *character = 0;
        return;
    }
    CeLspPos_FromOffset(text, length, offset, false, line, character);
}

// ----------------------------------------------------------------------------
// State (all shared state is guarded by s_cs; worker threads only read the
// pipe and post messages back to the main window)
// ----------------------------------------------------------------------------

typedef struct CELSP_OUT
{
    struct CELSP_OUT* next;
    char*  bytes;
    size_t len;
} CELSP_OUT;

typedef struct CELSP_RANGE_CHANGE
{
    int   startLine;
    int   startChar;
    int   endLine;
    int   endChar;
    char* text;
} CELSP_RANGE_CHANGE;

typedef struct CELSP_TEARDOWN_STATE
{
    HANDLE     hProcess;
    HANDLE     hThreadRead;
    HANDLE     hThreadWrite;
    HANDLE     hIn;
    HANDLE     hOut;
    HANDLE     hStderrR;
    HANDLE     hThreadStderr;
    HANDLE     hEvent;
    HANDLE     hStop;
    CELSP_OUT* queue;
    UINT       generation;
    bool       final;
} CELSP_TEARDOWN_STATE;

static CRITICAL_SECTION s_cs;
static bool             s_csInit = false;
static volatile LONG    s_running = 0;
static volatile LONG    s_ready = 0;
static volatile LONG    s_stopping = 0;
static volatile LONG    s_starting = 0;
// Timestamp of the last failed start (GetTickCount64); 0 means no failure.
static ULONGLONG        s_startFailedAt = 0u;
// Incremented on every session boundary so queued messages from an old
// server generation cannot decorate a newer document.
static volatile LONG    s_generation = 0;

static HWND   s_hwndMain = NULL;
static HANDLE s_hStdinW = NULL;
static HANDLE s_hStdoutR = NULL;
static HANDLE s_hStderrR = NULL;
static HANDLE s_hProcess = NULL;
static HANDLE s_hThreadRead = NULL;
static HANDLE s_hThreadWrite = NULL;
static HANDLE s_hThreadStderr = NULL;
static HANDLE s_hEvent = NULL;
static HANDLE s_hStopEvent = NULL;

static CELSP_OUT* s_head = NULL;
static CELSP_OUT* s_tail = NULL;

static CELSP_PENDING s_pending[CELSP_MAX_PENDING];
static int           s_pendingCount = 0;
static int           s_nextId = 1;
static int           s_restarts = 0;

static CELSP_RANGE_CHANGE s_ranges[CELSP_MAX_CHANGE_RANGES];
static int    s_rangeCount = 0;
static bool   s_fullSync = true;
static char   s_debouncedRoot[128];
static DocPos s_debouncedWordStart = 0;
static int    s_debouncedTriggerKind = 1;
static int    s_debouncedTriggerChar = 0;
static bool   s_completionArmed = false;

static char*  s_text = NULL;
static size_t s_textLen = 0u;
static size_t s_textCap = 0u;   // reused across keystrokes
static char*  s_uri = NULL;
static int    s_version = 0;
static bool   s_docOpen = false;
static bool   s_dirty = false;
static bool   s_utf16 = false;

// Provider capabilities parsed from the initialize result (CELSP_CAP_* bits)
// and the semanticTokens legend (token type names, index = tokenType value).
static unsigned s_caps = 0u;
static char     s_tokenTypes[32][32];
static int      s_tokenTypeCount = 0;

// _Snapshot() runs on the very first CE edit, long before the server process
// is started.  EnterCriticalSection() on the zero-initialised s_cs takes the
// contended path and dereferences a NULL LockSemaphore inside ntdll, so the
// section must be initialised lazily on first use.
static void _Lock(void)
{
    if (!s_csInit) {
        // Every caller before the worker threads exist is the UI thread.
        InitializeCriticalSection(&s_cs);
        s_csInit = true;
    }
    EnterCriticalSection(&s_cs);
}
static void _Unlock(void) { LeaveCriticalSection(&s_cs); }

static char* _StrDup(const char* text)
{
    if (!text) {
        return NULL;
    }
    const size_t n = strlen(text);
    char* const out = (char*)malloc(n + 1u);
    if (out) {
        memcpy(out, text, n);
        out[n] = '\0';
    }
    return out;
}

// ----------------------------------------------------------------------------
// Document snapshot (UI thread only)
// ----------------------------------------------------------------------------

// Copies the document into a reusable buffer.  This runs on every keystroke,
// so it must not malloc/free a fresh multi-megabyte block each time.
static size_t _ReadDocText(char** pBuf, size_t* pCap)
{
    const DocPos len = SciCall_GetLength();
    if (len <= 0) {
        return 0u;
    }
    const char* const src = SciCall_GetCharacterPointer();
    if (!src) {
        return 0u;
    }
    const size_t need = (size_t)len + 1u;
    if (need > *pCap) {
        size_t newCap = (*pCap == 0u) ? 4096u : *pCap;
        while (newCap < need) {
            newCap *= 2u;
        }
        char* const grown = (char*)realloc(*pBuf, newCap);
        if (!grown) {
            return 0u;
        }
        *pBuf = grown;
        *pCap = newCap;
    }
    memcpy(*pBuf, src, (size_t)len);
    (*pBuf)[len] = '\0';
    return (size_t)len;
}

static char* _GetDocUri(void)
{
    LPCWSTR const wpath = Path_Get(Paths.CurrentFile);
    if (wpath && wpath[0]) {
        // Long paths are supported by HPATHL, so size the UTF-8 copy from the
        // conversion instead of truncating into a fixed MAX_PATH buffer.
        const int need = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, NULL, 0, NULL, NULL);
        if (need > 0) {
            char* const utf8 = (char*)malloc((size_t)need);
            if (utf8) {
                char* uri = NULL;
                if (WideCharToMultiByte(CP_UTF8, 0, wpath, -1, utf8, need, NULL, NULL) > 0) {
                    uri = CeLspPos_UriFromPath(utf8);
                }
                free(utf8);
                if (uri) {
                    return uri;
                }
            }
        }
    }
    return _StrDup("file:///untitled.cea");
}

static void _Snapshot(void)
{
    char* const uri = _GetDocUri();
    _Lock();
    s_textLen = _ReadDocText(&s_text, &s_textCap);
    const bool uriChanged = (s_uri && uri && (strcmp(s_uri, uri) != 0));
    free(s_uri);
    s_uri = uri;
    ++s_version;
    s_dirty = true;
    if (uriChanged) {
        // A different document: the next flush re-opens it on the server.
        // The built-in server keeps one document at a time, so didOpen is the
        // whole workspace hand-over.
        s_docOpen = false;
    }
    _Unlock();
}

static void _Position(CELSP_JSON* params, DocPos position)
{
    int line = 0;
    int character = 0;
    _Lock();
    CeLspPos_FromOffset(s_text, s_textLen, (size_t)position, s_utf16, &line, &character);
    CELSP_JSON* const pos = CeLspJson_NewObject();
    char* const uri = s_uri ? _StrDup(s_uri) : _StrDup("file:///untitled.cea");
    _Unlock();
    if (pos) {
        CeLspJson_Set(pos, "line", CeLspJson_NewNumber((double)line));
        CeLspJson_Set(pos, "character", CeLspJson_NewNumber((double)character));
        CeLspJson_Set(params, "position", pos);
    }
    CELSP_JSON* const doc = CeLspJson_NewObject();
    if (doc) {
        CeLspJson_Set(doc, "uri", CeLspJson_NewString(uri ? uri : "file:///untitled.cea"));
        CeLspJson_Set(params, "textDocument", doc);
    }
    free(uri);
}

// ----------------------------------------------------------------------------
// Output queue / transport
// ----------------------------------------------------------------------------

static void _FreeQueueNodes(CELSP_OUT* node)
{
    while (node) {
        CELSP_OUT* const next = node->next;
        free(node->bytes);
        free(node);
        node = next;
    }
}

static void _FreeQueue(void)
{
    _Lock();
    CELSP_OUT* const node = s_head;
    s_head = NULL;
    s_tail = NULL;
    _Unlock();
    _FreeQueueNodes(node);
}

static CELSP_OUT* _DetachQueue(void)
{
    _Lock();
    CELSP_OUT* const node = s_head;
    s_head = NULL;
    s_tail = NULL;
    _Unlock();
    return node;
}

static bool _Enqueue(char* frame, size_t len)
{
    if (!frame) {
        return false;
    }
    CELSP_OUT* const node = (CELSP_OUT*)calloc(1u, sizeof(CELSP_OUT));
    if (!node) {
        free(frame);
        return false;
    }
    node->bytes = frame;
    node->len = len;
    _Lock();
    if (s_tail) {
        s_tail->next = node;
    }
    else {
        s_head = node;
    }
    s_tail = node;
    _Unlock();
    if (s_hEvent) {
        SetEvent(s_hEvent);
    }
    return true;
}

static bool _WriteAll(HANDLE h, const char* data, size_t len)
{
    size_t done = 0u;
    while (done < len) {
        DWORD written = 0u;
        const DWORD chunk = (len - done > 0xFFFFu) ? 0xFFFFu : (DWORD)(len - done);
        if (!WriteFile(h, data + done, chunk, &written, NULL) || (written == 0u)) {
            return false;
        }
        done += (size_t)written;
    }
    return true;
}

static void _WriterDrain(HANDLE hIn)
{
    for (;;) {
        CELSP_OUT* node = NULL;
        _Lock();
        if (s_head) {
            node = s_head;
            s_head = node->next;
            if (!s_head) {
                s_tail = NULL;
            }
        }
        _Unlock();
        if (!node) {
            break;
        }
        if (hIn) {
            _WriteAll(hIn, node->bytes, node->len);
        }
        free(node->bytes);
        free(node);
    }
}

static unsigned __stdcall _WriterThread(void* arg)
{
    const UINT generation = (UINT)(uintptr_t)arg;
    // Snapshot the handles: _Teardown() clears the globals and leaves the
    // handles owned by this thread when the wait times out.
    HANDLE const hEvent = s_hEvent;
    HANDLE const hStop = s_hStopEvent;
    HANDLE const hIn = s_hStdinW;
    for (;;) {
        if (hEvent) {
            WaitForSingleObject(hEvent, 50u);
        }
        if ((hStop && (WaitForSingleObject(hStop, 0u) == WAIT_OBJECT_0)) ||
            (generation != (UINT)InterlockedCompareExchange(&s_generation, 0, 0))) {
            break;
        }
        _WriterDrain(hIn);
    }
    if (hIn) {
        CloseHandle(hIn);
    }
    return 0u;
}

static bool _IsCurrentGeneration(UINT generation)
{
    return generation == (UINT)InterlockedCompareExchange(&s_generation, 0, 0);
}

static void _PostPayload(char* payload, UINT generation)
{
    if (!payload) {
        return;
    }
    if (s_hwndMain && IsWindow(s_hwndMain)) {
        if (!PostMessageW(s_hwndMain, WM_CELSP_MESSAGE,
                          (WPARAM)generation, (LPARAM)payload)) {
            free(payload);
        }
    }
    else {
        free(payload);
    }
}

static void _PostStatus(WPARAM status, const char* message)
{
    char* const payload = message ? _StrDup(message) : NULL;
    if (s_hwndMain && IsWindow(s_hwndMain)) {
        if (!PostMessageW(s_hwndMain, WM_CELSP_STATUS, status, (LPARAM)payload)) {
            free(payload);
        }
    }
    else {
        free(payload);
    }
}

static unsigned __stdcall _StderrThread(void* arg)
{
    const UINT generation = (UINT)(uintptr_t)arg;
    HANDLE const hErr = s_hStderrR;
    HANDLE const hStop = s_hStopEvent;
    char buf[2048];
    for (;;) {
        DWORD n = 0u;
        if (!hErr || !ReadFile(hErr, buf, (DWORD)(sizeof(buf) - 1u), &n, NULL) || (n == 0u)) {
            break;
        }
        buf[n] = '\0';
        if (_IsCurrentGeneration(generation)) {
            _PostStatus(CELSP_STATUS_ERROR, buf);
        }
        if (hStop && (WaitForSingleObject(hStop, 0u) == WAIT_OBJECT_0)) {
            break;
        }
    }
    if (hErr) {
        CloseHandle(hErr);
    }
    return 0u;
}

static unsigned __stdcall _ReaderThread(void* arg)
{
    UINT const generation = (UINT)(uintptr_t)arg;
    char*  buf = NULL;
    size_t len = 0u;
    size_t cap = 0u;
    char   chunk[8192];
    HANDLE const hOut = s_hStdoutR;   // owned by this thread once started

    while (s_running) {
        DWORD got = 0u;
        if (!ReadFile(hOut, chunk, (DWORD)sizeof(chunk), &got, NULL) || (got == 0u)) {
            break;
        }
        if (len + (size_t)got + 1u > cap) {
            const size_t wanted = len + (size_t)got + 1u;
            size_t newCap = (cap == 0u) ? 16384u : cap;
            while (newCap < wanted) {
                newCap *= 2u;
            }
            char* const grown = (char*)realloc(buf, newCap);
            if (!grown) {
                break;
            }
            buf = grown;
            cap = newCap;
        }
        memcpy(buf + len, chunk, (size_t)got);
        len += (size_t)got;
        buf[len] = '\0';

        for (;;) {
            const int hdr = CeLspFraming_HeaderEnd(buf, len);
            if (hdr < 0) {
                break;
            }
            size_t contentLen = 0u;
            if (!CeLspFraming_ContentLength(buf, (size_t)hdr, &contentLen)) {
                if (_IsCurrentGeneration(generation)) {
                    _PostStatus(CELSP_STATUS_ERROR, "Malformed LSP frame header");
                }
                len = 0u;
                break;
            }
            const size_t frameLen = (size_t)hdr + contentLen;
            if (len < frameLen) {
                break;
            }
            char* const payload = (char*)malloc(contentLen + 1u);
            if (payload) {
                memcpy(payload, buf + hdr, contentLen);
                payload[contentLen] = '\0';
                _PostPayload(payload, generation);
            }
            const size_t rest = len - frameLen;
            if (rest > 0u) {
                memmove(buf, buf + frameLen, rest);
            }
            len = rest;
            buf[len] = '\0';
        }
    }
    free(buf);
    if (hOut) {
        CloseHandle(hOut);
    }
    if (!s_stopping && _IsCurrentGeneration(generation)) {
        _PostStatus(CELSP_STATUS_EXITED, NULL);
    }
    return 0u;
}

// ----------------------------------------------------------------------------
// Message construction (UI thread only)
// ----------------------------------------------------------------------------

static void _ClearRanges(void)
{
    for (int i = 0; i < s_rangeCount; ++i) {
        free(s_ranges[i].text);
        s_ranges[i].text = NULL;
    }
    s_rangeCount = 0;
    s_fullSync = false;
}

void CeLspClient_OnDocumentRangeChanged(DocPos position, DocPos length,
                                        int linesAdded, const char* text)
{
    if (!_Celsp_AllowIncremental(CeLspFeatures_IsEnabled(), s_utf16, s_fullSync,
                                 s_rangeCount, (s_text != NULL) && (s_textLen > 0u)) ||
        (length <= 0) || !text || !*text || (linesAdded < 0) ||
        (position < 0) || ((size_t)position > s_textLen)) {
        // Multiple insertions in one debounce window would need re-basing onto
        // the snapshot; falling back to full sync keeps server text exact.
        _ClearRanges();
        s_fullSync = true;
        return;
    }
    CELSP_RANGE_CHANGE* const r = &s_ranges[s_rangeCount++];
    _Celsp_InsertionPosition(s_text, s_textLen, (size_t)position,
                             &r->startLine, &r->startChar);
    r->endLine = r->startLine;   // LSP insertion: empty range in the old document
    r->endChar = r->startChar;
    r->text = _StrDup(text);
    if (!r->text) {
        _ClearRanges();
        s_fullSync = true;
    }
}

static void _FlushChanges(void)
{
    char* text = NULL;
    char* uri = NULL;
    int version = 0;
    bool open = false;
    _Lock();
    if (!s_dirty) {
        _Unlock();
        return;
    }
    text = _StrDup(s_text ? s_text : "");
    uri = _StrDup(s_uri ? s_uri : "file:///untitled.cea");
    version = s_version;
    open = s_docOpen;
    s_dirty = false;
    s_docOpen = true;
    _Unlock();

    CELSP_JSON* params = NULL;
    if (open) {
        bool useRanges = !s_fullSync && !s_utf16 && (s_rangeCount > 0);
        params = CeLspJson_NewObject();
        CELSP_JSON* const doc = CeLspJson_NewObject();
        CELSP_JSON* changes = CeLspJson_NewArray();
        if (useRanges) {
            for (int i = 0; i < s_rangeCount; ++i) {
                const CELSP_RANGE_CHANGE* const r = &s_ranges[i];
                CELSP_JSON* const start = CeLspJson_NewObject();
                CELSP_JSON* const end = CeLspJson_NewObject();
                CELSP_JSON* const range = CeLspJson_NewObject();
                CELSP_JSON* const change = CeLspJson_NewObject();
                if (start) {
                    CeLspJson_Set(start, "line", CeLspJson_NewNumber(r->startLine));
                    CeLspJson_Set(start, "character", CeLspJson_NewNumber(r->startChar));
                }
                if (end) {
                    CeLspJson_Set(end, "line", CeLspJson_NewNumber(r->endLine));
                    CeLspJson_Set(end, "character", CeLspJson_NewNumber(r->endChar));
                }
                if (range) {
                    CeLspJson_Set(range, "start", start);
                    CeLspJson_Set(range, "end", end);
                }
                else {
                    CeLspJson_Free(start); CeLspJson_Free(end);
                }
                if (change) {
                    CeLspJson_Set(change, "range", range);
                    CeLspJson_Set(change, "text", CeLspJson_NewString(r->text ? r->text : ""));
                    CeLspJson_Add(changes, change);
                }
                else {
                    CeLspJson_Free(range);
                }
            }
            if (!changes || (CeLspJson_Count(changes) != (size_t)s_rangeCount)) {
                useRanges = false;
                CeLspJson_Free(changes);
                changes = CeLspJson_NewArray();
            }
        }
        if (!useRanges) {
            CELSP_JSON* const change = CeLspJson_NewObject();
            if (change) {
                CeLspJson_Set(change, "text", CeLspJson_NewString(text ? text : ""));
                CeLspJson_Add(changes, change);
            }
        }
        if (params && doc && changes) {
            CeLspJson_Set(doc, "uri", CeLspJson_NewString(uri));
            CeLspJson_Set(doc, "version", CeLspJson_NewNumber((double)version));
            CeLspJson_Set(params, "textDocument", doc);
            CeLspJson_Set(params, "contentChanges", changes);
        }
        else {
            CeLspJson_Free(params); CeLspJson_Free(doc); CeLspJson_Free(changes);
            params = NULL;
        }
        size_t frameLen = 0u;
        char* const frame = CeLspFraming_EncodeNotification("textDocument/didChange", params, &frameLen);
        _Enqueue(frame, frameLen);
        CeLspJson_Free(params);
    }
    else {
        params = CeLspJson_NewObject();
        CELSP_JSON* const doc = CeLspJson_NewObject();
        if (params && doc) {
            CeLspJson_Set(doc, "uri", CeLspJson_NewString(uri));
            CeLspJson_Set(doc, "languageId", CeLspJson_NewString("ce"));
            CeLspJson_Set(doc, "version", CeLspJson_NewNumber((double)version));
            CeLspJson_Set(doc, "text", CeLspJson_NewString(text ? text : ""));
            CeLspJson_Set(params, "textDocument", doc);
        }
        else {
            CeLspJson_Free(params); CeLspJson_Free(doc);
            params = NULL;
        }
        size_t frameLen = 0u;
        char* const frame = CeLspFraming_EncodeNotification("textDocument/didOpen", params, &frameLen);
        _Enqueue(frame, frameLen);
        CeLspJson_Free(params);
    }
    _ClearRanges();
    free(text);
    free(uri);
    if (s_hwndMain) {
        KillTimer(s_hwndMain, IDT_TIMER_CELSP_SYNC);
    }
}

static ULONGLONG _RequestTimeoutMs(int kind)
{
    switch (kind) {
    case CELSP_REQ_COMPLETION:
    case CELSP_REQ_SIGNATURE:
    case CELSP_REQ_HOVER:
        return CELSP_FAST_REQUEST_TIMEOUT_MS;
    default:
        return CELSP_REQUEST_TIMEOUT_MS;
    }
}

static void _PostCancelRequest(int id)
{
    CELSP_JSON* const params = CeLspJson_NewObject();
    if (params) {
        CeLspJson_Set(params, "id", CeLspJson_NewNumber((double)id));
    }
    size_t frameLen = 0u;
    char* const frame = CeLspFraming_EncodeNotification("$/cancelRequest", params, &frameLen);
    _Enqueue(frame, frameLen);
    CeLspJson_Free(params);
}

static bool _AddPending(int id, int kind, DocPos wordStart, const char* root,
                        ULONGLONG deadline, UINT generation)
{
    _Lock();
    if (s_pendingCount > 0) {
        int kinds[CELSP_MAX_PENDING];
        ULONGLONG deadlines[CELSP_MAX_PENDING];
        for (int i = 0; i < s_pendingCount; ++i) {
            kinds[i] = s_pending[i].kind;
            deadlines[i] = s_pending[i].deadline;
        }
        const int drop = _Celsp_PickPendingDrop(s_pendingCount, kinds, deadlines, kind);
        const bool sameKind = (drop >= 0) && (kinds[drop] == kind);
        if (sameKind || (s_pendingCount >= CELSP_MAX_PENDING)) {
            const int removeAt = (drop >= 0) ? drop : 0;
            for (int i = removeAt; i + 1 < s_pendingCount; ++i) {
                s_pending[i] = s_pending[i + 1];
            }
            --s_pendingCount;
        }
    }
    CELSP_PENDING* const p = &s_pending[s_pendingCount++];
    p->id = id;
    p->kind = kind;
    p->version = s_version;
    p->deadline = deadline;
    p->generation = generation;
    p->wordStart = wordStart;
    p->root[0] = '\0';
    if (root) {
        const size_t n = strlen(root);
        const size_t copy = (n < (sizeof(p->root) - 1u)) ? n : (sizeof(p->root) - 1u);
        memcpy(p->root, root, copy);
        p->root[copy] = '\0';
    }
    const bool armTimer = (s_pendingCount > 0) && (s_hwndMain != NULL);
    _Unlock();
    if (armTimer) {
        SetTimer(s_hwndMain, IDT_TIMER_CELSP_TIMEOUT, CELSP_REQUEST_TIMEOUT_TIMER_MS, NULL);
    }
    return true;
}

static bool _SendRequest(const char* method, const CELSP_JSON* params, int kind,
                         DocPos wordStart, const char* root)
{
    int id = 0;
    bool wrapped = false;
    _Lock();
    if (s_nextId >= INT_MAX) {
        s_nextId = 1;
        s_pendingCount = 0;
        wrapped = true;
    }
    id = s_nextId++;
    _Unlock();
    if (wrapped) {
        CeLspFeatures_OnLogMessage("LSP request id wrapped; pending requests cleared", 2);
    }
    const ULONGLONG deadline = GetTickCount64() + _RequestTimeoutMs(kind);
    const UINT generation = (UINT)InterlockedCompareExchange(&s_generation, 0, 0);
    _AddPending(id, kind, wordStart, root, deadline, generation);
    size_t frameLen = 0u;
    char* const frame = CeLspFraming_EncodeRequest(id, method, params, &frameLen);
    return _Enqueue(frame, frameLen);
}

static bool _EnsureStarted(void)
{
    if (s_ready && s_running) {
        return true;
    }
    if (s_starting || s_running) {
        return false; // handshake not finished yet; caller uses the fallback
    }
    return CeLspClient_Start(s_hwndMain ? s_hwndMain : Globals.hwndMain);
}

// A provider is advertised either as `true` or as an options object.
static bool _CapPresent(const CELSP_JSON* caps, const char* key)
{
    const CELSP_JSON* const v = CeLspJson_Get(caps, key);
    if (!v) {
        return false;
    }
    return (CeLspJson_Type(v) == CELSP_JSON_BOOL) ? CeLspJson_Bool(v) : true;
}

static void _ParseCapabilities(const CELSP_JSON* caps)
{
    s_caps = 0u;
    s_tokenTypeCount = 0;
    if (!caps) {
        return;
    }
    struct { const char* key; int bit; } const table[] = {
        { "definitionProvider",        CELSP_CAP_DEFINITION },
        { "declarationProvider",       CELSP_CAP_DECLARATION },
        { "referencesProvider",        CELSP_CAP_REFERENCES },
        { "documentSymbolProvider",    CELSP_CAP_DOCSYMBOLS },
        { "workspaceSymbolProvider",   CELSP_CAP_WSSYMBOLS },
        { "documentFormattingProvider", CELSP_CAP_FORMATTING },
        { "semanticTokensProvider",    CELSP_CAP_SEMTOKENS },
        { "inlayHintProvider",         CELSP_CAP_INLAY },
        { "codeLensProvider",          CELSP_CAP_CODELENS },
        { "documentLinkProvider",      CELSP_CAP_DOCLINKS },
        { "documentHighlightProvider", CELSP_CAP_DOCHIGHLIGHT }
    };
    for (size_t i = 0u; i < COUNTOF(table); ++i) {
        if (_CapPresent(caps, table[i].key)) {
            s_caps |= (1u << (unsigned)table[i].bit);
        }
    }
    // semanticTokens legend: tokenTypes[i] names the value i in the data tuples.
    const CELSP_JSON* const stp = CeLspJson_Get(caps, "semanticTokensProvider");
    const CELSP_JSON* const legend = CeLspJson_Get(stp, "legend");
    const CELSP_JSON* const types = CeLspJson_Get(legend, "tokenTypes");
    const size_t count = CeLspJson_Count(types);
    if (count > COUNTOF(s_tokenTypes)) {
        s_caps &= ~(1u << (unsigned)CELSP_CAP_SEMTOKENS);
        s_tokenTypeCount = 0;
        CeLspFeatures_OnLogMessage("LSP semantic token legend exceeds 32 types; disabled", 2);
        return;
    }
    for (size_t i = 0u; (i < count) && (i < COUNTOF(s_tokenTypes)); ++i) {
        const char* const name = CeLspJson_String(CeLspJson_At(types, i));
        if (!name) {
            continue;
        }
        const size_t n = strlen(name);
        const size_t copy = (n < (sizeof(s_tokenTypes[0]) - 1u)) ? n : (sizeof(s_tokenTypes[0]) - 1u);
        memcpy(s_tokenTypes[s_tokenTypeCount], name, copy);
        s_tokenTypes[s_tokenTypeCount][copy] = '\0';
        ++s_tokenTypeCount;
    }
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

bool CeLspClient_IsReady(void)
{
    return (s_ready != 0) && (s_running != 0);
}

bool CeLspClient_IsStarting(void)
{
    return (s_starting != 0) || (s_running != 0);
}

bool CeLspClient_UsesUtf16(void)
{
    return s_utf16;
}

bool CeLspClient_Start(HWND hwndMain)
{
    if (s_running) {
        return true;
    }
    if (!_Celsp_ShouldRetryStart(GetTickCount64(), s_startFailedAt, s_restarts)) {
        return false; // cooldown avoids spawning a process on every keystroke
    }
    s_startFailedAt = 0u;
    // SCN_* handlers pass the Scintilla editor window; the LSP worker threads
    // post WM_CELSP_* to this handle, so it must be the main window whose
    // message loop dispatches them.
    HWND const target = Globals.hwndMain ? Globals.hwndMain : hwndMain;
    if (!target) {
        return false;
    }
    InterlockedExchange(&s_starting, 1);
    s_hwndMain = target;
    if (!s_csInit) {
        InitializeCriticalSection(&s_cs);
        s_csInit = true;
    }

    HANDLE hInR = NULL, hInW = NULL, hOutR = NULL, hOutW = NULL;
    HANDLE hErrR = NULL, hErrW = NULL;
    WCHAR* serverPath = NULL;
    WCHAR* modulePath = NULL;
    WCHAR* cmdLine = NULL;
    const WCHAR* const configuredPath = Path_Get(Settings2.CeLspServerPath);
    if (configuredPath && *configuredPath) {
        const size_t n = wcslen(configuredPath) + 1u;
        serverPath = (WCHAR*)calloc(n, sizeof(WCHAR));
        if (serverPath) {
            StringCchCopyW(serverPath, n, configuredPath);
        }
    }
    else {
        modulePath = (WCHAR*)calloc(PATHLONG_MAX_CCH, sizeof(WCHAR));
        if (modulePath && GetModuleFileNameW(NULL, modulePath, PATHLONG_MAX_CCH) != 0u) {
            WCHAR* const slash = wcsrchr(modulePath, L'\\');
            if (slash) {
                *(slash + 1) = L'\0';
            }
            const size_t n = wcslen(modulePath) + wcslen(L"CeLspServer.exe") + 1u;
            serverPath = (WCHAR*)calloc(n, sizeof(WCHAR));
            if (serverPath) {
                StringCchPrintfW(serverPath, n, L"%sCeLspServer.exe", modulePath);
            }
        }
    }
    if (!serverPath) {
        s_startFailedAt = GetTickCount64();
        _PostStatus(CELSP_STATUS_ERROR, "Cannot resolve CeLspServer.exe path");
        goto failPipes;
    }
    const size_t argsLen = wcslen(Settings2.CeLspServerArgs);
    const size_t cmdCap = wcslen(serverPath) + argsLen + 8u;
    cmdLine = (WCHAR*)calloc(cmdCap, sizeof(WCHAR));
    if (!cmdLine) {
        s_startFailedAt = GetTickCount64();
        _PostStatus(CELSP_STATUS_ERROR, "Cannot build CeLspServer.exe command line");
        goto failPipes;
    }
    if (argsLen) {
        StringCchPrintfW(cmdLine, cmdCap, L"\"%s\" %s", serverPath, Settings2.CeLspServerArgs);
    }
    else {
        StringCchPrintfW(cmdLine, cmdCap, L"\"%s\"", serverPath);
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hOutR, &hOutW, &sa, 0) || !CreatePipe(&hInR, &hInW, &sa, 0) ||
        !CreatePipe(&hErrR, &hErrW, &sa, 0)) {
        s_startFailedAt = GetTickCount64();
        _PostStatus(CELSP_STATUS_ERROR, "Cannot create LSP stdio pipes");
        goto failPipes;
    }
    SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hInW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hErrR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hInR;
    si.hStdOutput = hOutW;
    si.hStdError = hErrW;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        s_startFailedAt = GetTickCount64();
        _PostStatus(CELSP_STATUS_ERROR, "Cannot start CeLspServer.exe; using local completion");
        goto failPipes;
    }
    s_startFailedAt = 0u;
    CloseHandle(pi.hThread);
    CloseHandle(hInR);  hInR = NULL;
    CloseHandle(hOutW); hOutW = NULL;
    CloseHandle(hErrW); hErrW = NULL;

    s_hStdinW = hInW;
    s_hStdoutR = hOutR;
    s_hStderrR = hErrR;
    s_hProcess = pi.hProcess;
    if (!s_hEvent) {
        s_hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    if (!s_hStopEvent) {
        s_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    }
    if (!s_hEvent || !s_hStopEvent) {
        goto failProcess;
    }
    ResetEvent(s_hEvent);
    ResetEvent(s_hStopEvent);

    InterlockedIncrement(&s_generation);
    InterlockedExchange(&s_running, 1);
    InterlockedExchange(&s_ready, 0);
    InterlockedExchange(&s_stopping, 0);
    InterlockedExchange(&s_starting, 0);

    _Lock();
    _FreeQueue();
    s_pendingCount = 0;
    _Unlock();
    _Snapshot();

    UINT const sessionGeneration = (UINT)InterlockedCompareExchange(&s_generation, 0, 0);
    const uintptr_t thRead = _beginthreadex(NULL, 0, _ReaderThread,
                                            (void*)(uintptr_t)sessionGeneration, 0, NULL);
    const uintptr_t thWrite = _beginthreadex(NULL, 0, _WriterThread,
                                            (void*)(uintptr_t)sessionGeneration, 0, NULL);
    const uintptr_t thStderr = _beginthreadex(NULL, 0, _StderrThread,
                                             (void*)(uintptr_t)sessionGeneration, 0, NULL);
    if ((thRead == 0) || (thWrite == 0) || (thStderr == 0)) {
        s_hThreadRead = (HANDLE)thRead;
        s_hThreadWrite = (HANDLE)thWrite;
        s_hThreadStderr = (HANDLE)thStderr;
        CeLspClient_Stop();
        return false;
    }
    s_hThreadRead = (HANDLE)thRead;
    s_hThreadWrite = (HANDLE)thWrite;
    s_hThreadStderr = (HANDLE)thStderr;

    // initialize: negotiate UTF-8 positions (Scintilla's native encoding).
    CELSP_JSON* const general = CeLspJson_NewObject();
    CELSP_JSON* const encodings = CeLspJson_NewArray();
    if (encodings) {
        CeLspJson_Add(encodings, CeLspJson_NewString("utf-8"));
        CeLspJson_Add(encodings, CeLspJson_NewString("utf-16"));
    }
    if (general) {
        CeLspJson_Set(general, "positionEncodings", encodings);
    }
    else {
        CeLspJson_Free(encodings);
    }
    CELSP_JSON* const params = CeLspJson_NewObject();
    if (params) {
        CeLspJson_Set(params, "processId", CeLspJson_NewNumber((double)GetCurrentProcessId()));
        CeLspJson_Set(params, "rootUri", CeLspJson_NewNull());
        CeLspJson_Set(params, "capabilities", general);
        CeLspJson_Set(params, "workspaceFolders", CeLspJson_NewNull());
    }
    else {
        CeLspJson_Free(general);
    }
    _SendRequest("initialize", params, CELSP_REQ_INIT, 0, NULL);
    CeLspJson_Free(params);
    free(cmdLine);
    free(serverPath);
    free(modulePath);
    return true;

failProcess:
    free(cmdLine);
    free(serverPath);
    free(modulePath);
    if (s_hProcess) { TerminateProcess(s_hProcess, 1u); CloseHandle(s_hProcess); s_hProcess = NULL; }
    if (s_hStdinW) { CloseHandle(s_hStdinW); s_hStdinW = NULL; }
    if (s_hStdoutR) { CloseHandle(s_hStdoutR); s_hStdoutR = NULL; }
    if (s_hStderrR) { CloseHandle(s_hStderrR); s_hStderrR = NULL; }
    InterlockedExchange(&s_starting, 0);
    return false;
failPipes:
    free(cmdLine);
    free(serverPath);
    free(modulePath);
    if (hInR) { CloseHandle(hInR); }
    if (hInW) { CloseHandle(hInW); }
    if (hOutR) { CloseHandle(hOutR); }
    if (hOutW) { CloseHandle(hOutW); }
    if (hErrR) { CloseHandle(hErrR); }
    if (hErrW) { CloseHandle(hErrW); }
    InterlockedExchange(&s_starting, 0);
    return false;
}

static void _CompleteTeardown(CELSP_TEARDOWN_STATE* st)
{
    if (!st) {
        return;
    }
    if (st->hProcess) {
        if (WaitForSingleObject(st->hProcess, CELSP_STOP_TIMEOUT) == WAIT_TIMEOUT) {
            TerminateProcess(st->hProcess, 1u);
        }
        CloseHandle(st->hProcess);
    }
    const bool readerExited = !st->hThreadRead ||
        (WaitForSingleObject(st->hThreadRead, CELSP_THREAD_TIMEOUT) != WAIT_TIMEOUT);
    const bool writerExited = !st->hThreadWrite ||
        (WaitForSingleObject(st->hThreadWrite, CELSP_THREAD_TIMEOUT) != WAIT_TIMEOUT);
    const bool stderrExited = !st->hThreadStderr ||
        (WaitForSingleObject(st->hThreadStderr, CELSP_THREAD_TIMEOUT) != WAIT_TIMEOUT);
    if (st->hThreadRead && readerExited) { CloseHandle(st->hThreadRead); }
    if (st->hThreadWrite && writerExited) { CloseHandle(st->hThreadWrite); }
    if (st->hThreadStderr && stderrExited) { CloseHandle(st->hThreadStderr); }
    if (st->hIn && !st->hThreadWrite) { CloseHandle(st->hIn); }
    if (st->hOut && !st->hThreadRead) { CloseHandle(st->hOut); }
    if (st->hStderrR && !st->hThreadStderr) { CloseHandle(st->hStderrR); }
    const bool threadsExited = readerExited && writerExited && stderrExited;
    if (st->final) {
        if (threadsExited) {
            if (st->hEvent) { CloseHandle(st->hEvent); }
            if (st->hStop) { CloseHandle(st->hStop); }
        }
        _Lock();
        free(s_text); s_text = NULL;
        s_textLen = 0u;
        s_textCap = 0u;
        free(s_uri); s_uri = NULL;
        s_docOpen = false;
        s_dirty = false;
        s_version = 0;
        _Unlock();
    }
    _FreeQueueNodes(st->queue);
    if (!st->final && s_hwndMain && IsWindow(s_hwndMain)) {
        PostMessageW(s_hwndMain, WM_CELSP_STATUS, (WPARAM)CELSP_STATUS_TEARDOWN_DONE,
                     (LPARAM)(uintptr_t)st->generation);
    }
}

static unsigned __stdcall _TeardownThread(void* arg)
{
    CELSP_TEARDOWN_STATE* const st = (CELSP_TEARDOWN_STATE*)arg;
    _CompleteTeardown(st);
    free(st);
    return 0u;
}

static void _Teardown(bool final)
{
    if (!s_running && !s_hProcess) {
        return;
    }
    CELSP_TEARDOWN_STATE* const st = (CELSP_TEARDOWN_STATE*)calloc(1u, sizeof(CELSP_TEARDOWN_STATE));
    if (!st) {
        return;
    }
    st->final = final;
    st->generation = (UINT)InterlockedIncrement(&s_generation);
    InterlockedExchange(&s_stopping, 1);
    InterlockedExchange(&s_ready, 0);
    s_caps = 0u;
    s_tokenTypeCount = 0;

    // The writer closes stdin when it observes this generation boundary; the
    // built-in server exits on EOF.  No shutdown/exit frames are queued here:
    // the queue is detached immediately and must not be drained by an old
    // worker after a restarted session has started enqueueing.
    InterlockedExchange(&s_running, 0);
    if (s_hEvent) { SetEvent(s_hEvent); }
    if (s_hStopEvent) { SetEvent(s_hStopEvent); }

    st->hThreadRead = s_hThreadRead;
    st->hThreadWrite = s_hThreadWrite;
    st->hThreadStderr = s_hThreadStderr;
    st->hIn = s_hStdinW;
    st->hOut = s_hStdoutR;
    st->hStderrR = s_hStderrR;
    st->hProcess = s_hProcess;
    st->hEvent = s_hEvent;
    st->hStop = s_hStopEvent;
    st->queue = _DetachQueue();
    s_hThreadRead = NULL;
    s_hThreadWrite = NULL;
    s_hThreadStderr = NULL;
    s_hStdinW = NULL;
    s_hStdoutR = NULL;
    s_hStderrR = NULL;
    s_hProcess = NULL;
    if (final) {
        s_hEvent = NULL;
        s_hStopEvent = NULL;
    }

    _Lock();
    s_pendingCount = 0;
    s_docOpen = false;
    s_dirty = !final; // re-send didOpen after a restart handshake
    s_fullSync = true;
    if (final) { s_version = 0; }
    _Unlock();

    if (s_hwndMain && !final) {
        SetTimer(s_hwndMain, IDT_TIMER_CELSP_TEARDOWN, CELSP_TEARDOWN_TIMER_MS, NULL);
    }
    InterlockedExchange(&s_stopping, 0);

    if (final || (_beginthreadex(NULL, 0, _TeardownThread, st, 0, NULL) == 0)) {
        _CompleteTeardown(st);
        free(st);
    }
}

void CeLspClient_Stop(void)
{
    _ClearRanges();
    _Teardown(true);
}

void CeLspClient_OnTeardownTimer(void)
{
    if (s_hwndMain) {
        KillTimer(s_hwndMain, IDT_TIMER_CELSP_TEARDOWN);
    }
}

void CeLspClient_ResetStartFailure(void)
{
    s_startFailedAt = 0u;
}

void CeLspClient_OnDocumentOpened(void)
{
    s_startFailedAt = 0u;   // a new document gets an immediate start attempt
    _ClearRanges();
    s_fullSync = true;
    _Lock();
    s_docOpen = false;
    _Unlock();
    _Snapshot();
    if (s_hwndMain) {
        SetTimer(s_hwndMain, IDT_TIMER_CELSP_SYNC,
                 (UINT)Settings2.CeLspDebounceMs, NULL);
    }
}

void CeLspClient_OnDocumentChanged(void)
{
    _Snapshot();
    if (s_hwndMain) {
        SetTimer(s_hwndMain, IDT_TIMER_CELSP_SYNC,
                 (UINT)Settings2.CeLspDebounceMs, NULL);
    }
}

void CeLspClient_OnSyncTimer(void)
{
    if (s_hwndMain) {
        KillTimer(s_hwndMain, IDT_TIMER_CELSP_SYNC);
    }
    if (!CeLspClient_IsReady()) {
        return;
    }
    _FlushChanges();
    // The server has the current text now: re-pull the whole-document views
    // (semantic tokens, inlay hints, code lens, document links).
    CeLspFeatures_RefreshDecorations();
}

void CeLspClient_OnDocumentSaved(void)
{
    if (!CeLspClient_IsReady()) {
        return;
    }
    _FlushChanges();
    size_t frameLen = 0u;
    CELSP_JSON* const params = CeLspJson_NewObject();
    _Lock();
    if (params && s_uri) {
        CELSP_JSON* const doc = CeLspJson_NewObject();
        if (doc) {
            CeLspJson_Set(doc, "uri", CeLspJson_NewString(s_uri));
            CeLspJson_Set(params, "textDocument", doc);
        }
    }
    _Unlock();
    char* const frame = CeLspFraming_EncodeNotification("textDocument/didSave", params, &frameLen);
    _Enqueue(frame, frameLen);
    CeLspJson_Free(params);
}

void CeLspClient_OnDocumentClosed(void)
{
    if (CeLspClient_IsReady()) {
        size_t frameLen = 0u;
        CELSP_JSON* const params = CeLspJson_NewObject();
        CELSP_JSON* const doc = CeLspJson_NewObject();
        _Lock();
        if (params && doc) {
            CeLspJson_Set(doc, "uri", CeLspJson_NewString(s_uri ? s_uri : "file:///untitled.cea"));
            CeLspJson_Set(params, "textDocument", doc);
        }
        else {
            CeLspJson_Free(doc);
        }
        _Unlock();
        char* const frame = CeLspFraming_EncodeNotification("textDocument/didClose", params, &frameLen);
        _Enqueue(frame, frameLen);
        CeLspJson_Free(params);
    }
    _Lock();
    s_docOpen = false;
    s_dirty = false;
    _Unlock();
}

static bool _SendCompletionRequest(const char* root, DocPos wordStart, int triggerKind, int triggerChar)
{
    _FlushChanges();
    CELSP_JSON* const params = CeLspJson_NewObject();
    _Position(params, SciCall_GetCurrentPos());
    CELSP_JSON* const context = CeLspJson_NewObject();
    if (context) {
        CeLspJson_Set(context, "triggerKind", CeLspJson_NewNumber((double)triggerKind));
        if (triggerChar > 0) {
            char ch[2] = { (char)triggerChar, '\0' };
            CeLspJson_Set(context, "triggerCharacter", CeLspJson_NewString(ch));
        }
        CeLspJson_Set(params, "context", context);
    }
    const bool ok = _SendRequest("textDocument/completion", params,
                                 CELSP_REQ_COMPLETION, wordStart, root);
    CeLspJson_Free(params);
    return ok;
}

bool CeLspClient_RequestCompletion(const char* root, DocPos wordStart, int triggerKind, int triggerChar)
{
    if (!CeLspClient_IsReady() && !_EnsureStarted()) {
        return false;
    }
    if (!CeLspClient_IsReady()) {
        CeLspClient_OnDocumentChanged();
        return false;
    }
    StringCchCopyA(s_debouncedRoot, COUNTOF(s_debouncedRoot), root ? root : "");
    s_debouncedWordStart = wordStart;
    s_debouncedTriggerKind = triggerKind;
    s_debouncedTriggerChar = triggerChar;
    s_completionArmed = true;
    if (!s_hwndMain) {
        CeLspClient_OnCompletionTimer();
        return true;
    }
    SetTimer(s_hwndMain, IDT_TIMER_CELSP_COMPLETION, CELSP_COMPLETION_DEBOUNCE_MS, NULL);
    return true;
}

void CeLspClient_OnCompletionTimer(void)
{
    if (s_hwndMain) {
        KillTimer(s_hwndMain, IDT_TIMER_CELSP_COMPLETION);
    }
    if (!s_completionArmed) {
        return;
    }
    s_completionArmed = false;
    (void)_SendCompletionRequest(s_debouncedRoot, s_debouncedWordStart,
                                 s_debouncedTriggerKind, s_debouncedTriggerChar);
}

void CeLspClient_CancelCompletionDebounce(void)
{
    if (s_hwndMain) {
        KillTimer(s_hwndMain, IDT_TIMER_CELSP_COMPLETION);
    }
    s_completionArmed = false;
}

bool CeLspClient_RequestSignatureHelp(void)
{
    if (!CeLspClient_IsReady()) {
        return false;
    }
    _FlushChanges();
    CELSP_JSON* const params = CeLspJson_NewObject();
    _Position(params, SciCall_GetCurrentPos());
    const bool ok = _SendRequest("textDocument/signatureHelp", params,
                                 CELSP_REQ_SIGNATURE, 0, NULL);
    CeLspJson_Free(params);
    return ok;
}

bool CeLspClient_RequestHover(DocPos position)
{
    if (!CeLspClient_IsReady()) {
        return false;
    }
    _FlushChanges();
    CELSP_JSON* const params = CeLspJson_NewObject();
    _Position(params, position);
    const bool ok = _SendRequest("textDocument/hover", params, CELSP_REQ_HOVER, 0, NULL);
    CeLspJson_Free(params);
    return ok;
}

bool CeLspClient_RequestPositional(const char* method, int kind)
{
    if (!CeLspClient_IsReady() || !method) {
        return false;
    }
    _FlushChanges();
    CELSP_JSON* const params = CeLspJson_NewObject();
    _Position(params, SciCall_GetCurrentPos());
    const bool ok = _SendRequest(method, params, kind, 0, NULL);
    CeLspJson_Free(params);
    return ok;
}

bool CeLspClient_RequestReferences(void)
{
    if (!CeLspClient_IsReady()) {
        return false;
    }
    _FlushChanges();
    CELSP_JSON* const params = CeLspJson_NewObject();
    _Position(params, SciCall_GetCurrentPos());
    CELSP_JSON* const context = CeLspJson_NewObject();
    if (context) {
        CeLspJson_Set(context, "includeDeclaration", CeLspJson_NewBool(true));
        CeLspJson_Set(params, "context", context);
    }
    const bool ok = _SendRequest("textDocument/references", params, CELSP_REQ_REFERENCES, 0, NULL);
    CeLspJson_Free(params);
    return ok;
}

bool CeLspClient_RequestWorkspaceSymbols(const char* query)
{
    if (!CeLspClient_IsReady()) {
        return false;
    }
    CELSP_JSON* const params = CeLspJson_NewObject();
    if (params) {
        CeLspJson_Set(params, "query", CeLspJson_NewString(query ? query : ""));
    }
    const bool ok = _SendRequest("workspace/symbol", params, CELSP_REQ_WSSYMBOLS, 0, NULL);
    CeLspJson_Free(params);
    return ok;
}

bool CeLspClient_RequestFormatting(void)
{
    if (!CeLspClient_IsReady()) {
        return false;
    }
    _FlushChanges();
    // textDocument/formatting: { textDocument: {uri}, options: {...} } - no position.
    CELSP_JSON* const params = CeLspJson_NewObject();
    char* uri = NULL;
    _Lock();
    uri = _StrDup(s_uri ? s_uri : "file:///untitled.cea");
    _Unlock();
    CELSP_JSON* const doc = CeLspJson_NewObject();
    if (params && doc) {
        CeLspJson_Set(doc, "uri", CeLspJson_NewString(uri ? uri : "file:///untitled.cea"));
        CeLspJson_Set(params, "textDocument", doc);
        CELSP_JSON* const options = CeLspJson_NewObject();
        if (options) {
            CeLspJson_Set(options, "tabSize", CeLspJson_NewNumber(4.0));
            CeLspJson_Set(options, "insertSpaces", CeLspJson_NewBool(true));
            CeLspJson_Set(params, "options", options);
        }
        else {
            CeLspJson_Free(options);
        }
    }
    else {
        CeLspJson_Free(doc);
    }
    free(uri);
    const bool ok = _SendRequest("textDocument/formatting", params, CELSP_REQ_FORMATTING, 0, NULL);
    CeLspJson_Free(params);
    return ok;
}

bool CeLspClient_HasCap(int cap)
{
    return (cap >= 0) && (cap < CELSP_CAP_CNT) && ((s_caps & (1u << (unsigned)cap)) != 0u);
}

bool CeLspClient_DiagnosticSeverityVisible(int severity)
{
    return _Celsp_DiagSeverityForSquiggle(severity);
}

bool CeLspClient_PosToOffset(int line, int character, DocPos* out)
{
    if (!out) {
        return false;
    }
    bool ok = false;
    _Lock();
    if (s_text && (s_textLen > 0u)) {
        const size_t off = CeLspPos_ToOffset(s_text, s_textLen, line, character, s_utf16);
        *out = (DocPos)off;
        ok = true;
    }
    _Unlock();
    return ok;
}

int CeLspClient_TokenTypeCount(void)
{
    return s_tokenTypeCount;
}

const char* CeLspClient_TokenType(int idx)
{
    if ((idx < 0) || (idx >= s_tokenTypeCount)) {
        return NULL;
    }
    return s_tokenTypes[idx];
}

void CeLspClient_OnRequestTimeoutTimer(void)
{
    if (s_hwndMain) {
        KillTimer(s_hwndMain, IDT_TIMER_CELSP_TIMEOUT);
    }
    const ULONGLONG now = GetTickCount64();
    const UINT generation = (UINT)InterlockedCompareExchange(&s_generation, 0, 0);
    int expired[CELSP_MAX_PENDING];
    int expiredCount = 0;
    _Lock();
    int write = 0;
    for (int i = 0; i < s_pendingCount; ++i) {
        const bool stale = !_Celsp_MessageIsCurrent(s_pending[i].generation, generation) ||
                           _Celsp_RequestExpired(now, s_pending[i].deadline);
        if (stale) {
            expired[expiredCount++] = s_pending[i].id;
            continue;
        }
        s_pending[write++] = s_pending[i];
    }
    s_pendingCount = write;
    const bool armTimer = (write > 0) && (s_hwndMain != NULL);
    _Unlock();
    for (int i = 0; i < expiredCount; ++i) {
        _PostCancelRequest(expired[i]);
    }
    if (armTimer) {
        SetTimer(s_hwndMain, IDT_TIMER_CELSP_TIMEOUT, CELSP_REQUEST_TIMEOUT_TIMER_MS, NULL);
    }
}

void CeLspClient_CancelPending(int kind)
{
    int ids[CELSP_MAX_PENDING];
    int idCount = 0;
    _Lock();
    int write = 0;
    for (int i = 0; i < s_pendingCount; ++i) {
        if (s_pending[i].kind != kind) {
            s_pending[write++] = s_pending[i];
            continue;
        }
        ids[idCount++] = s_pending[i].id;
    }
    s_pendingCount = write;
    _Unlock();
    for (int i = 0; i < idCount; ++i) {
        _PostCancelRequest(ids[i]);
    }
}

// ----------------------------------------------------------------------------
// Main-thread dispatch of server messages
// ----------------------------------------------------------------------------

static void _HandleNotification(UINT generation, const char* method, const CELSP_JSON* params)
{
    if (strcmp(method, "textDocument/publishDiagnostics") == 0) {
        const char* const uri = params ? CeLspJson_GetString(params, "uri") : NULL;
        int const version = params ? CeLspJson_GetInt(params, "version") : 0;
        if (!_Celsp_DiagIsCurrent(uri, version, generation,
                                  s_uri, s_version, (UINT)InterlockedCompareExchange(&s_generation, 0, 0))) {
            CeLspFeatures_OnDiagnostics(NULL); // clear stale decorations from the old session
            return;
        }
        CeLspFeatures_OnDiagnostics(params);
    }
    else if ((strcmp(method, "window/logMessage") == 0) ||
             (strcmp(method, "window/showMessage") == 0)) {
        const char* const message = CeLspJson_GetString(params, "message");
        CeLspFeatures_OnLogMessage(message ? message : "", 3);
    }
    else if (strcmp(method, "np3/lspResync") == 0) {
        // The server rejected an incremental range; resend the whole snapshot.
        _Lock();
        s_fullSync = true;
        s_dirty = true;
        _Unlock();
        if (s_hwndMain) {
            SetTimer(s_hwndMain, IDT_TIMER_CELSP_SYNC,
                     (UINT)Settings2.CeLspDebounceMs, NULL);
        }
    }
    else if (strcmp(method, "window/showDocument") == 0) {
        // No workspace project model: ignore.
    }
}

void CeLspClient_OnMessage(WPARAM generation, LPARAM payload)
{
    char* const json = (char*)payload;
    if (!json) {
        return;
    }
    CELSP_JSON* const msg = CeLspJson_Parse(json, 0u);
    free(json);
    if (!msg) {
        CeLspFeatures_OnLogMessage("Malformed JSON-RPC message from LSP server", 1);
        return;
    }
    if (!_Celsp_MessageIsCurrent((UINT)generation,
                                 (UINT)InterlockedCompareExchange(&s_generation, 0, 0))) {
        CeLspJson_Free(msg);   // response from a torn-down server generation
        return;
    }
    const char* const method = CeLspJson_GetString(msg, "method");
    if (method) {
        _HandleNotification((UINT)generation, method, CeLspJson_Get(msg, "params"));
        CeLspJson_Free(msg);
        return;
    }

    const int id = CeLspJson_GetInt(msg, "id");
    CELSP_PENDING req = { 0 };
    bool found = false;
    _Lock();
    for (int i = 0; i < s_pendingCount; ++i) {
        if (s_pending[i].id == id) {
            req = s_pending[i];
            for (int j = i; j + 1 < s_pendingCount; ++j) {
                s_pending[j] = s_pending[j + 1];
            }
            --s_pendingCount;
            found = true;
            break;
        }
    }
    _Unlock();
    if (!found) {
        CeLspJson_Free(msg);
        return;
    }
    if ((req.kind != CELSP_REQ_INIT) && (req.kind != CELSP_REQ_SHUTDOWN)) {
        int currentVersion = 0;
        _Lock();
        currentVersion = s_version;
        _Unlock();
        if (req.version != currentVersion) {
            // Stale result: the document changed while the request was in
            // flight.  Never show a completion list for old text.
            CeLspJson_Free(msg);
            return;
        }
    }

    const CELSP_JSON* const error = CeLspJson_Get(msg, "error");
    const CELSP_JSON* const result = CeLspJson_Get(msg, "result");
    if (error) {
        const char* const text = CeLspJson_GetString(error, "message");
        CeLspFeatures_OnLogMessage(text ? text : "LSP request failed", 1);
        CeLspJson_Free(msg);
        return;
    }

    switch (req.kind) {
    case CELSP_REQ_INIT: {
        const CELSP_JSON* const caps = CeLspJson_Get(result, "capabilities");
        const char* const encoding = CeLspJson_GetString(caps, "positionEncoding");
        s_utf16 = (encoding && (strcmp(encoding, "utf-16") == 0));
        _ParseCapabilities(caps);
        InterlockedExchange(&s_ready, 1);
        s_restarts = 0;
        size_t frameLen = 0u;
        char* const initialized = CeLspFraming_EncodeNotification("initialized", NULL, &frameLen);
        _Enqueue(initialized, frameLen);
        _FlushChanges(); // didOpen with the current snapshot
        CeLspFeatures_OnServerStatus(CELSP_STATUS_READY, NULL);
        break;
    }
    case CELSP_REQ_COMPLETION:
        CeLspFeatures_OnCompletion(result, &req);
        break;
    case CELSP_REQ_SIGNATURE:
        CeLspFeatures_OnSignatureHelp(result);
        break;
    case CELSP_REQ_HOVER:
        CeLspFeatures_OnHover(result);
        break;
    case CELSP_REQ_DEFINITION:
    case CELSP_REQ_DECLARATION:
    case CELSP_REQ_REFERENCES:
        CeLspFeatures_OnLocations(result, req.kind);
        break;
    case CELSP_REQ_DOCSYMBOLS:
        CeLspFeatures_OnDocumentSymbols(result);
        break;
    case CELSP_REQ_WSSYMBOLS:
        CeLspFeatures_OnWorkspaceSymbols(result);
        break;
    case CELSP_REQ_FORMATTING:
        CeLspFeatures_OnFormatting(result);
        break;
    case CELSP_REQ_SEMTOKENS:
        CeLspFeatures_OnSemanticTokens(result);
        break;
    case CELSP_REQ_INLAY:
        CeLspFeatures_OnInlayHints(result);
        break;
    case CELSP_REQ_CODELENS:
        CeLspFeatures_OnCodeLens(result);
        break;
    case CELSP_REQ_DOCLINKS:
        CeLspFeatures_OnDocumentLinks(result);
        break;
    case CELSP_REQ_DOCHIGHLIGHT:
        CeLspFeatures_OnDocumentHighlight(result);
        break;
    case CELSP_REQ_SHUTDOWN:
    default:
        break;
    }
    CeLspJson_Free(msg);
}

void CeLspClient_OnStatus(WPARAM status, LPARAM detail)
{
    if (status == CELSP_STATUS_TEARDOWN_DONE) {
        if (s_hwndMain && _Celsp_MessageIsCurrent((UINT)(uintptr_t)detail,
                (UINT)InterlockedCompareExchange(&s_generation, 0, 0))) {
            KillTimer(s_hwndMain, IDT_TIMER_CELSP_TEARDOWN);
        }
        return;
    }
    char* const message = (char*)detail;
    CeLspFeatures_OnServerStatus((int)status, message);
    if ((status == CELSP_STATUS_EXITED) && !s_stopping) {
        ++s_restarts;
        _Teardown(false);
        if (s_restarts <= CELSP_MAX_RESTARTS) {
            CeLspClient_Start(s_hwndMain ? s_hwndMain : Globals.hwndMain);
        }
        else {
            // Stop the tight crash loop; a new document / setting change can
            // retry immediately after the cooldown.
            s_startFailedAt = GetTickCount64();
        }
    }
    free(message);
}
