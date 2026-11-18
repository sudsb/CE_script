// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeSession.c                                                                 *
*   CE editor sessions and the explicit "add to current address list" action.*
*   The pure helpers at the top are also exercised by test/TestCeSession.ps1,*
*   so they must stay free of Win32/Scintilla dependencies.                  *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <wchar.h>
#include <process.h>

#include "TypeDefs.h"
#include "Notepad3.h"
#include "SciCall.h"
#include "CeScript.h"
#include "CeBridge.h"
#include "CeOutput.h"
#include "CeSession.h"
#include "..\language\common_res.h"

#define CE_SESSION_META_MAX (8192)
// Bounds for the shutdown close-session request.  They are deliberately short:
// the application is exiting and must not stall on a busy CE.
#define CE_SESSION_CLOSE_PIPE_WAIT_MS    400
#define CE_SESSION_CLOSE_REPLY_TIMEOUT_MS 800

typedef struct _CE_SESSION_META {
    char id[64];
    int  memrecId;
    char description[128];
    char address[64];
    char templateCaption[128];
    int  format;             // 1: freshly generated code, format it once
} CE_SESSION_META;

typedef struct _CE_SESSION_STATE {
    bool  active;
    char  id[64];
    char  description[128];
    char  address[64];
    char  templateCaption[128];
    WCHAR metaPath[MAX_PATH];
} CE_SESSION_STATE;

typedef struct _CE_SESSION_JOB {
    HWND  hwnd;
    char* payload;
    DWORD len;
} CE_SESSION_JOB;

static CE_SESSION_STATE s_ceSession = { 0 };
// Set from the sidecar of a freshly generated script (right-click "自动汇编"):
// the editor formats it once, directly after the load, instead of waiting for
// the first save.  Consumed by CeSession_ConsumeFormatRequest(), so a later
// reload or tab switch never reformats the text again.
static bool s_formatOnOpen = false;
// Updated by the apply worker, read on the UI thread: keep it atomic.
static volatile LONG s_ceSessionMemrecId = 0;
static volatile LONG s_ceSessionApplyBusy = 0;
static volatile LONG s_ceSessionApplyStart = 0;   // watchdog against a stuck worker
static volatile LONG s_lastResult = 0;            // worker -> UI result code

// ----------------------------------------------------------------------------
// Pure helpers (kept Win32-free for the focused harness).
// ----------------------------------------------------------------------------

static bool _CeSession_BuildApplyPayload(char* dst, size_t cap, const char* sessionId,
                                         int memrecId, const char* script, size_t scriptLen,
                                         size_t* outLen)
{
    if (!dst || !sessionId || !outLen || (scriptLen > 0 && !script)) {
        return false;
    }
    char head[96] = { 0 };
    int const n = snprintf(head, sizeof(head), "%s|%d\n", sessionId, memrecId);
    if (n <= 0 || (size_t)n >= sizeof(head)) {
        return false;
    }
    size_t const need = (size_t)n + scriptLen;
    if (cap < need + 1) {
        return false;
    }
    memcpy(dst, head, (size_t)n);
    if (scriptLen > 0) {
        memcpy(dst + n, script, scriptLen);
    }
    dst[need] = '\0';
    *outLen = need;
    return true;
}

static bool _CeSession_ParseMeta(const char* text, size_t len, CE_SESSION_META* out)
{
    if (!text || !out || len == 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    bool hasId = false;
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n' && text[e] != '\r') {
            ++e;
        }
        size_t const lineLen = e - i;
        if (lineLen > 0 && lineLen < 512) {
            char line[512] = { 0 };
            memcpy(line, text + i, lineLen);
            char* const eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char const* const value = eq + 1;
                if (strcmp(line, "id") == 0) {
                    snprintf(out->id, sizeof(out->id), "%s", value);
                    hasId = (out->id[0] != '\0');
                }
                else if (strcmp(line, "memrecId") == 0) {
                    out->memrecId = atoi(value);
                }
                else if (strcmp(line, "description") == 0) {
                    snprintf(out->description, sizeof(out->description), "%s", value);
                }
                else if (strcmp(line, "address") == 0) {
                    snprintf(out->address, sizeof(out->address), "%s", value);
                }
                else if (strcmp(line, "templateCaption") == 0) {
                    snprintf(out->templateCaption, sizeof(out->templateCaption), "%s", value);
                }
                else if (strcmp(line, "format") == 0) {
                    out->format = atoi(value);
                }
            }
        }
        i = (e < len) ? (e + 1) : e;
    }
    return hasId;
}

static bool _CeSession_ParseReply(const char* reply, size_t len, int* outMemrecId, char* err, size_t errCap)
{
    if (!reply || len == 0) {
        return false;
    }
    if (err && errCap > 0) {
        err[0] = '\0';
    }
    if (len >= 3 && strncmp(reply, "OK:", 3) == 0) {
        char num[32] = { 0 };
        size_t const n = (len - 3 < sizeof(num) - 1) ? (len - 3) : (sizeof(num) - 1);
        memcpy(num, reply + 3, n);
        int const id = atoi(num);
        if (outMemrecId) {
            *outMemrecId = id;
        }
        return (id > 0);
    }
    if (len >= 4 && strncmp(reply, "ERR:", 4) == 0) {
        if (err && errCap > 0) {
            size_t const n = (len - 4 < errCap - 1) ? (len - 4) : (errCap - 1);
            memcpy(err, reply + 4, n);
            err[n] = '\0';
        }
        return false;
    }
    return false;
}

static bool _CeSession_MetaPathFromScript(const char* scriptPath, char* out, size_t cap)
{
    static const char suffix[] = ".np3ce";
    if (!scriptPath || !out) {
        return false;
    }
    size_t const n = strlen(scriptPath);
    if (cap < n + sizeof(suffix)) {
        return false;
    }
    memcpy(out, scriptPath, n);
    memcpy(out + n, suffix, sizeof(suffix));
    return true;
}

// Rewrite only the memrecId key of a session sidecar, keeping every other
// line (and their order) byte-identical.  A sidecar without the key gets one
// appended.  Pure: no Win32 calls, so the focused harness can use it.
static bool _CeSession_ReplaceMetaMemrecId(const char* text, size_t len, int id,
                                           char* out, size_t cap)
{
    if (!out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (!text && len > 0) {
        return false;
    }
    char const key[] = "memrecId=";
    size_t const keyLen = (sizeof(key) - 1);
    size_t pos = 0;
    size_t written = 0;
    bool replaced = false;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && text[eol] != '\n') {
            eol++;
        }
        size_t const lineLen = eol - pos;
        size_t const next = (eol < len) ? (eol + 1) : len;
        if (!replaced && lineLen >= keyLen && memcmp(text + pos, key, keyLen) == 0) {
            int const n = snprintf(out + written, cap - written, "%s%d\n", key, id);
            if (n < 0 || (size_t)n >= cap - written) {
                return false;
            }
            written += (size_t)n;
            replaced = true;
        }
        else if (lineLen > 0) {
            if (written + lineLen + 2 > cap) {
                return false;
            }
            memcpy(out + written, text + pos, lineLen);
            written += lineLen;
            out[written++] = '\n';
        }
        pos = next;
    }
    if (!replaced) {
        int const n = snprintf(out + written, cap - written, "%s%d\n", key, id);
        if (n < 0 || (size_t)n >= cap - written) {
            return false;
        }
        written += (size_t)n;
    }
    out[written] = '\0';
    return true;
}

// Read the sidecar, replace its memrecId key and write it back.  The sidecar
// is written only by the editor, so this stays the single writer.
static bool _CeSession_RewriteMetaMemrecId(LPCWSTR metaPathW, int id)
{
    if (!metaPathW || metaPathW[0] == L'\0' || id <= 0) {
        return false;
    }
    char text[8192] = { 0 };
    HANDLE const h = CreateFileW(metaPathW, GENERIC_READ, FILE_SHARE_READ, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD read = 0;
    BOOL const got = ReadFile(h, text, (DWORD)(sizeof(text) - 1), &read, NULL);
    CloseHandle(h);
    if (!got) {
        return false;
    }
    text[read] = '\0';
    char updated[8192] = { 0 };
    if (!_CeSession_ReplaceMetaMemrecId(text, (size_t)read, id, updated, sizeof(updated))) {
        return false;
    }
    size_t const len = strlen(updated);
    HANDLE const w = CreateFileW(metaPathW, GENERIC_WRITE, 0, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    BOOL const put = WriteFile(w, updated, (DWORD)len, &written, NULL);
    CloseHandle(w);
    return put && written == (DWORD)len;
}

// ----------------------------------------------------------------------------
// Win32 / document integration
// ----------------------------------------------------------------------------

static bool _CeSession_ToUtf8(LPCWSTR wide, char* out, size_t cap)
{
    int const n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL);
    return (n > 0);
}

static bool _CeSession_ToWide(const char* utf8, WCHAR* out, size_t cch)
{
    int const n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)cch);
    return (n > 0);
}

// Lightweight step log: if the editor ever looks frozen again, the last line
// of %TEMP%\np3_ce_addlist.log shows exactly which step it stopped at.
static void _CeSession_Log(const char* tag)
{
    char path[MAX_PATH] = { 0 };
    if (GetTempPathA(MAX_PATH, path) == 0 ||
        FAILED(StringCchCatA(path, COUNTOF(path), "np3_ce_addlist.log"))) {
        return;
    }
    FILE* file = NULL;
    if (fopen_s(&file, path, "a") == 0 && file) {
        fprintf(file, "%lu %s\n", (unsigned long)GetTickCount(), tag);
        fclose(file);
    }
}

// Worker -> UI: only an atomic code plus a payload-free message.  The worker
// never allocates buffers for PostMessage and never touches window handles.
static void _CeSession_Report(HWND hwnd, LONG code)
{
    InterlockedExchange(&s_lastResult, code);
    PostMessageW(hwnd, WM_APP_CE_ADD_RESULT, 0, 0);
}

void CeSession_OnOpened(LPCWSTR scriptPath)
{
    memset(&s_ceSession, 0, sizeof(s_ceSession));
    s_formatOnOpen = false;
    if (!scriptPath || !scriptPath[0]) {
        return;
    }
    char utf8Path[MAX_PATH * 3] = { 0 };
    char metaUtf8[MAX_PATH * 3] = { 0 };
    WCHAR metaWide[MAX_PATH] = { L'\0' };
    if (!_CeSession_ToUtf8(scriptPath, utf8Path, sizeof(utf8Path)) ||
        !_CeSession_MetaPathFromScript(utf8Path, metaUtf8, sizeof(metaUtf8)) ||
        !_CeSession_ToWide(metaUtf8, metaWide, COUNTOF(metaWide))) {
        return;
    }
    HANDLE const file = CreateFileW(metaWide, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;   // ordinary document, no CE session
    }
    char buf[CE_SESSION_META_MAX + 1] = { 0 };
    DWORD read = 0;
    ReadFile(file, buf, CE_SESSION_META_MAX, &read, NULL);
    CloseHandle(file);

    CE_SESSION_META meta = { 0 };
    if (!_CeSession_ParseMeta(buf, read, &meta)) {
        return;
    }
    snprintf(s_ceSession.id, sizeof(s_ceSession.id), "%s", meta.id);
    InterlockedExchange(&s_ceSessionMemrecId, (LONG)meta.memrecId);
    snprintf(s_ceSession.description, sizeof(s_ceSession.description), "%s", meta.description);
    snprintf(s_ceSession.address, sizeof(s_ceSession.address), "%s", meta.address);
    snprintf(s_ceSession.templateCaption, sizeof(s_ceSession.templateCaption), "%s", meta.templateCaption);
    StringCchCopyW(s_ceSession.metaPath, COUNTOF(s_ceSession.metaPath), metaWide);
    s_formatOnOpen = (meta.format != 0);
    s_ceSession.active = true;
}

bool CeSession_ConsumeFormatRequest(void)
{
    bool const wanted = s_formatOnOpen;
    s_formatOnOpen = false;
    return wanted;
}

// Close the active CE session.  blocking = true sends the frame on the calling
// thread and waits (bounded) for CE's reply; that is required while shutting
// down, because the fire-and-forget variant runs on a detached thread which
// the exiting process can kill before the frame is written.
static void _CeSession_CloseSession(bool blocking)
{
    if (!s_ceSession.active) {
        return;
    }
    char payload[96] = { 0 };
    int const n = snprintf(payload, sizeof(payload), "%s", s_ceSession.id);
    if (n > 0) {
        if (blocking) {
            char* reply = NULL;
            DWORD replyLen = 0;
            CeBridge_SendRequestTimed(CE_BRIDGE_CMD_CLOSE_SESSION, payload, (DWORD)n,
                                      &reply, &replyLen,
                                      CE_SESSION_CLOSE_PIPE_WAIT_MS,
                                      CE_SESSION_CLOSE_REPLY_TIMEOUT_MS);
            if (reply) {
                FreeMem(reply);
            }
        }
        else {
            // Never wait for a reply here: this runs while switching documents,
            // where a busy CE main thread must not freeze Notepad3.
            CeBridge_SendNoReply(CE_BRIDGE_CMD_CLOSE_SESSION, payload, (DWORD)n);
        }
    }
    InterlockedExchange(&s_ceSessionMemrecId, 0);
    memset(&s_ceSession, 0, sizeof(s_ceSession));
}

void CeSession_OnClosed(void)
{
    _CeSession_CloseSession(false);
}

// The apply request runs off the UI thread: CE's main-thread synchronize()
// can be delayed while a modal template dialog is open, and a synchronous
// pipe read used to freeze the whole editor (including window close).
static unsigned __stdcall _ApplyThread(void* arg)
{
    CE_SESSION_JOB* const job = (CE_SESSION_JOB*)arg;
    if (!job) {
        InterlockedExchange(&s_ceSessionApplyBusy, 0);
        _endthreadex(0);
        return 0;
    }
    HWND const hwnd = job->hwnd;
    _CeSession_Log("worker_enter");

    char* reply = NULL;
    DWORD replyLen = 0;
    bool const sent = CeBridge_SendRequest(CE_BRIDGE_CMD_APPLY_SCRIPT,
                                           job->payload, job->len, &reply, &replyLen);
    int memrecId = 0;
    bool const applied = sent && reply &&
                         _CeSession_ParseReply(reply, replyLen, &memrecId, NULL, 0);
    _CeSession_Log("worker_reply");

    if (applied) {
        InterlockedExchange(&s_ceSessionMemrecId, (LONG)memrecId);
    }
    if (reply) {
        FreeMem(reply);
    }
    FreeMem(job->payload);
    FreeMem(job);
    InterlockedExchange(&s_ceSessionApplyBusy, 0);
    _CeSession_Report(hwnd, applied ? (LONG)memrecId : -1L);
    _CeSession_Log("worker_done");
    _endthreadex(0);
    return 0;
}

void CeSession_AddCurrentToAddressList(HWND hwnd)
{
    _CeSession_Log("enter");
    if (InterlockedCompareExchange(&s_ceSessionApplyBusy, 0, 0) != 0) {
        DWORD const started = (DWORD)InterlockedCompareExchange(&s_ceSessionApplyStart, 0, 0);
        if ((DWORD)(GetTickCount() - started) < 10000) {
            _CeSession_Report(hwnd, -5L);
            _CeSession_Log("busy");
            return;
        }
        InterlockedExchange(&s_ceSessionApplyBusy, 0);  // stale worker, allow a new request
    }
    InterlockedExchange(&s_ceSessionApplyStart, (LONG)GetTickCount());
    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc) || doc.len <= 0) {
        _CeSession_Report(hwnd, -2L);
        _CeSession_Log("empty_doc");
        return;
    }
    _CeSession_Log("doc_ok");
    if ((size_t)doc.len > CE_BRIDGE_MAX_FRAME - 1024) {
        CeScript_FreeDoc(&doc);
        _CeSession_Report(hwnd, -3L);
        _CeSession_Log("too_large");
        return;
    }
    char* const script = (char*)AllocMem((size_t)doc.len + 1, HEAP_ZERO_MEMORY);
    if (script) {
        memcpy(script, doc.text, (size_t)doc.len);
    }
    DWORD const scriptLen = (DWORD)doc.len;
    CeScript_FreeDoc(&doc);
    if (!script) {
        _CeSession_Report(hwnd, -4L);
        return;
    }

    char sessionId[64] = { 0 };
    if (s_ceSession.active) {
        snprintf(sessionId, sizeof(sessionId), "%s", s_ceSession.id);
    }
    else {
        snprintf(sessionId, sizeof(sessionId), "adhoc-%lu-%lu",
                 (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    }

    size_t const cap = (size_t)scriptLen + 256;
    char* const payload = (char*)AllocMem(cap, HEAP_ZERO_MEMORY);
    size_t payloadLen = 0;
    int const memrecId = (int)InterlockedCompareExchange(&s_ceSessionMemrecId, 0, 0);
    bool const built = payload && _CeSession_BuildApplyPayload(
        payload, cap, sessionId, s_ceSession.active ? memrecId : 0,
        script, (size_t)scriptLen, &payloadLen);
    FreeMem(script);
    if (!built) {
        if (payload) {
            FreeMem(payload);
        }
        _CeSession_Report(hwnd, -4L);
        return;
    }
    _CeSession_Log("payload_built");

    CE_SESSION_JOB* const job = (CE_SESSION_JOB*)AllocMem(sizeof(CE_SESSION_JOB), HEAP_ZERO_MEMORY);
    if (!job) {
        FreeMem(payload);
        _CeSession_Report(hwnd, -4L);
        return;
    }
    job->hwnd = IsWindow(hwnd) ? hwnd : Globals.hwndMain;
    job->payload = payload;
    job->len = (DWORD)payloadLen;

    // UI returns immediately; the worker only stores an atomic result code.
    InterlockedExchange(&s_ceSessionApplyBusy, 1);
    HANDLE const thread = (HANDLE)_beginthreadex(NULL, 0, _ApplyThread, job, 0, NULL);
    if (!thread) {
        FreeMem(payload);
        FreeMem(job);
        InterlockedExchange(&s_ceSessionApplyBusy, 0);
        _CeSession_Report(hwnd, -4L);
        return;
    }
    CloseHandle(thread);
    _CeSession_Log("thread_started");
}

// After a successful save, mirror the saved buffer into the CE address-list
// record.  Uses the same worker-thread pattern as apply so a busy CE main
// thread can never block the save path.  Sync is deliberately silent (no
// result message): a failed sync must never disturb an already-successful
// local save.  The Lua handler updates mr.Script and releases beginEdit.
static unsigned __stdcall _SyncThread(void* arg)
{
    CE_SESSION_JOB* const job = (CE_SESSION_JOB*)arg;
    if (!job) {
        _endthreadex(0);
        return 0;
    }
    char* reply = NULL;
    DWORD replyLen = 0;
    CeBridge_SendRequest(CE_BRIDGE_CMD_SYNC_SCRIPT, job->payload, job->len,
                         &reply, &replyLen);
    // The reply carries the record id the bridge actually used.  A session that
    // had no record yet gets one on its first save, and the new id has to
    // reach the sidecar: the next save would otherwise ask for record 0 again
    // and create a second record.
    int memrecId = 0;
    if (reply && _CeSession_ParseReply(reply, replyLen, &memrecId, NULL, 0)) {
        int const current = (int)InterlockedCompareExchange(&s_ceSessionMemrecId, 0, 0);
        if (memrecId > 0 && memrecId != current) {
            InterlockedExchange(&s_ceSessionMemrecId, (LONG)memrecId);
            if (!_CeSession_RewriteMetaMemrecId(s_ceSession.metaPath, memrecId)) {
                _CeSession_Log("meta_rewrite_failed");
            }
        }
    }
    if (reply) {
        FreeMem(reply);
    }
    FreeMem(job->payload);
    FreeMem(job);
    _endthreadex(0);
    return 0;
}

void CeSession_SyncSavedScript(HWND hwnd)
{
    UNREFERENCED_PARAMETER(hwnd);
    if (!s_ceSession.active || s_ceSession.id[0] == '\0') {
        return;   // ordinary document, no CE session to sync to
    }

    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc) || doc.len <= 0) {
        CeScript_FreeDoc(&doc);
        return;
    }
    if ((size_t)doc.len > CE_BRIDGE_MAX_FRAME - 1024) {
        CeScript_FreeDoc(&doc);
        return;
    }
    char* const script = (char*)AllocMem((size_t)doc.len + 1, HEAP_ZERO_MEMORY);
    if (script) {
        memcpy(script, doc.text, (size_t)doc.len);
    }
    DWORD const scriptLen = (DWORD)doc.len;
    CeScript_FreeDoc(&doc);
    if (!script) {
        return;
    }

    int const memrecId = (int)InterlockedCompareExchange(&s_ceSessionMemrecId, 0, 0);
    size_t const cap = (size_t)scriptLen + 256;
    char* const payload = (char*)AllocMem(cap, HEAP_ZERO_MEMORY);
    size_t payloadLen = 0;
    bool const built = payload && _CeSession_BuildApplyPayload(
        payload, cap, s_ceSession.id, memrecId, script, (size_t)scriptLen, &payloadLen);
    FreeMem(script);
    if (!built) {
        if (payload) {
            FreeMem(payload);
        }
        return;
    }

    CE_SESSION_JOB* const job = (CE_SESSION_JOB*)AllocMem(sizeof(CE_SESSION_JOB), HEAP_ZERO_MEMORY);
    if (!job) {
        FreeMem(payload);
        return;
    }
    job->hwnd = IsWindow(hwnd) ? hwnd : Globals.hwndMain;
    job->payload = payload;
    job->len = (DWORD)payloadLen;

    HANDLE const thread = (HANDLE)_beginthreadex(NULL, 0, _SyncThread, job, 0, NULL);
    if (!thread) {
        FreeMem(payload);
        FreeMem(job);
        return;
    }
    CloseHandle(thread);
}

// Runs on the UI thread in response to WM_APP_CE_ADD_RESULT.  INFO never
// triggers CePanel_AutoShow(), so reporting cannot create/relayout the panel.
void CeSession_ReportResult(HWND hwnd)
{
    LONG const code = InterlockedExchange(&s_lastResult, 0);
    WCHAR msg[320] = { L'\0' };
    switch (code) {
    case -1:
        StringCchPrintfW(msg, COUNTOF(msg),
            L"--- 写入 CE 地址列表失败 / Failed: 未连接 CE 桥接或请求超时（请启动/重启 Cheat Engine）---");
        break;
    case -2:
        StringCchPrintfW(msg, COUNTOF(msg), L"--- 当前文档为空，无法添加到 CE 地址列表 ---");
        break;
    case -3:
        StringCchPrintfW(msg, COUNTOF(msg), L"--- 脚本文档过大，无法发送到 CE ---");
        break;
    case -4:
        StringCchPrintfW(msg, COUNTOF(msg), L"--- 无法创建写入请求 ---");
        break;
    case -5:
        StringCchPrintfW(msg, COUNTOF(msg), L"--- 上一次写入仍在进行中（最多等待 3 秒）---");
        break;
    default:
        if (code <= 0) {
            return;
        }
        StringCchPrintfW(msg, COUNTOF(msg),
            L"--- 已写入 CE 当前地址列表 / Added to CE address list (记录 #%ld；未保存 .CT 文件) ---",
            (long)code);
        break;
    }
    CeOutput_Append(hwnd, CE_BRIDGE_INFO, msg);
    _CeSession_Log("result_shown");
}

void CeSession_Destroy(void)
{
    // Shutdown: the close frame must reach CE before the process exits, so
    // send it synchronously instead of relying on a detached worker thread.
    _CeSession_CloseSession(true);
}

bool CeSession_IsActive(void)
{
    return s_ceSession.active;
}
