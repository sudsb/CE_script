// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeBridge.c                                                                  *
*   Route script runs: pure Lua executes in the local Lua runtime (CeLua.c),  *
*   CE-related scripts discover a Cheat Engine installation, deploy           *
*   ce_bridge/np3_ce_bridge.lua into <CE>\autorun and talk to it over a      *
*   named pipe.  The CE script itself runs inside CE's Lua state, so          *
*   read/write of the debugged process, breakpoints/hooks and disassembly     *
*   use the native CE API; this module only transports source and output.      *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"
#include <process.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <strsafe.h>
#include <string.h>
#include <tlhelp32.h>
#include <wchar.h>

#include "Config/Config.h"
#include "TypeDefs.h"
#include "Notepad3.h"
#include "MuiLanguage.h"
#include "SciCall.h"
#include "CeScript.h"
#include "CeOutput.h"
#include "CeBridge.h"
#include "CeLint.h"
#include "CeLua.h"
#include "..\language\common_res.h"

#define CE_BRIDGE_PIPE_W       L"\\\\.\\pipe\\NP3_CE_BRIDGE"
#define CE_BRIDGE_LUA_FILE     L"autorun\\np3_ce_bridge.lua"
#define CE_BRIDGE_WAIT_MS      2000
#define CE_BRIDGE_REPLY_TIMEOUT_MS 3000
#define CE_BRIDGE_NOREPLY_WAIT_MS 250
#define CE_BRIDGE_SHUTDOWN_WAIT_MS 3000

static const char s_errNoPipe[]  = "Cannot connect to the CE bridge pipe. Start (or restart) Cheat Engine so autorun\\np3_ce_bridge.lua gets loaded.";
static const char s_errOpen[]    = "Cannot open the CE bridge pipe.";
static const char s_errWrite[]   = "Cannot send the script to Cheat Engine.";
static const char s_errLost[]    = "The connection to Cheat Engine was lost.";
static const char s_errFrame[]   = "Malformed frame received from the CE bridge.";

static volatile LONG s_ceBridgeRunning = 0;
static volatile LONG s_ceBridgeWatchRun = 0;
static volatile LONG s_ceBridgeWatchCancel = 0;
static volatile LONG s_ceBridgeWatchArm = 0;
// Non-zero while an AA script executed from Notepad3 is enabled in Cheat
// Engine (CE-side B.aaState entry exists).  Updated from the AA run result and
// from the watch poll, so exiting can offer to run the [DISABLE] section.
static volatile LONG s_ceBridgeAaActive = 0;

// The AA script enabled by Notepad3 belongs to one document.  Keep the key and
// the script text from the enabling moment: the document may be replaced or
// closed before the user cancels, and its current text is no longer available.
typedef struct _CE_AA_OWNER {
    char  key[17];
    char* script;
    DWORD len;
} CE_AA_OWNER;
static CE_AA_OWNER s_aaOwner = { { 0 }, NULL, 0 };

typedef struct _CE_BRIDGE_JOB {
    HWND        hwnd;
    char*       script;   // UTF-8, NUL terminated
    DWORD       len;
    CE_RUN_MODE mode;     // local Lua runtime vs. CE named-pipe bridge
    bool        aa;       // AA run: RESULT '1'/'0' means enabled/disabled
} CE_BRIDGE_JOB;

// ----------------------------------------------------------------------------
// Pure protocol / path helpers.  These are also exercised by
// test/TestCeBridge.ps1, so keep them free of Win32 dependencies.
// ----------------------------------------------------------------------------

// Wrap-around safe elapsed-time check for GetTickCount()-based deadlines.
static bool _CeBridge_DeadlineReached(DWORD now, DWORD start, DWORD timeoutMs)
{
    return (DWORD)(now - start) >= timeoutMs;
}

// Parse one CE check record "<line>|<e|w>|<message>".  Pure helper (no Win32)
// so test/TestCeSession.cpp can exercise it directly.
static bool _CeBridge_ParseCheckRecord(const char* rec, size_t len,
                                       int* lineOut, bool* errorOut,
                                       char* msgOut, size_t msgCap)
{
    if (!rec || len == 0 || !lineOut || !errorOut || !msgOut || msgCap == 0) {
        return false;
    }
    size_t p1 = 0;
    while (p1 < len && rec[p1] != '|') {
        ++p1;
    }
    if (p1 == 0 || p1 >= len) {
        return false;
    }
    size_t p2 = p1 + 1;
    while (p2 < len && rec[p2] != '|') {
        ++p2;
    }
    if (p2 >= len) {
        return false;
    }
    int line = 0;
    for (size_t i = 0; i < p1; ++i) {
        if (rec[i] < '0' || rec[i] > '9') {
            return false;
        }
        line = line * 10 + (rec[i] - '0');
    }
    if (line <= 0) {
        return false;
    }
    size_t const avail = len - (p2 + 1);
    size_t const n = (avail < msgCap - 1) ? avail : (msgCap - 1);
    for (size_t i = 0; i < n; ++i) {
        msgOut[i] = rec[p2 + 1 + i];
    }
    msgOut[n] = '\0';
    *lineOut = line;
    *errorOut = (rec[p1 + 1] == 'e' || rec[p1 + 1] == 'E');
    return true;
}

static DWORD _CeBridge_DecodeLen(const BYTE* p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static void _CeBridge_EncodeLen(BYTE* p, DWORD len)
{
    p[0] = (BYTE)(len & 0xFF);
    p[1] = (BYTE)((len >> 8) & 0xFF);
    p[2] = (BYTE)((len >> 16) & 0xFF);
    p[3] = (BYTE)((len >> 24) & 0xFF);
}

static bool _CeBridge_EncodeFrame(BYTE* dst, size_t cap, BYTE cmd, const char* payload, DWORD len, size_t* outLen)
{
    if (!dst || !outLen || (len > 0 && !payload)) {
        return false;
    }
    if (len > CE_BRIDGE_MAX_FRAME) {
        return false;
    }
    size_t const need = 5 + (size_t)len;
    if (cap < need) {
        return false;
    }
    dst[0] = cmd;
    _CeBridge_EncodeLen(dst + 1, len);
    if (len > 0) {
        memcpy(dst + 5, payload, (size_t)len);
    }
    *outLen = need;
    return true;
}

// Only CE-specific directives identify a marker-less AA script.  Common Lua
// names such as assert/label/define must not turn a Lua document into an AA run.
static bool _CeBridge_IsAaDeclaration(const char* p, size_t len)
{
    static const char* const words[] = {
        "alloc(", "globalalloc(", "sharedalloc(", "dealloc(",
        "registersymbol(", "unregistersymbol(", "aobscan(", "aobscanmodule(",
        "aobscanregion(", "aobscanpattern(", "readmem(", "loadbinary(",
        "createthread(", "fullaccess(", "kalloc("
    };
    if (!p) {
        return false;
    }
    while ((len > 0) && ((*p == ' ') || (*p == '\t'))) {
        ++p;
        --len;
    }
    if ((len == 0) || (*p == '-') || (*p == '/') || (*p == ';') || (*p == '[') || (*p == '{')) {
        return false;
    }
    for (size_t i = 0; i < COUNTOF(words); ++i) {
        size_t const n = strlen(words[i]);
        if (len < n) {
            continue;
        }
        bool match = true;
        for (size_t k = 0; k < n; ++k) {
            char const ch = p[k];
            char const lower = ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + ('a' - 'A')) : ch;
            if (lower != words[i][k]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static bool _CeBridge_AaNameEqualsCI(const char* a, const char* b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if ((ca >= 'A') && (ca <= 'Z')) {
            ca = (char)(ca + ('a' - 'A'));
        }
        if ((cb >= 'A') && (cb <= 'Z')) {
            cb = (char)(cb + ('a' - 'A'));
        }
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

// Collect the user-defined data symbols an AA script declares.  "newmem" is
// CE's built-in code cave and label() names are code addresses, so neither is
// a value to watch.  No Win32 dependencies (see TestCeBridge.ps1).
static int _CeBridge_CollectAaSymbols(const char* script, size_t len, char out[][64], int max)
{
    static const char* const calls[] = {
        "alloc(", "globalalloc(", "sharedalloc(", "allocnx(", "allocxo(",
        "define(", "registersymbol(", "aobscan(", "aobscanmodule(",
        "aobscanregion(", "aobscanpattern(", "kalloc("
    };
    if (!script || !out || (max <= 0)) {
        return 0;
    }
    int count = 0;
    size_t pos = 0;
    while ((pos < len) && (count < max)) {
        size_t end = pos;
        while ((end < len) && (script[end] != '\n')) {
            ++end;
        }
        const char* p = script + pos;
        size_t n = end - pos;
        while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == '\r'))) {
            ++p;
            --n;
        }
        bool const skip = (n == 0) || (*p == ';') || (*p == '{') || (*p == '[') ||
            ((n > 1) && ((*p == '-' && p[1] == '-') || (*p == '/' && p[1] == '/')));
        if (!skip) {
            for (size_t i = 0; i < COUNTOF(calls); ++i) {
                size_t const m = strlen(calls[i]);
                if (n < m) {
                    continue;
                }
                bool match = true;
                for (size_t k = 0; k < m; ++k) {
                    char const ch = p[k];
                    char const lower = ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + ('a' - 'A')) : ch;
                    if (lower != calls[i][k]) {
                        match = false;
                        break;
                    }
                }
                if (!match) {
                    continue;
                }
                const char* q = p + m;
                size_t left = n - m;
                while ((left > 0) && ((*q == ' ') || (*q == '\t') || (*q == '\'') || (*q == '"'))) {
                    ++q;
                    --left;
                }
                char name[64] = { '\0' };
                size_t k = 0;
                while ((left > 0) && (k < sizeof(name) - 1)) {
                    char const ch = *q;
                    bool const word = ((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z')) ||
                        ((ch >= '0') && (ch <= '9')) || (ch == '_') || (ch == '.') || (ch == '`');
                    if (!word) {
                        break;
                    }
                    name[k++] = ch;
                    ++q;
                    --left;
                }
                name[k] = '\0';
                if ((k > 0) && !_CeBridge_AaNameEqualsCI(name, "newmem")) {
                    bool duplicate = false;
                    for (int j = 0; j < count; ++j) {
                        if (_CeBridge_AaNameEqualsCI(name, out[j])) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        memcpy(out[count], name, k + 1);
                        ++count;
                    }
                }
                break;
            }
        }
        pos = (end < len) ? (end + 1) : len;
    }
    return count;
}

// Lua long strings end at ]=*] with exactly the same number of '=' as the
// opener, so pick one more than the longest such sequence in the AA script.
static size_t _CeBridge_AaBracketLevel(const char* script, size_t len)
{
    size_t level = 0;
    if (!script) {
        return level;
    }
    for (size_t i = 0; i < len; ++i) {
        if (script[i] != ']') {
            continue;
        }
        size_t n = 0;
        while ((i + 1 + n < len) && (script[i + 1 + n] == '=')) {
            ++n;
        }
        if ((i + 1 + n < len) && (script[i + 1 + n] == ']') && (n + 1 > level)) {
            level = n + 1;
        }
    }
    return level;
}

// ----------------------------------------------------------------------------
// Manual watch directives (AA comment lines, ignored by Cheat Engine):
//   // np3watch <name> [type] [format] [count] [if <cond>]
//   // np3watch begin [if <cond>] ... // np3watch end
//   // np3unwatch <name>    (also np3nowatch)
//   // np3watch off | on    (disable/enable the automatic symbol watch)
// Types: auto byte word dword qword pointer float double string wstring array
//        ("reg" is set automatically for CPU register names)
// Formats: auto hex dec bin (decimal/hexadecimal/binary accepted as aliases)
// Conditions: "<lhs> <op> <rhs>" with op one of == != > < >= <=; lhs/rhs is a
//   number (decimal / 0x.. / $..) or a register or symbol name.  Watch lines
//   inside a begin/end block inherit the block condition (joined with " and ").
// ----------------------------------------------------------------------------

#define CE_BRIDGE_MAX_WATCHES   64
#define CE_BRIDGE_WATCH_NAME    64
#define CE_BRIDGE_WATCH_TYPE    16
#define CE_BRIDGE_WATCH_FMT     8
#define CE_BRIDGE_WATCH_COND   256
#define CE_BRIDGE_WATCH_BLOCK   8
#define CE_BRIDGE_WATCH_COUNT   1024

static bool _CeBridge_WatchTokenIs(const char* p, size_t len, const char* lower)
{
    size_t const n = strlen(lower);
    if (!p || (len != n)) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        char const ch = p[i];
        char const lc = ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + ('a' - 'A')) : ch;
        if (lc != lower[i]) {
            return false;
        }
    }
    return true;
}

static bool _CeBridge_WatchTokenIn(const char* p, size_t len, const char* const* words)
{
    for (size_t i = 0; words[i]; ++i) {
        if (_CeBridge_WatchTokenIs(p, len, words[i])) {
            return true;
        }
    }
    return false;
}

static bool _CeBridge_IsWatchType(const char* p, size_t len)
{
    static const char* const types[] = {
        "auto", "byte", "word", "dword", "qword", "pointer", "float", "double",
        "string", "wstring", "array", "u8", "u16", "u32", "u64", "i8", "i16",
        "i32", "i64", "ptr", "f32", "f64", "str", "wstr", "arr", NULL
    };
    return _CeBridge_WatchTokenIn(p, len, types);
}

static bool _CeBridge_IsWatchFormat(const char* p, size_t len)
{
    static const char* const formats[] = {
        "bin", "dec", "hex", "binary", "decimal", "hexadecimal", NULL
    };
    return _CeBridge_WatchTokenIn(p, len, formats);
}

static bool _CeBridge_IsWatchNameChar(char ch)
{
    return ((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z')) ||
        ((ch >= '0') && (ch <= '9')) || (ch == '_') || (ch == '.') ||
        (ch == '`') || (ch == '@');
}

// Recognise a CPU register name (exact lowercase).  base/mask/shift derive any
// alias (eax/al/ah/r8d/sil...) from the canonical getRegisters() entry so the
// CE side only ever looks up the 64-bit (or 32-bit) key.
static bool _CeBridge_WatchRegister(const char* name, char base[8],
                                    unsigned long long* mask, int* shift)
{
    static const char* const exact[] = {
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "eflags", NULL
    };
    static const char* const tail32[] = { "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", NULL };
    if (!name || !base || !mask || !shift) {
        return false;
    }
    size_t const n = strlen(name);
    if ((n == 0) || (n > 6)) {
        return false;
    }
    *mask = ~(unsigned long long)0;
    *shift = 0;
    for (size_t i = 0; exact[i]; ++i) {
        if (strcmp(name, exact[i]) == 0) {
            memcpy(base, name, n + 1);
            return true;
        }
    }
    if (strcmp(name, "flags") == 0) {
        memcpy(base, "eflags", 7);
        return true;
    }
    if ((n == 3) && (name[0] == 'e')) {
        if (strcmp(name + 1, "ip") == 0) {
            memcpy(base, "rip", 4);
            *mask = 0xFFFFFFFFull;
            return true;
        }
        for (size_t i = 0; tail32[i]; ++i) {
            if (strcmp(name + 1, tail32[i]) == 0) {
                base[0] = 'r';
                memcpy(base + 1, name + 1, 3);
                *mask = 0xFFFFFFFFull;
                return true;
            }
        }
    }
    if ((n == 2) && (((name[1] == 'x') && (strchr("abcd", name[0]) != NULL)) ||
                     ((name[1] == 'i') && ((name[0] == 's') || (name[0] == 'd'))) ||
                     ((name[1] == 'p') && ((name[0] == 'b') || (name[0] == 's'))))) {
        // 16-bit: ax bx cx dx si di bp sp
        base[0] = 'r';
        base[1] = name[0];
        base[2] = name[1];
        base[3] = '\0';
        *mask = 0xFFFFull;
        return true;
    }
    if ((n == 2) && (strchr("abcd", name[0]) != NULL)) {
        if ((name[1] == 'l') || (name[1] == 'h')) {
            // 8-bit: al bl cl dl / ah bh ch dh
            base[0] = 'r';
            base[1] = name[0];
            base[2] = 'x';
            base[3] = '\0';
            *mask = 0xFFull;
            *shift = (name[1] == 'h') ? 8 : 0;
            return true;
        }
    }
    if ((n == 3) && (name[2] == 'l') &&
        (((name[1] == 'i') && ((name[0] == 's') || (name[0] == 'd'))) ||
         ((name[1] == 'p') && ((name[0] == 'b') || (name[0] == 's'))))) {
        // 8-bit: sil dil bpl spl
        base[0] = 'r';
        base[1] = name[0];
        base[2] = name[1];
        base[3] = '\0';
        *mask = 0xFFull;
        return true;
    }
    if ((n >= 3) && (name[0] == 'r') && ((name[n - 1] == 'd') || (name[n - 1] == 'b'))) {
        int idx = 0;
        bool digits = true;
        for (size_t i = 1; i + 1 < n; ++i) {
            if ((name[i] < '0') || (name[i] > '9')) {
                digits = false;
                break;
            }
            idx = (idx * 10) + (int)(name[i] - '0');
        }
        if (digits && (idx >= 8) && (idx <= 15)) {
            snprintf(base, 8, "r%d", idx);
            *mask = (name[n - 1] == 'd') ? 0xFFFFFFFFull : 0xFFull;
            return true;
        }
    }
    return false;
}

static bool _CeBridge_IsCondSpace(char ch)
{
    return (ch == ' ') || (ch == '\t') || (ch == '\r');
}

// Translate a watch condition into a Lua expression.  The condition is not
// parsed here: anything Lua understands is passed through, so multi-condition
// tests ("a == 3 || b != 4"), parentheses, calls and table fields all work.
// Guards only: "||"/"&&"/"!" and the CE "$hex" literal are rewritten, adjacent
// blanks are collapsed, quotes must terminate and parentheses must balance.
static bool _CeBridge_ParseWatchCondition(const char* p, size_t n, char* out, size_t cap)
{
    if (!p || !out || (cap < 2)) {
        return false;
    }
    out[0] = '\0';
    size_t w = 0;
    int depth = 0;
    char quote = '\0';
    size_t i = 0;
    while ((i < n) && _CeBridge_IsCondSpace(p[i])) {
        ++i;
    }
    while (i < n) {
        char const ch = p[i];
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            }
            if (w + 2 > cap) {
                return false;
            }
            out[w++] = ch;
            ++i;
            continue;
        }
        if ((ch == '\'') || (ch == '"')) {
            quote = ch;
            out[w++] = ch;
            ++i;
            continue;
        }
        const char* rep = NULL;
        size_t repLen = 0;
        size_t step = 1;
        if ((ch == '|') && (i + 1 < n) && (p[i + 1] == '|')) {
            rep = " or ";
            repLen = 4;
            step = 2;
        }
        else if ((ch == '&') && (i + 1 < n) && (p[i + 1] == '&')) {
            rep = " and ";
            repLen = 5;
            step = 2;
        }
        else if ((ch == '!') && (i + 1 < n) && (p[i + 1] == '=')) {
            rep = "~=";
            repLen = 2;
            step = 2;
        }
        else if (ch == '!') {
            rep = " not ";
            repLen = 5;
            step = 1;
        }
        else if (ch == '$') {
            // CE style hex literal -> Lua 0x
            size_t k = i + 1;
            while ((k < n) && (((p[k] >= '0') && (p[k] <= '9')) ||
                               ((p[k] >= 'a') && (p[k] <= 'f')) || ((p[k] >= 'A') && (p[k] <= 'F')))) {
                ++k;
            }
            if (k > i + 1) {
                if (w + 3 > cap) {
                    return false;
                }
                out[w++] = '0';
                out[w++] = 'x';
                i += 1;
                continue;   // hex digits are copied one by one below
            }
        }
        else if (ch == '(') {
            ++depth;
        }
        else if (ch == ')') {
            if (depth <= 0) {
                return false;
            }
            --depth;
        }
        if (rep) {
            const char* repText = rep;
            size_t repCopy = repLen;
            if ((w > 0) && (out[w - 1] == ' ') && (repText[0] == ' ')) {
                ++repText;
                --repCopy;
            }
            if (w + repCopy + 1 > cap) {
                return false;
            }
            memcpy(out + w, repText, repCopy);
            w += repCopy;
            i += step;
            continue;
        }
        if (_CeBridge_IsCondSpace(ch)) {
            if ((w > 0) && (out[w - 1] != ' ')) {
                if (w + 2 > cap) {
                    return false;
                }
                out[w++] = ' ';
            }
            ++i;
            continue;
        }
        if (w + 2 > cap) {
            return false;
        }
        out[w++] = ch;
        ++i;
    }
    if ((quote != '\0') || (depth != 0)) {
        out[0] = '\0';   // drop the partially copied text
        return false;
    }
    while ((w > 0) && (out[w - 1] == ' ')) {
        --w;
    }
    out[w] = '\0';
    return (w > 0);
}

// Validate the tokens after the name of a block-scoped shorthand entry
// ("// rcx hex 4 if rcx != 0").  Unknown words mean the line is an ordinary AA
// comment, so it must not turn into a watch.
static bool _CeBridge_IsWatchSpecRest(const char* p, size_t n)
{
    while (n > 0) {
        while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == ',') || (*p == '\r'))) {
            ++p;
            --n;
        }
        size_t t = 0;
        while ((t < n) && (p[t] != ' ') && (p[t] != '\t') && (p[t] != ',') && (p[t] != '\r')) {
            ++t;
        }
        if (t == 0) {
            break;
        }
        if (_CeBridge_WatchTokenIs(p, t, "if")) {
            return true;
        }
        bool digits = true;
        for (size_t i = 0; i < t; ++i) {
            if ((p[i] < '0') || (p[i] > '9')) {
                digits = false;
                break;
            }
        }
        if (!digits && !_CeBridge_IsWatchType(p, t) && !_CeBridge_IsWatchFormat(p, t)) {
            return false;
        }
        p += t;
        n -= t;
    }
    return true;
}

// Join every active block condition plus the line's own condition with " and ".
static void _CeBridge_JoinWatchCond(char block[][CE_BRIDGE_WATCH_COND], int depth,
                                    const char* own, char* out, size_t cap)
{
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < depth; ++i) {
        if (block[i][0] == '\0') {
            continue;
        }
        size_t const need = strlen(block[i]) + (used ? 5u : 0u) + 1u;
        if ((used + need) > cap) {
            return;
        }
        if (used) {
            memcpy(out + used, " and ", 5);
            used += 5;
        }
        memcpy(out + used, block[i], strlen(block[i]));
        used += strlen(block[i]);
        out[used] = '\0';
    }
    if (own && own[0]) {
        size_t const ownLen = strlen(own);
        size_t const need = ownLen + (used ? 5u : 0u) + 1u;
        if ((used + need) <= cap) {
            if (used) {
                memcpy(out + used, " and ", 5);
                used += 5;
            }
            memcpy(out + used, own, ownLen);
            used += ownLen;
            out[used] = '\0';
        }
    }
}

static bool _CeBridge_IsIdentChar(char ch)
{
    return ((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z')) ||
        ((ch >= '0') && (ch <= '9')) || (ch == '_');
}

// Is the leading word of [p, n) exactly |word| (case-insensitive)?
static bool _CeBridge_LineWordIs(const char* p, size_t n, const char* word, size_t* used)
{
    size_t const len = strlen(word);
    if (!p || (n < len)) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char const ch = p[i];
        char const lc = ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + ('a' - 'A')) : ch;
        if (lc != word[i]) {
            return false;
        }
    }
    if ((n > len) && _CeBridge_IsIdentChar(p[len])) {
        return false;
    }
    if (used) {
        *used = len;
    }
    return true;
}

static void _CeBridge_SetWatchType(char out[CE_BRIDGE_WATCH_TYPE], const char* value)
{
    size_t i = 0;
    for (; value && value[i] && (i + 1 < CE_BRIDGE_WATCH_TYPE); ++i) {
        out[i] = value[i];
    }
    out[i] = '\0';
}

// Classify a define() literal: quoted text, floating-point literal or an
// integer literal whose width selects dword/qword.
static bool _CeBridge_ClassifyLiteral(const char* p, size_t n, char type[CE_BRIDGE_WATCH_TYPE])
{
    while ((n > 0) && ((p[n - 1] == ' ') || (p[n - 1] == '\t') || (p[n - 1] == '\r'))) {
        --n;
    }
    if ((n >= 2) && ((p[0] == '"') || (p[0] == '\''))) {
        _CeBridge_SetWatchType(type, "string");
        return true;
    }
    if (n == 0) {
        return false;
    }
    bool digits = false;
    bool dot = false;
    bool exp = false;
    for (size_t i = 0; i < n; ++i) {
        if ((p[i] >= '0') && (p[i] <= '9')) {
            digits = true;
        }
        else if (p[i] == '.') {
            dot = true;
        }
        else if ((p[i] == 'e') || (p[i] == 'E')) {
            exp = true;
        }
    }
    if (digits && (dot || exp)) {
        _CeBridge_SetWatchType(type, "double");
        return true;
    }
    size_t start = 0;
    if ((n > 1) && (p[0] == '0') && ((p[1] == 'x') || (p[1] == 'X'))) {
        start = 2;
    }
    else if ((n > 0) && (p[0] == '$')) {
        start = 1;
    }
    if (start > 0) {
        size_t significant = 0;
        bool hex = (n > start);
        for (size_t i = start; i < n; ++i) {
            char const ch = p[i];
            bool const isHex = ((ch >= '0') && (ch <= '9')) || ((ch >= 'a') && (ch <= 'f')) ||
                ((ch >= 'A') && (ch <= 'F'));
            if (!isHex) {
                hex = false;
                break;
            }
            if ((ch != '0') || (significant > 0)) {
                significant = i - start + 1;
            }
        }
        if (hex) {
            _CeBridge_SetWatchType(type, (significant > 8) ? "qword" : "dword");
            return true;
        }
        return false;
    }
    unsigned long long value = 0;
    bool decimal = (n > 0);
    for (size_t i = 0; i < n; ++i) {
        if ((p[i] < '0') || (p[i] > '9')) {
            decimal = false;
            break;
        }
        value = (value * 10ull) + (unsigned long long)(p[i] - '0');
        if (value > 0xFFFFFFFFull) {
            value = 0x100000000ull;
        }
    }
    if (decimal) {
        _CeBridge_SetWatchType(type, (value > 0xFFFFFFFFull) ? "qword" : "dword");
        return true;
    }
    return false;
}

// Infer an object type from AA declarations so a watch line can omit it:
// define() literal (highest priority), `name: db|dw|dd|dq`, alloc size.
// Returns count for alloc(name, N>8) so large buffers display as bytes.
static void _CeBridge_InferSymbolType(const char* script, size_t len, const char* name,
                                      char type[CE_BRIDGE_WATCH_TYPE], int* count)
{
    type[0] = '\0';
    if (count) {
        *count = 0;
    }
    size_t const nameLen = name ? strlen(name) : 0;
    if (!script || (nameLen == 0)) {
        return;
    }
    int best = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while ((end < len) && (script[end] != '\n')) {
            ++end;
        }
        const char* p = script + pos;
        size_t n = end - pos;
        while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == '\r'))) {
            ++p;
            --n;
        }
        bool const comment = ((n >= 2) && (p[0] == '/') && (p[1] == '/')) ||
            ((n >= 1) && ((p[0] == ';') || (p[0] == '{')));
        if (!comment && (n > nameLen) && (p[nameLen] == ':') &&
            _CeBridge_WatchTokenIs(p, nameLen, name)) {
            const char* q = p + nameLen + 1;
            size_t m = n - nameLen - 1;
            while ((m > 0) && ((*q == ' ') || (*q == '\t'))) {
                ++q;
                --m;
            }
            char inferred[CE_BRIDGE_WATCH_TYPE] = { '\0' };
            if (_CeBridge_LineWordIs(q, m, "db", NULL)) {
                _CeBridge_SetWatchType(inferred, "byte");
            }
            else if (_CeBridge_LineWordIs(q, m, "dw", NULL)) {
                _CeBridge_SetWatchType(inferred, "word");
            }
            else if (_CeBridge_LineWordIs(q, m, "dd", NULL)) {
                _CeBridge_SetWatchType(inferred, "dword");
            }
            else if (_CeBridge_LineWordIs(q, m, "dq", NULL)) {
                _CeBridge_SetWatchType(inferred, "qword");
            }
            if (inferred[0] && (best < 2)) {
                _CeBridge_SetWatchType(type, inferred);
                best = 2;
            }
        }
        else if (!comment) {
            size_t wordLen = 0;
            bool const isAlloc = _CeBridge_LineWordIs(p, n, "alloc", &wordLen) ||
                _CeBridge_LineWordIs(p, n, "kalloc", &wordLen) ||
                _CeBridge_LineWordIs(p, n, "globalalloc", &wordLen) ||
                _CeBridge_LineWordIs(p, n, "sharedalloc", &wordLen) ||
                _CeBridge_LineWordIs(p, n, "allocnx", &wordLen) ||
                _CeBridge_LineWordIs(p, n, "allocxo", &wordLen);
            bool const isDefine = _CeBridge_LineWordIs(p, n, "define", &wordLen);
            if (isAlloc || isDefine) {
                const char* q = p + wordLen;
                size_t m = n - wordLen;
                while ((m > 0) && ((*q == ' ') || (*q == '\t'))) {
                    ++q;
                    --m;
                }
                if ((m == 0) || (*q != '(')) {
                    q = NULL;
                }
                if (q) {
                    ++q;
                    --m;
                    while ((m > 0) && ((*q == ' ') || (*q == '\t'))) {
                        ++q;
                        --m;
                    }
                    size_t argLen = 0;
                    while ((argLen < m) && _CeBridge_IsWatchNameChar(q[argLen])) {
                        ++argLen;
                    }
                    if ((argLen > 0) && _CeBridge_WatchTokenIs(q, argLen, name)) {
                        q += argLen;
                        m -= argLen;
                        while ((m > 0) && ((*q == ' ') || (*q == '\t'))) {
                            ++q;
                            --m;
                        }
                        if ((m > 0) && (*q == ',')) {
                            ++q;
                            --m;
                            while ((m > 0) && ((*q == ' ') || (*q == '\t'))) {
                                ++q;
                                --m;
                            }
                            size_t valueLen = 0;
                            while ((valueLen < m) && (q[valueLen] != ')') &&
                                (q[valueLen] != '\r')) {
                                ++valueLen;
                            }
                            if (isDefine) {
                                char inferred[CE_BRIDGE_WATCH_TYPE] = { '\0' };
                                if (_CeBridge_ClassifyLiteral(q, valueLen, inferred) &&
                                    (best < 3)) {
                                    _CeBridge_SetWatchType(type, inferred);
                                    best = 3;
                                    if (count) {
                                        *count = 0;
                                    }
                                }
                            }
                            else if (best < 1) {
                                int size = 0;
                                bool digits = (valueLen > 0);
                                for (size_t i = 0; i < valueLen; ++i) {
                                    if ((q[i] < '0') || (q[i] > '9')) {
                                        digits = false;
                                        break;
                                    }
                                    size = (size * 10) + (int)(q[i] - '0');
                                    if (size > CE_BRIDGE_WATCH_COUNT) {
                                        size = CE_BRIDGE_WATCH_COUNT;
                                        break;
                                    }
                                }
                                if (digits) {
                                    const char* inferred = "byte";
                                    if (size == 2) {
                                        inferred = "word";
                                    }
                                    else if (size == 4) {
                                        inferred = "dword";
                                    }
                                    else if (size == 8) {
                                        inferred = "qword";
                                    }
                                    _CeBridge_SetWatchType(type, inferred);
                                    best = 1;
                                    if (count) {
                                        *count = (size > 8) ? size : 0;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        pos = (end < len) ? (end + 1) : len;
    }
}

// Collect the manual watch directives.  Later lines override earlier ones with
// the same name; `autoWatch` starts at 1 and is cleared by `np3watch off`.
// No Win32 dependencies (see TestCeBridge.ps1).
static int _CeBridge_CollectWatchDirectives(const char* script, size_t len,
                                            char names[][CE_BRIDGE_WATCH_NAME],
                                            char types[][CE_BRIDGE_WATCH_TYPE],
                                            char formats[][CE_BRIDGE_WATCH_FMT],
                                            char conds[][CE_BRIDGE_WATCH_COND],
                                            int counts[], int excludes[], int regs[],
                                            int max, int* autoWatch)
{
    char blockConds[CE_BRIDGE_WATCH_BLOCK][CE_BRIDGE_WATCH_COND] = { { '\0' } };
    int depth = 0;
    if (autoWatch) {
        *autoWatch = 1;
    }
    if (!script || !names || !types || !formats || !conds || !counts || !excludes ||
        !regs || (max <= 0)) {
        return 0;
    }
    int count = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while ((end < len) && (script[end] != '\n')) {
            ++end;
        }
        const char* p = script + pos;
        size_t n = end - pos;
        while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == '\r'))) {
            ++p;
            --n;
        }
        if ((n >= 2) && (p[0] == '/') && (p[1] == '/')) {
            p += 2;
            n -= 2;
        }
        else if ((n >= 1) && (p[0] == ';')) {
            p += 1;
            n -= 1;
        }
        else {
            pos = (end < len) ? (end + 1) : len;
            continue;
        }
        while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == '\r'))) {
            ++p;
            --n;
        }
        size_t dlen = 0;
        while ((dlen < n) && !((p[dlen] == ' ') || (p[dlen] == '\t') || (p[dlen] == ',') || (p[dlen] == '\r'))) {
            ++dlen;
        }
        bool isWatch = _CeBridge_WatchTokenIs(p, dlen, "np3watch");
        bool isExclude = _CeBridge_WatchTokenIs(p, dlen, "np3unwatch") ||
            _CeBridge_WatchTokenIs(p, dlen, "np3nowatch");
        bool shorthand = false;
        if (!isWatch && !isExclude) {
            // Inside a begin/end block the "np3watch" prefix may be omitted:
            // "// rcx", "// rcx if rax == 2", nested "// begin ..." and "// end".
            if ((depth > 0) && (dlen > 0)) {
                isWatch = true;
                shorthand = true;
            }
            else {
                pos = (end < len) ? (end + 1) : len;
                continue;
            }
        }
        if (!shorthand) {
            p += dlen;
            n -= dlen;
        }
        while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == ',') || (*p == '\r'))) {
            ++p;
            --n;
        }
        // The first token decides between the auto-watch switch and a name.
        size_t tlen = 0;
        while ((tlen < n) && !((p[tlen] == ' ') || (p[tlen] == '\t') || (p[tlen] == ',') || (p[tlen] == '\r'))) {
            ++tlen;
        }
        if (isWatch && (tlen > 0)) {
            if (_CeBridge_WatchTokenIs(p, tlen, "off") || _CeBridge_WatchTokenIs(p, tlen, "false") ||
                _CeBridge_WatchTokenIs(p, tlen, "disable")) {
                if (autoWatch) {
                    *autoWatch = 0;
                }
                pos = (end < len) ? (end + 1) : len;
                continue;
            }
            if (_CeBridge_WatchTokenIs(p, tlen, "on") || _CeBridge_WatchTokenIs(p, tlen, "true") ||
                _CeBridge_WatchTokenIs(p, tlen, "enable")) {
                if (autoWatch) {
                    *autoWatch = 1;
                }
                pos = (end < len) ? (end + 1) : len;
                continue;
            }
            if (_CeBridge_WatchTokenIs(p, tlen, "begin")) {
                p += tlen;
                n -= tlen;
                char own[CE_BRIDGE_WATCH_COND] = { '\0' };
                while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == ',') || (*p == '\r'))) {
                    ++p;
                    --n;
                }
                size_t k = 0;
                while ((k < n) && !((p[k] == ' ') || (p[k] == '\t') || (p[k] == '\r'))) {
                    ++k;
                }
                if (_CeBridge_WatchTokenIs(p, k, "if")) {
                    p += k;
                    n -= k;
                    while ((n > 0) && ((*p == ' ') || (*p == '\t'))) {
                        ++p;
                        --n;
                    }
                    if (!_CeBridge_ParseWatchCondition(p, n, own, sizeof(own))) {
                        own[0] = '\0';
                    }
                }
                if (depth < CE_BRIDGE_WATCH_BLOCK) {
                    memcpy(blockConds[depth], own, CE_BRIDGE_WATCH_COND);
                    ++depth;
                }
                pos = (end < len) ? (end + 1) : len;
                continue;
            }
            if (_CeBridge_WatchTokenIs(p, tlen, "end")) {
                if (depth > 0) {
                    --depth;
                    blockConds[depth][0] = '\0';
                }
                pos = (end < len) ? (end + 1) : len;
                continue;
            }
        }
        if ((tlen == 0) || (tlen >= CE_BRIDGE_WATCH_NAME) ||
            ((p[0] >= '0') && (p[0] <= '9'))) {
            pos = (end < len) ? (end + 1) : len;
            continue;
        }
        char name[CE_BRIDGE_WATCH_NAME] = { '\0' };
        bool nameOk = true;
        for (size_t i = 0; i < tlen; ++i) {
            if (!_CeBridge_IsWatchNameChar(p[i])) {
                nameOk = false;
                break;
            }
            name[i] = p[i];
        }
        if (!nameOk) {
            pos = (end < len) ? (end + 1) : len;
            continue;
        }
        name[tlen] = '\0';
        p += tlen;
        n -= tlen;
        if (shorthand && !_CeBridge_IsWatchSpecRest(p, n)) {
            pos = (end < len) ? (end + 1) : len;
            continue;
        }

        int isReg = 0;
        {
            char lower[CE_BRIDGE_WATCH_NAME] = { '\0' };
            char base[8] = { '\0' };
            unsigned long long mask = 0;
            int shift = 0;
            for (size_t i = 0; i < tlen; ++i) {
                const char ch = name[i];
                lower[i] = ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + ('a' - 'A')) : ch;
            }
            if (_CeBridge_WatchRegister(lower, base, &mask, &shift)) {
                isReg = 1;
            }
        }
        int slot = -1;
        for (int i = 0; i < count; ++i) {
            if (_CeBridge_AaNameEqualsCI(names[i], name)) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            if (count >= max) {
                pos = (end < len) ? (end + 1) : len;
                continue;
            }
            slot = count;
            types[slot][0] = '\0';
            formats[slot][0] = '\0';
            conds[slot][0] = '\0';
            counts[slot] = 0;
            regs[slot] = 0;
            ++count;
        }
        memcpy(names[slot], name, tlen + 1);
        excludes[slot] = isExclude ? 1 : 0;
        if (isWatch) {
            // a later directive line replaces the earlier type/format/count
            types[slot][0] = '\0';
            formats[slot][0] = '\0';
            conds[slot][0] = '\0';
            counts[slot] = 0;
            regs[slot] = isReg;
            if (isReg) {
                _CeBridge_SetWatchType(types[slot], "reg");
            }
        }
        else {
            conds[slot][0] = '\0';
            regs[slot] = 0;
        }

        char ownCond[CE_BRIDGE_WATCH_COND] = { '\0' };
        while (n > 0) {
            while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == ',') || (*p == '\r'))) {
                ++p;
                --n;
            }
            size_t vlen = 0;
            while ((vlen < n) && !((p[vlen] == ' ') || (p[vlen] == '\t') || (p[vlen] == ',') || (p[vlen] == '\r'))) {
                ++vlen;
            }
            if (vlen == 0) {
                break;
            }
            if (_CeBridge_WatchTokenIs(p, vlen, "if")) {
                p += vlen;
                n -= vlen;
                while ((n > 0) && ((*p == ' ') || (*p == '\t'))) {
                    ++p;
                    --n;
                }
                if (!_CeBridge_ParseWatchCondition(p, n, ownCond, sizeof(ownCond))) {
                    ownCond[0] = '\0';
                }
                break;
            }
            bool digits = true;
            for (size_t i = 0; i < vlen; ++i) {
                if ((p[i] < '0') || (p[i] > '9')) {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                int value = 0;
                for (size_t i = 0; i < vlen; ++i) {
                    value = (value * 10) + (int)(p[i] - '0');
                    if (value > CE_BRIDGE_WATCH_COUNT) {
                        value = CE_BRIDGE_WATCH_COUNT;
                        break;
                    }
                }
                counts[slot] = value;
            }
            else if (_CeBridge_IsWatchType(p, vlen) && !isReg && (types[slot][0] == '\0')) {
                size_t k = 0;
                for (; (k < vlen) && (k + 1 < CE_BRIDGE_WATCH_TYPE); ++k) {
                    char const ch = p[k];
                    types[slot][k] = ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + ('a' - 'A')) : ch;
                }
                types[slot][k] = '\0';
            }
            else if (_CeBridge_IsWatchFormat(p, vlen) && (formats[slot][0] == '\0')) {
                size_t k = 0;
                for (; (k < vlen) && (k + 1 < CE_BRIDGE_WATCH_FMT); ++k) {
                    char const ch = p[k];
                    formats[slot][k] = ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + ('a' - 'A')) : ch;
                }
                formats[slot][k] = '\0';
            }
            p += vlen;
            n -= vlen;
        }
        if (isWatch) {
            _CeBridge_JoinWatchCond(blockConds, depth, ownCond, conds[slot], CE_BRIDGE_WATCH_COND);
        }
        pos = (end < len) ? (end + 1) : len;
    }
    return count;
}

// Skip leading blanks and the comment marker.  Same rules as the watch parser.
static bool _CeBridge_SkipToWatchToken(const char** pp, size_t* pn)
{
    const char* p = *pp;
    size_t n = *pn;
    while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == '\r'))) {
        ++p;
        --n;
    }
    if ((n >= 2) && (p[0] == '/') && (p[1] == '/')) {
        p += 2;
        n -= 2;
    }
    else if ((n >= 1) && (p[0] == ';')) {
        p += 1;
        n -= 1;
    }
    else {
        return false;
    }
    while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == '\r'))) {
        ++p;
        --n;
    }
    *pp = p;
    *pn = n;
    return true;
}

static size_t _CeBridge_WatchTokenLen(const char* p, size_t n)
{
    size_t t = 0;
    while ((t < n) && (p[t] != ' ') && (p[t] != '\t') && (p[t] != ',') && (p[t] != '\r')) {
        ++t;
    }
    return t;
}

// Classify one line for the clear action.  Explicit np3* directives are always
// removed; the block-scoped shorthand entries ("// rcx") only inside a block.
// delta reports the begin/end depth change and applies even to removed lines,
// so deleting a block marker does not hide the entries it scoped.
static void _CeBridge_ClassifyWatchLine(const char* p, size_t n, bool inBlock,
                                        bool* remove, int* delta)
{
    *remove = false;
    *delta = 0;
    if (!_CeBridge_SkipToWatchToken(&p, &n)) {
        return;
    }
    size_t t = _CeBridge_WatchTokenLen(p, n);
    if (t == 0) {
        return;
    }
    if (_CeBridge_WatchTokenIs(p, t, "np3unwatch") || _CeBridge_WatchTokenIs(p, t, "np3nowatch")) {
        *remove = true;
        return;
    }
    bool const isExplicit = _CeBridge_WatchTokenIs(p, t, "np3watch");
    if (!isExplicit && !inBlock) {
        return;
    }
    if (isExplicit) {
        p += t;
        n -= t;
        while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == ',') || (*p == '\r'))) {
            ++p;
            --n;
        }
        t = _CeBridge_WatchTokenLen(p, n);
        *remove = true;
        if (_CeBridge_WatchTokenIs(p, t, "begin")) {
            *delta = 1;
        }
        else if (_CeBridge_WatchTokenIs(p, t, "end")) {
            *delta = -1;
        }
        return;
    }
    if (_CeBridge_WatchTokenIs(p, t, "begin")) {
        *delta = 1;
        *remove = true;
        return;
    }
    if (_CeBridge_WatchTokenIs(p, t, "end")) {
        *delta = -1;
        *remove = true;
        return;
    }
    if ((t >= CE_BRIDGE_WATCH_NAME) || ((p[0] >= '0') && (p[0] <= '9'))) {
        return;
    }
    for (size_t i = 0; i < t; ++i) {
        if (!_CeBridge_IsWatchNameChar(p[i])) {
            return;
        }
    }
    if (!_CeBridge_IsWatchSpecRest(p + t, n - t)) {
        return;
    }
    *remove = true;
}

// True when the line is a watch directive comment (np3watch / np3unwatch /
// np3nowatch, including begin/end/if forms) so the editor can strip all of
// them in one action.  No Win32 dependencies (see TestCeBridge.ps1).
static bool _CeBridge_IsWatchDirectiveLine(const char* p, size_t n)
{
    while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == '\r'))) {
        ++p;
        --n;
    }
    if ((n >= 2) && (p[0] == '/') && (p[1] == '/')) {
        p += 2;
        n -= 2;
    }
    else if ((n >= 1) && (p[0] == ';')) {
        p += 1;
        n -= 1;
    }
    else {
        return false;
    }
    while ((n > 0) && ((*p == ' ') || (*p == '\t') || (*p == '\r'))) {
        ++p;
        --n;
    }
    size_t t = 0;
    while ((t < n) && ((p[t] != ' ') && (p[t] != '\t') && (p[t] != ',') && (p[t] != '\r'))) {
        ++t;
    }
    return _CeBridge_WatchTokenIs(p, t, "np3watch") ||
        _CeBridge_WatchTokenIs(p, t, "np3unwatch") ||
        _CeBridge_WatchTokenIs(p, t, "np3nowatch");
}

bool CeBridge_IsWatchDirectiveLine(const char* line, size_t len)
{
    return _CeBridge_IsWatchDirectiveLine(line, len);
}

// Delete every watch directive comment line inside the selection (the whole
// document when nothing is selected) as one undoable action.
void CeBridge_ClearWatchLines(void)
{
    DocPos const selStart = SciCall_GetSelectionStart();
    DocPos const selEnd = SciCall_GetSelectionEnd();
    DocLn firstLine;
    DocLn lastLine;
    if (selEnd > selStart) {
        firstLine = SciCall_LineFromPosition(selStart);
        lastLine = SciCall_LineFromPosition(selEnd);
        if ((selEnd == SciCall_PositionFromLine(lastLine)) && (lastLine > firstLine)) {
            --lastLine;
        }
    }
    else {
        firstLine = 0;
        lastLine = SciCall_GetLineCount() - 1;
    }
    SciCall_BeginUndoAction();
    // Top-down so the block depth is known before each line; deleting the
    // current line keeps the index on the next one.  A removed begin/end still
    // updates the depth, otherwise its entries would survive the removal.
    int depth = 0;
    DocLn ln = firstLine;
    while (ln <= lastLine) {
        DocPos const ls = SciCall_PositionFromLine(ln);
        DocPos const le = SciCall_GetLineEndPosition(ln);
        DocPos const n = le - ls;
        if (n <= 0) {
            ++ln;
            continue;
        }
        char buf[512];
        DocPos const copy = (n < ((DocPos)sizeof(buf) - 1)) ? n : ((DocPos)sizeof(buf) - 1);
        const char* const text = SciCall_GetRangePointer(ls, copy);
        if (!text) {
            ++ln;
            continue;
        }
        memcpy(buf, text, (size_t)copy);
        buf[copy] = '\0';
        bool remove = false;
        int delta = 0;
        _CeBridge_ClassifyWatchLine(buf, (size_t)copy, depth > 0, &remove, &delta);
        if (remove) {
            DocPos const next = (ln + 1 < SciCall_GetLineCount())
                ? SciCall_PositionFromLine(ln + 1) : SciCall_GetTextLength();
            SciCall_DeleteRange(ls, next - ls);
            --lastLine;
        }
        else {
            ++ln;
        }
        depth += delta;
        if (depth < 0) {
            depth = 0;
        }
    }
    SciCall_EndUndoAction();
}

// Stable 16-hex-digit key for CE-side enable/disable state, derived from the
// current document path (an unsaved document shares one key).
static void _CeBridge_AaStateKey(LPCWSTR path, char out[17])
{
    static const char hex[] = "0123456789abcdef";
    uint64_t hash = 14695981039346656037ULL;  // FNV-1a 64 offset basis
    LPCWSTR const p = path ? path : L"";
    for (size_t i = 0; p[i]; ++i) {
        uint16_t const ch = (uint16_t)p[i];
        hash ^= (uint8_t)(ch & 0xFF);
        hash *= 1099511628211ULL;
        hash ^= (uint8_t)(ch >> 8);
        hash *= 1099511628211ULL;
    }
    for (int i = 15; i >= 0; --i) {
        out[i] = hex[(size_t)(hash & 0xFu)];
        hash >>= 4;
    }
    out[16] = '\0';
}

static bool _CeBridge_AaAppend(char** dst, char* const end, const char* text, size_t len)
{
    if (!dst || !*dst || !end || (len > 0 && !text) || (end <= *dst)) {
        return false;
    }
    size_t const avail = (size_t)(end - *dst);
    if (len >= avail) {
        return false;
    }
    if (len > 0) {
        memcpy(*dst, text, len);
        *dst += len;
    }
    **dst = '\0';
    return true;
}

// Wrap an AA document into Lua source for CE's own Lua state: the first run
// enables via autoAssemble(), the next one disables with the returned
// disableInfo table.  Only CE-native APIs are used, so an already deployed
// bridge script needs no update.  No Win32 dependencies (see TestCeBridge.ps1).
// Escape a Lua single-quoted string body (conditions may contain '...' literals).
static bool _CeBridge_AaEscapeLua(const char* src, char* dst, size_t cap)
{
    size_t w = 0;
    for (const char* s = src; *s; ++s) {
        char const ch = *s;
        if ((ch == '\\') || (ch == '\'')) {
            if (w + 2 >= cap) {
                return false;
            }
            dst[w++] = '\\';
        }
        else if (w + 1 >= cap) {
            return false;
        }
        dst[w++] = ch;
    }
    dst[w] = '\0';
    return true;
}

static bool _CeBridge_BuildAaRequest(char* dst, size_t cap, const char* script, size_t len,
                                     const char* keyHex, bool hasDisable, size_t* outLen)
{
    static const char head1[] =
        "-- NP3 AA one-click (generated by CeBridge.c); runs in CE's Lua state.\n"
        "local NP3_KEY='";
    static const char head2[] = "'\nlocal NP3_HAS_DISABLE=";
    static const char symHead[] = "\nlocal NP3_SYMBOLS={";
    static const char watchHead[] = "\nlocal NP3_AUTO_WATCH=";
    static const char watchHead2[] = "\nlocal NP3_WATCHES={";
    static const char head3[] = "\nlocal NP3_S=[";
    static const char tail[] =
        "]\n"
        "local B=NP3_CE_BRIDGE\n"
        "B.aaState=B.aaState or {}\n"
        "if not B.watchStop then\n"
        "  B.watchOut={}\n"
        "  B.watchActive=function() return B.watchTimer~=nil end\n"
        "  B.watchDrain=function()\n"
        "    if #B.watchOut==0 then return '' end\n"
        "    local s=table.concat(B.watchOut,string.char(10)); B.watchOut={}; return s\n"
        "  end\n"
        "  B.watchStop=function()\n"
        "    if B.watchTimer then pcall(function() B.watchTimer.Enabled=false; B.watchTimer.destroy() end) end\n"
        "    B.watchTimer=nil; B.watchOut={}\n"
        "  end\n"
        "  B.watchStart=function(symbols,key,specs,autoWatch)\n"
        "    B.watchStop()\n"
        "    local t=createTimer(nil,false)\n"
        "    if not t then return end\n"
        "    local alias={u8='byte',i8='byte',uint8='byte',u16='word',i16='word',uint16='word',\n"
        "      u32='dword',i32='dword',uint32='dword',u64='qword',i64='qword',uint64='qword',\n"
        "      ptr='pointer',f32='float',f64='double',str='string',wstr='wstring',arr='array'}\n"
        "    local validType={byte=true,word=true,dword=true,qword=true,pointer=true,\n"
        "      float=true,double=true,string=true,wstring=true,array=true}\n"
        "    local function normType(s)\n"
        "      s=tostring(s or ''):lower()\n"
        "      if s=='' or s=='auto' then return '' end\n"
        "      s=alias[s] or s\n"
        "      if validType[s] then return s end\n"
        "      return ''\n"
        "    end\n"
        "    local function normFmt(s)\n"
        "      s=tostring(s or ''):lower()\n"
        "      if s=='decimal' then s='dec' elseif s=='hexadecimal' then s='hex'\n"
        "      elseif s=='binary' then s='bin' end\n"
        "      if s=='hex' or s=='dec' or s=='bin' then return s end\n"
        "      return ''\n"
        "    end\n"
        "    local regAlias={eax='rax',ebx='rbx',ecx='rcx',edx='rdx',esi='rsi',edi='rdi',ebp='rbp',esp='rsp',eip='rip',flags='eflags',\n"
        "      ax='rax',bx='rbx',cx='rcx',dx='rdx',si='rsi',di='rdi',bp='rbp',sp='rsp',\n"
        "      al='rax',bl='rbx',cl='rcx',dl='rdx',ah='rax',bh='rbx',ch='rcx',dh='rdx',\n"
        "      sil='rsi',dil='rdi',bpl='rbp',spl='rsp'}\n"
        "    local regMask={eax=0xFFFFFFFF,ebx=0xFFFFFFFF,ecx=0xFFFFFFFF,edx=0xFFFFFFFF,esi=0xFFFFFFFF,edi=0xFFFFFFFF,ebp=0xFFFFFFFF,esp=0xFFFFFFFF,eip=0xFFFFFFFF,\n"
        "      ax=0xFFFF,bx=0xFFFF,cx=0xFFFF,dx=0xFFFF,si=0xFFFF,di=0xFFFF,bp=0xFFFF,sp=0xFFFF,\n"
        "      al=0xFF,bl=0xFF,cl=0xFF,dl=0xFF,sil=0xFF,dil=0xFF,bpl=0xFF,spl=0xFF,ah=0xFF,bh=0xFF,ch=0xFF,dh=0xFF}\n"
        "    local regShift={ah=8,bh=8,ch=8,dh=8}\n"
        "    local function regValue(r,name)\n"
        "      if not r or not name or name=='' then return nil end\n"
        "      local v=r[name]\n"
        "      if v==nil and (name:sub(1,1)=='r') then v=r['e'..name:sub(2)] end\n"
        "      if v~=nil then return v end\n"
        "      local b=regAlias[name]\n"
        "      if b then\n"
        "        v=r[b]\n"
        "        if v==nil and (b:sub(1,1)=='r') then v=r['e'..b:sub(2)] end\n"
        "        if v==nil then return nil end\n"
        "        local m=regMask[name]; if m then v=v&m end\n"
        "        local s=regShift[name]; if s then v=v>>s end\n"
        "        return v\n"
        "      end\n"
        "      local d=name:match('^r(%d+)[db]$')\n"
        "      if d then\n"
        "        local x=tonumber(d)\n"
        "        if x and x>=8 and x<=15 then\n"
        "          v=r[name:sub(1,#name-1)]\n"
        "          if v==nil then return nil end\n"
        "          return v & (name:sub(#name)=='d' and 0xFFFFFFFF or 0xFF)\n"
        "        end\n"
        "      end\n"
        "      return nil\n"
        "    end\n"
        "    -- Conditions are Lua expressions: the editor rewrites '||', '&&', '!'\n"
        "    -- and '$hex', so a condition may combine several tests, use\n"
        "    -- parentheses or call Lua code.  A bare name resolves to a register,\n"
        "    -- then a Lua global (script variables / objects), then an AA symbol\n"
        "    -- read as a 32-bit integer.\n"
        "    local condRegs={}\n"
        "    local condCache={}\n"
        "    local function condResolve(name)\n"
        "      local v=regValue(condRegs,name)\n"
        "      if v~=nil then return v end\n"
        "      local g=_G and rawget(_G,name)\n"
        "      if g~=nil then return g end\n"
        "      local ok,a=pcall(getAddress,name)\n"
        "      if ok and a and a~=0 then\n"
        "        local oki,i=pcall(readInteger,a)\n"
        "        if oki then return i end\n"
        "        return a\n"
        "      end\n"
        "      return nil\n"
        "    end\n"
        "    local condEnv=setmetatable({},{__index=function(_,k) return condResolve(k) end})\n"
        "    local function condCompile(expr)\n"
        "      local src='return ('..expr..')'\n"
        "      local f\n"
        "      if setfenv then\n"
        "        f=loadstring(src)\n"
        "        if f then setfenv(f,condEnv) end\n"
        "      else\n"
        "        f=load(src,'np3cond','t',condEnv)\n"
        "      end\n"
        "      return f\n"
        "    end\n"
        "    local function evalCond(expr,r)\n"
        "      if expr=='' then return true end\n"
        "      local f=condCache[expr]\n"
        "      if f==nil then\n"
        "        f=condCompile(expr) or false\n"
        "        condCache[expr]=f\n"
        "      end\n"
        "      if not f then return false end\n"
        "      condRegs=r or {}\n"
        "      local ok,res=pcall(f)\n"
        "      return (ok and res) and true or false\n"
        "    end\n"
        "    local function toBin(v,bits)\n"
        "      local o={}\n"
        "      for i=bits-1,0,-1 do o[#o+1]=(((v>>i)&1)==1) and '1' or '0' end\n"
        "      return '0b'..table.concat(o)\n"
        "    end\n"
        "    local function fmtInt(v,fmt,bits,typeName)\n"
        "      if fmt=='' then fmt=(typeName=='pointer') and 'hex' or 'dec' end\n"
        "      if fmt=='hex' then return string.format('0x%X',v) end\n"
        "      if fmt=='bin' then return toBin(v,bits) end\n"
        "      return string.format('%u',v)\n"
        "    end\n"
        "    local function readUInt(addr,typeName)\n"
        "      local ok,v\n"
        "      if typeName=='byte' then\n"
        "        ok,v=pcall(readByte,addr); if ok and v then return v&0xFF end\n"
        "      elseif typeName=='word' then\n"
        "        ok,v=pcall(readWord,addr); if ok and v then return v&0xFFFF end\n"
        "      elseif typeName=='dword' then\n"
        "        ok,v=pcall(readInteger,addr); if ok and v then return v&0xFFFFFFFF end\n"
        "      elseif typeName=='pointer' and type(readPointer)=='function' then\n"
        "        ok,v=pcall(readPointer,addr); if ok and v then return v end\n"
        "      end\n"
        "      ok,v=pcall(readQword,addr)\n"
        "      if not ok or not v then ok,v=pcall(readInteger,addr) end\n"
        "      if ok then return v end\n"
        "      return nil\n"
        "    end\n"
        "    local function scalarSize(s)\n"
        "      if s=='byte' then return 1 elseif s=='word' then return 2\n"
        "      elseif s=='dword' or s=='float' then return 4 end\n"
        "      return 8\n"
        "    end\n"
        "    local function fmtFloat(addr,typeName,fmt)\n"
        "      local ok,v\n"
        "      if typeName=='float' then ok,v=pcall(readFloat,addr)\n"
        "      else ok,v=pcall(readDouble,addr) end\n"
        "      if not ok or not v then return '?' end\n"
        "      if fmt=='hex' or fmt=='bin' then\n"
        "        local okr,raw\n"
        "        if typeName=='float' then\n"
        "          okr,raw=pcall(readInteger,addr); if okr and raw then raw=raw&0xFFFFFFFF end\n"
        "        else okr,raw=pcall(readQword,addr) end\n"
        "        if okr and raw then\n"
        "          if fmt=='hex' then return string.format('0x%X',raw) end\n"
        "          return toBin(raw,(typeName=='float') and 32 or 64)\n"
        "        end\n"
        "      end\n"
        "      return string.format((typeName=='float') and '%.6g' or '%.10g',v)\n"
        "    end\n"
        "    local function readScalar(addr,typeName,fmt)\n"
        "      if typeName=='float' or typeName=='double' then return fmtFloat(addr,typeName,fmt) end\n"
        "      local v=readUInt(addr,typeName)\n"
        "      if v==nil then return '?' end\n"
        "      local bits=64\n"
        "      if typeName=='byte' then bits=8 elseif typeName=='word' then bits=16\n"
        "      elseif typeName=='dword' then bits=32 end\n"
        "      return fmtInt(v,fmt,bits,typeName)\n"
        "    end\n"
        "    local function readText(addr,count,wide)\n"
        "      local n=count or 0\n"
        "      if n<=0 then n=64 end\n"
        "      local ok,s=pcall(readString,addr,n,wide and true or false)\n"
        "      if not ok or type(s)~='string' then return nil end\n"
        "      s=s:gsub('[\\r\\n\\t]',' ')\n"
        "      if #s>n then s=s:sub(1,n) end\n"
        "      return s\n"
        "    end\n"
        "    local function inferType(addr)\n"
        "      local ok,s=pcall(readString,addr,12,false)\n"
        "      if ok and type(s)=='string' and #s>=2 then\n"
        "        local printable=0\n"
        "        for i=1,#s do local b=s:byte(i); if b>=32 and b<127 then printable=printable+1 end end\n"
        "        if printable==#s and s:find('%a') then return 'string' end\n"
        "      end\n"
        "      local okf,f=pcall(readFloat,addr)\n"
        "      if okf and type(f)=='number' and f==f and f~=0 and math.abs(f)>1e-4\n"
        "        and math.abs(f)<1e9 and (f-math.floor(f))~=0 then\n"
        "        local okq,q=pcall(readQword,addr)\n"
        "        if okq and q and (q<0 or q>0xFFFF) then return 'float' end\n"
        "      end\n"
        "      local okq,q=pcall(readQword,addr)\n"
        "      if okq and q then\n"
        "        if q>=0 and q<=0xFFFFFFFF then return 'dword' end\n"
        "        return 'qword'\n"
        "      end\n"
        "      return 'dword'\n"
        "    end\n"
        "    local function formatValue(v)\n"
        "      if v.type=='' then v.type=inferType(v.addr) end\n"
        "      local t=v.type\n"
        "      local n=v.count or 0\n"
        "      if t=='string' or t=='wstring' then\n"
        "        local s=readText(v.addr,n,t=='wstring')\n"
        "        return s or '?',t,v.fmt\n"
        "      end\n"
        "      if t=='array' or n>1 then\n"
        "        local elem=(t=='array') and 'dword' or t\n"
        "        local total=n\n"
        "        if total<1 then total=4 end\n"
        "        local step=scalarSize(elem)\n"
        "        local o={}\n"
        "        for i=1,total do o[#o+1]=readScalar(v.addr+(i-1)*step,elem,v.fmt) end\n"
        "        return table.concat(o,','),t..'['..total..']',v.fmt\n"
        "      end\n"
        "      return readScalar(v.addr,t,v.fmt),t,v.fmt\n"
        "    end\n"
        "    local st={}\n"
        "    local index={}\n"
        "    local function emit(s) B.watchOut[#B.watchOut+1]=s; if #B.watchOut>200 then table.remove(B.watchOut,1) end end\n"
        "    local function add(name)\n"
        "      if not name or name=='' then return nil end\n"
        "      if index[name] then return index[name] end\n"
        "      local v={name=name,addr=nil,last=nil,wait=0,type='',fmt='',count=0,cond='',reg=false,rb='',rm=-1,rs=0}\n"
        "      st[#st+1]=v; index[name]=v; return v\n"
        "    end\n"
        "    local function drop(name)\n"
        "      local v=index[name]\n"
        "      if not v then return end\n"
        "      for i=1,#st do if st[i]==v then table.remove(st,i) break end end\n"
        "      index[name]=nil\n"
        "    end\n"
        "    if autoWatch==nil or autoWatch then\n"
        "      for i=1,#(symbols or {}) do add(symbols[i]) end\n"
        "    end\n"
        "    for i=1,#(specs or {}) do\n"
        "      local s=specs[i]\n"
        "      if s.exclude then\n"
        "        drop(s.name)\n"
        "      else\n"
        "        local v=add(s.name)\n"
        "        if v then\n"
        "          if s.reg then v.reg=true; v.rb=s.rb or ''; v.rm=s.rm or -1; v.rs=s.rs or 0; v.type='reg'\n"
        "          else v.type=normType(s.type) end\n"
        "          v.fmt=normFmt(s.fmt); v.count=s.count or 0; v.cond=s.cond or ''\n"
        "          if v.cond~='' and condCache[v.cond]==nil then\n"
        "            local f=condCompile(v.cond)\n"
        "            if f then condCache[v.cond]=f\n"
        "            else condCache[v.cond]=false; emit('[WATCH] '..v.name..' | 条件无效: '..v.cond) end\n"
        "          end\n"
        "        end\n"
        "      end\n"
        "    end\n"
        "    t.Interval=500\n"
        "    t.OnTimer=function()\n"
        "      if B.watchTimer~=t then return end\n"
        "      local regs=nil\n"
        "      pcall(function() regs=getRegisters() end)\n"
        // CE reports register keys in upper case (RAX, EFLAGS...); fold them so
        // the lower-case names used by the directives resolve the same way.
        "      if type(regs)=='table' then\n"
        "        local lr={}\n"
        "        for k,vv in pairs(regs) do\n"
        "          if type(k)=='string' then lr[k:lower()]=vv end\n"
        "        end\n"
        "        regs=lr\n"
        "      end\n"
        "      for i=1,#st do\n"
        "        local v=st[i]\n"
        "        if v.reg then\n"
        "          local rv=regValue(regs,v.rb)\n"
        "          if rv~=nil and evalCond(v.cond,regs) then\n"
        "            rv=(rv & v.rm)\n"
        "            if v.rs>0 then rv=rv>>v.rs end\n"
        "            local fmt=v.fmt\n"
        "            if fmt=='' then fmt='hex' end\n"
        "            local shown\n"
        "            if fmt=='hex' then shown=string.format('0x%X',rv)\n"
        "            elseif fmt=='bin' then shown=toBin(rv,64)\n"
        "            else shown=string.format('%u',rv) end\n"
        "            local marker='reg|'..fmt..'|'..shown\n"
        "            if (marker~=v.last) or (not v.on) then\n"
        "              v.last=marker; v.on=true\n"
        "              local extra=(v.cond~='') and (' | if '..v.cond) or ''\n"
        "              emit('[WATCH] '..v.name..' = '..shown..' | type=reg | fmt='..fmt..extra)\n"
        "            end\n"
        "          else\n"
        "            v.on=false\n"
        "          end\n"
        "        else\n"
        "          if not v.addr then\n"
        "            local ok,a=pcall(getAddress,v.name); if ok and a and a~=0 then v.addr=a end\n"
        "          end\n"
        "          if v.addr then\n"
        "            local ok,value,typeName,fmt=pcall(formatValue,v)\n"
        "            if ok and evalCond(v.cond,regs) then\n"
        "              local shown=fmt\n"
        "              if shown=='' then shown='auto' end\n"
        "              local marker=typeName..'|'..shown..'|'..tostring(value)\n"
        "              if (marker~=v.last) or (not v.on) then\n"
        "                v.last=marker; v.on=true\n"
        "                local extra=(v.cond~='') and (' | if '..v.cond) or ''\n"
        "                emit(string.format('[WATCH] %s @ %X = %s | type=%s | fmt=%s',\n"
        "                  v.name,v.addr,tostring(value),typeName,shown)..extra)\n"
        "              end\n"
        "            else\n"
        "              v.on=false\n"
        "            end\n"
        "          else\n"
        "            v.wait=v.wait+1\n"
        "            if v.wait==10 then emit('[WATCH] waiting for '..v.name); v.wait=0 end\n"
        "          end\n"
        "        end\n"
        "      end\n"
        "    end\n"
        "    B.watchTimer=t\n"
        "    t.Enabled=true\n"
        "    emit('[WATCH] monitoring '..#st..' symbol(s)')\n"
        "  end\n"
        "end\n"
        "local st=B.aaState[NP3_KEY]\n"
        "local pid=getOpenedProcessID()\n"
        "if not pid or pid==0 then error('NP3 AA: no target process attached in Cheat Engine') end\n"
        "print('NP3 AA: target PID '..tostring(pid))\n"
        "if st then\n"
        "  if not NP3_HAS_DISABLE then\n"
        "    error('NP3 AA: no [DISABLE] section; cannot disable. Add [DISABLE] or restart CE.')\n"
        "  end\n"
        "  B.watchStop()\n"
        "  print('NP3 AA: DISABLE')\n"
        "  local c,cm=autoAssembleCheck(NP3_S,false)\n"
        "  if not c then error('NP3 AA: disable precheck failed: '..tostring(cm)) end\n"
        "  local d=autoAssemble(NP3_S,st.info)\n"
        "  if not d then error('NP3 AA: disable failed') end\n"
        "  B.aaState[NP3_KEY]=nil\n"
        "  print('NP3 AA: disabled')\n"
        "  return '0'\n"
        "else\n"
        "  print('NP3 AA: ENABLE')\n"
        "  local c,cm=autoAssembleCheck(NP3_S,true)\n"
        "  if not c then error('NP3 AA: enable precheck failed: '..tostring(cm)) end\n"
        "  local e,info=autoAssemble(NP3_S)\n"
        "  if not e then error('NP3 AA: enable failed: '..tostring(info or '')) end\n"
        "  if not info then error('NP3 AA: CE returned no disableInfo; restart CE before retrying') end\n"
        "  B.aaState[NP3_KEY]={info=info}\n"
        "  B.watchStart(NP3_SYMBOLS,NP3_KEY,NP3_WATCHES,NP3_AUTO_WATCH)\n"
        "  print('NP3 AA: enabled; press Run again to disable')\n"
        "  return '1'\n"
        "end\n";

    if (!dst || !outLen || !keyHex || (cap == 0) || (len > 0 && !script)) {
        return false;
    }
    *outLen = 0;
    size_t const level = _CeBridge_AaBracketLevel(script, len);
    char* p = dst;
    char* const end = dst + cap;
    *p = '\0';
    if (!_CeBridge_AaAppend(&p, end, head1, sizeof(head1) - 1) ||
        !_CeBridge_AaAppend(&p, end, keyHex, strlen(keyHex)) ||
        !_CeBridge_AaAppend(&p, end, head2, sizeof(head2) - 1) ||
        !_CeBridge_AaAppend(&p, end, hasDisable ? "true" : "false", hasDisable ? 4 : 5) ||
        !_CeBridge_AaAppend(&p, end, symHead, sizeof(symHead) - 1)) {
        return false;
    }
    char symbols[24][64] = { { 0 } };
    int const symbolCount = _CeBridge_CollectAaSymbols(script, len, symbols, COUNTOF(symbols));
    char wNames[CE_BRIDGE_MAX_WATCHES][CE_BRIDGE_WATCH_NAME] = { { 0 } };
    char wTypes[CE_BRIDGE_MAX_WATCHES][CE_BRIDGE_WATCH_TYPE] = { { 0 } };
    char wFormats[CE_BRIDGE_MAX_WATCHES][CE_BRIDGE_WATCH_FMT] = { { 0 } };
    char wConds[CE_BRIDGE_MAX_WATCHES][CE_BRIDGE_WATCH_COND] = { { 0 } };
    int wCounts[CE_BRIDGE_MAX_WATCHES] = { 0 };
    int wExcludes[CE_BRIDGE_MAX_WATCHES] = { 0 };
    int wRegs[CE_BRIDGE_MAX_WATCHES] = { 0 };
    int autoWatch = 1;
    int watchCount = _CeBridge_CollectWatchDirectives(script, len, wNames, wTypes, wFormats,
                                                      wConds, wCounts, wExcludes, wRegs,
                                                      CE_BRIDGE_MAX_WATCHES, &autoWatch);
    // Fill omitted types from AA declarations (define literal, data label,
    // alloc size); the CE side probes the value when nothing is inferred.
    for (int i = 0; i < watchCount; ++i) {
        bool const autoType = (wTypes[i][0] == '\0') ||
            _CeBridge_WatchTokenIs(wTypes[i], strlen(wTypes[i]), "auto");
        if (!wExcludes[i] && autoType) {
            char inferred[CE_BRIDGE_WATCH_TYPE] = { '\0' };
            int inferredCount = 0;
            _CeBridge_InferSymbolType(script, len, wNames[i], inferred, &inferredCount);
            if (inferred[0]) {
                memcpy(wTypes[i], inferred, sizeof(inferred));
                wTypes[i][CE_BRIDGE_WATCH_TYPE - 1] = '\0';
                if ((wCounts[i] == 0) && (inferredCount > 0)) {
                    wCounts[i] = inferredCount;
                }
            }
        }
    }
    // Automatic symbols get the same declaration-based inference so their
    // default type does not depend on the runtime probe alone.
    if (autoWatch) {
        for (int s = 0; (s < symbolCount) && (watchCount < CE_BRIDGE_MAX_WATCHES); ++s) {
            bool seen = false;
            for (int i = 0; i < watchCount; ++i) {
                if (_CeBridge_AaNameEqualsCI(wNames[i], symbols[s])) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            char inferred[CE_BRIDGE_WATCH_TYPE] = { '\0' };
            int inferredCount = 0;
            _CeBridge_InferSymbolType(script, len, symbols[s], inferred, &inferredCount);
            // Large alloc buffers stay on the runtime probe: expanding every
            // automatic symbol to N bytes would flood the output pane.
            if (!inferred[0] || (inferredCount > 8)) {
                continue;
            }
            int const slot = watchCount;
            size_t const nameLen = strlen(symbols[s]);
            if ((nameLen >= CE_BRIDGE_WATCH_NAME) || (nameLen == 0)) {
                continue;
            }
            memcpy(wNames[slot], symbols[s], nameLen + 1);
            memcpy(wTypes[slot], inferred, sizeof(inferred));
            wTypes[slot][CE_BRIDGE_WATCH_TYPE - 1] = '\0';
            wFormats[slot][0] = '\0';
            wCounts[slot] = inferredCount;
            wExcludes[slot] = 0;
            ++watchCount;
        }
    }
    for (int i = 0; i < symbolCount; ++i) {
        bool const more = (i + 1 < symbolCount);
        if (!_CeBridge_AaAppend(&p, end, "'", 1) ||
            !_CeBridge_AaAppend(&p, end, symbols[i], strlen(symbols[i])) ||
            !_CeBridge_AaAppend(&p, end, more ? "'," : "'", more ? 2 : 1)) {
            return false;
        }
    }
    if (!_CeBridge_AaAppend(&p, end, "}", 1) ||
        !_CeBridge_AaAppend(&p, end, watchHead, sizeof(watchHead) - 1) ||
        !_CeBridge_AaAppend(&p, end, autoWatch ? "true" : "false", autoWatch ? 4 : 5) ||
        !_CeBridge_AaAppend(&p, end, watchHead2, sizeof(watchHead2) - 1)) {
        return false;
    }
    for (int i = 0; i < watchCount; ++i) {
        char entry[CE_BRIDGE_WATCH_NAME + CE_BRIDGE_WATCH_TYPE + CE_BRIDGE_WATCH_FMT +
                   (2 * CE_BRIDGE_WATCH_COND) + 128] = { '\0' };
        char cond[2 * CE_BRIDGE_WATCH_COND] = { '\0' };
        if (!_CeBridge_AaEscapeLua(wConds[i], cond, sizeof(cond))) {
            return false;
        }
        int n;
        if (wRegs[i]) {
            char rb[8] = { '\0' };
            unsigned long long rm = 0;
            int rs = 0;
            char lower[CE_BRIDGE_WATCH_NAME] = { '\0' };
            for (size_t k = 0; (k + 1 < sizeof(lower)) && wNames[i][k]; ++k) {
                const char ch = wNames[i][k];
                lower[k] = ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + ('a' - 'A')) : ch;
            }
            _CeBridge_WatchRegister(lower, rb, &rm, &rs);
            n = snprintf(entry, sizeof(entry),
                         "{name='%s',type='reg',fmt='%s',count=0,exclude=false,cond='%s',"
                         "reg=true,rb='%s',rm=%lld,rs=%d},",
                         wNames[i], wFormats[i], cond, rb,
                         (rm == ~(unsigned long long)0) ? -1LL : (long long)rm, rs);
        }
        else {
            n = snprintf(entry, sizeof(entry),
                         "{name='%s',type='%s',fmt='%s',count=%d,exclude=%s,cond='%s',reg=false},",
                         wNames[i], wTypes[i], wFormats[i], wCounts[i],
                         wExcludes[i] ? "true" : "false", cond);
        }
        if ((n <= 0) || ((size_t)n >= sizeof(entry)) ||
            !_CeBridge_AaAppend(&p, end, entry, (size_t)n)) {
            return false;
        }
    }
    if (!_CeBridge_AaAppend(&p, end, "}", 1) ||
        !_CeBridge_AaAppend(&p, end, head3, sizeof(head3) - 1)) {
        return false;
    }
    for (size_t i = 0; i < level; ++i) {
        if (!_CeBridge_AaAppend(&p, end, "=", 1)) {
            return false;
        }
    }
    if (!_CeBridge_AaAppend(&p, end, "[\n", 2) ||
        !_CeBridge_AaAppend(&p, end, script, len) ||
        !_CeBridge_AaAppend(&p, end, "\n]", 2)) {
        return false;
    }
    for (size_t i = 0; i < level; ++i) {
        if (!_CeBridge_AaAppend(&p, end, "=", 1)) {
            return false;
        }
    }
    if (!_CeBridge_AaAppend(&p, end, tail, sizeof(tail) - 1)) {
        return false;
    }
    *outLen = (size_t)(p - dst);
    return true;
}

// Cancel request: stop the CE-side watch and, when the script is still
// enabled, run its [DISABLE] section with the saved disableInfo.  Never enables
// a script.  No Win32 dependencies (see TestCeBridge.ps1).
static bool _CeBridge_BuildAaCancelRequest(char* dst, size_t cap, const char* script, size_t len,
                                           const char* keyHex, size_t* outLen)
{
    static const char head1[] =
        "-- NP3 AA cancel: stop the value watch and disable an enabled script.\n"
        "local B=NP3_CE_BRIDGE\n"
        "if B.watchStop then B.watchStop() end\n"
        "local st=B.aaState and B.aaState['";
    static const char head2[] =
        "']\n"
        "if st then\n"
        "  local NP3_S=[";
    static const char tail[] =
        "]\n"
        "  local ok=autoAssemble(NP3_S,st.info)\n"
        "  if not ok then error('NP3 AA: cancel/disable failed') end\n"
        "  B.aaState['";
    static const char tail2[] =
        "']=nil\n"
        "  print('NP3 AA: cancelled and disabled')\n"
        "else\n"
        "  print('NP3 AA: watch stopped; no active script')\n"
        "end\n"
        "return '0'\n";

    if (!dst || !outLen || !keyHex || (cap == 0) || (len > 0 && !script)) {
        return false;
    }
    *outLen = 0;
    size_t const level = _CeBridge_AaBracketLevel(script, len);
    char* p = dst;
    char* const end = dst + cap;
    *p = '\0';
    if (!_CeBridge_AaAppend(&p, end, head1, sizeof(head1) - 1) ||
        !_CeBridge_AaAppend(&p, end, keyHex, strlen(keyHex)) ||
        !_CeBridge_AaAppend(&p, end, head2, sizeof(head2) - 1)) {
        return false;
    }
    for (size_t i = 0; i < level; ++i) {
        if (!_CeBridge_AaAppend(&p, end, "=", 1)) {
            return false;
        }
    }
    if (!_CeBridge_AaAppend(&p, end, "[\n", 2) ||
        !_CeBridge_AaAppend(&p, end, script, len) ||
        !_CeBridge_AaAppend(&p, end, "\n]", 2)) {
        return false;
    }
    for (size_t i = 0; i < level; ++i) {
        if (!_CeBridge_AaAppend(&p, end, "=", 1)) {
            return false;
        }
    }
    if (!_CeBridge_AaAppend(&p, end, tail, sizeof(tail) - 1) ||
        !_CeBridge_AaAppend(&p, end, keyHex, strlen(keyHex)) ||
        !_CeBridge_AaAppend(&p, end, tail2, sizeof(tail2) - 1)) {
        return false;
    }
    *outLen = (size_t)(p - dst);
    return true;
}

static void _CeBridge_AaOwnerSet(const char* key, const char* script, DWORD len)
{
    if (!key || !script) {
        return;
    }
    char* const copy = (char*)AllocMem((size_t)len + 1, HEAP_ZERO_MEMORY);
    if (!copy) {
        return;
    }
    memcpy(copy, script, len);
    FreeMem(s_aaOwner.script);
    s_aaOwner.script = copy;
    s_aaOwner.len = len;
    StringCchCopyA(s_aaOwner.key, COUNTOF(s_aaOwner.key), key);
}

static void _CeBridge_AaOwnerClear(void)
{
    FreeMem(s_aaOwner.script);
    s_aaOwner.script = NULL;
    s_aaOwner.len = 0;
    s_aaOwner.key[0] = '\0';
}

static bool _CeBridge_AaOwnerMatches(const char* key)
{
    return (s_aaOwner.script != NULL) && key && (strcmp(s_aaOwner.key, key) == 0);
}

static char* _CeBridge_AaOwnerCancelRequest(DWORD* outLen)
{
    if (outLen) {
        *outLen = 0;
    }
    if (!s_aaOwner.script || !s_aaOwner.key[0]) {
        return NULL;
    }
    size_t const cap = (size_t)s_aaOwner.len + 4096;
    char* const req = (char*)AllocMem(cap, HEAP_ZERO_MEMORY);
    size_t reqLen = 0;
    if (!req || !_CeBridge_BuildAaCancelRequest(req, cap, s_aaOwner.script,
                                               (size_t)s_aaOwner.len, s_aaOwner.key, &reqLen)) {
        FreeMem(req);
        return NULL;
    }
    if (outLen) {
        *outLen = (DWORD)reqLen;
    }
    return req;
}

// Directory names like "Cheat Engine 7.7" / "cheatengine" / "Cheat-Engine".
static bool _CeBridge_IsCeDirName(LPCWSTR name, size_t len)
{
    static const WCHAR prefix[] = L"cheatengine";
    size_t const prefixLen = sizeof(prefix) / sizeof(prefix[0]) - 1;
    if (!name) {
        return false;
    }
    size_t k = 0;
    for (size_t i = 0; i < len && name[i]; ++i) {
        WCHAR ch = name[i];
        if (ch == L' ' || ch == L'_' || ch == L'-') {
            continue;
        }
        if (ch >= L'A' && ch <= L'Z') {
            ch = (WCHAR)(ch + (L'a' - L'A'));
        }
        if (k >= prefixLen) {
            return true;
        }
        if (ch != prefix[k]) {
            return false;
        }
        ++k;
    }
    return k == prefixLen;
}

// Cheat Engine executables: "cheatengine-x86_64.exe" / "cheatengine-i386.exe"
// (and the plain 32-bit "cheatengine.exe").  Only the base name is compared, so
// the caller can pass either a file name or a full path's leaf.
static bool _CeBridge_IsCeExeName(LPCWSTR name, size_t len)
{
    static const WCHAR* const names[] = {
        L"cheatengine-x86_64", L"cheatengine-i386", L"cheatengine"
    };
    if (!name || len < 4) {
        return false;
    }
    size_t base = len;
    if (name[base - 4] == L'.' &&
        (name[base - 3] == L'e' || name[base - 3] == L'E') &&
        (name[base - 2] == L'x' || name[base - 2] == L'X') &&
        (name[base - 1] == L'e' || name[base - 1] == L'E')) {
        base -= 4;
    }
    for (size_t n = 0; n < COUNTOF(names); ++n) {
        size_t const nameLen = wcslen(names[n]);
        if (base != nameLen) {
            continue;
        }
        bool same = true;
        for (size_t i = 0; i < nameLen && same; ++i) {
            WCHAR ch = name[i];
            if (ch >= L'A' && ch <= L'Z') {
                ch = (WCHAR)(ch + (L'a' - L'A'));
            }
            same = (ch == names[n][i]);
        }
        if (same) {
            return true;
        }
    }
    return false;
}

// Strips the last path component in place:
//   "E:\CE\autorun\notepad" -> "E:\CE\autorun"  (returns true)
//   "E:\" / "E:"            -> unchanged         (returns false, no parent)
static bool _CeBridge_ParentDir(WCHAR* dir)
{
    if (!dir || !dir[0]) {
        return false;
    }
    size_t len = wcslen(dir);
    while (len > 0 && (dir[len - 1] == L'\\' || dir[len - 1] == L'/')) {
        dir[--len] = L'\0';
    }
    WCHAR* const slash = wcsrchr(dir, L'\\');
    if (!slash || slash == dir) {
        return false;   // "\foo" is drive-relative: nothing sane above it
    }
    if (slash == dir + 2 && dir[1] == L':') {
        dir[3] = L'\0';  // "E:\CE" -> "E:\", stop walking
        return false;
    }
    *slash = L'\0';
    return true;
}

// Candidate roots on one drive.  CE may be installed directly below the root,
// below a "Cheat Engine" folder, or in a program folder.
static size_t _CeBridge_BuildRoots(WCHAR roots[][MAX_PATH], size_t maxRoots, WCHAR drive)
{
    static const WCHAR* const tails[] = {
        L"\\", L"\\Cheat Engine\\", L"\\Program Files\\",
        L"\\Program Files (x86)\\", L"\\Games\\", L"\\Tools\\"
    };
    size_t const tailCount = sizeof(tails) / sizeof(tails[0]);
    size_t count = 0;
    for (size_t i = 0; i < tailCount && count < maxRoots; ++i) {
        WCHAR* const dst = roots[count];
        size_t k = 0;
        dst[k++] = drive;
        dst[k++] = L':';
        for (const WCHAR* p = tails[i]; *p && k + 1 < MAX_PATH; ++p) {
            dst[k++] = *p;
        }
        dst[k] = L'\0';
        ++count;
    }
    return count;
}

// ----------------------------------------------------------------------------
// Installation discovery / deployment
// ----------------------------------------------------------------------------

static bool _JoinPath(WCHAR* dst, size_t cch, LPCWSTR dir, LPCWSTR leaf)
{
    if (!dst || !dir || !leaf) {
        return false;
    }
    size_t const dlen = wcslen(dir);
    LPCWSTR const sep = (dlen > 0 && (dir[dlen - 1] == L'\\' || dir[dlen - 1] == L'/')) ? L"" : L"\\";
    return SUCCEEDED(StringCchPrintfW(dst, cch, L"%s%s%s", dir, sep, leaf));
}

static bool _IsRegularFile(LPCWSTR path)
{
    DWORD const attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool _IsDirectory(LPCWSTR path)
{
    DWORD const attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// A CE installation is recognized by its layout, not by a version number.
static bool _IsCeInstallDir(LPCWSTR dir)
{
    if (!dir || !dir[0]) {
        return false;
    }
    WCHAR path[MAX_PATH] = { L'\0' };
    if (!_JoinPath(path, COUNTOF(path), dir, L"celua.txt") || !_IsRegularFile(path)) {
        return false;
    }
    if (!_JoinPath(path, COUNTOF(path), dir, L"autorun") || !_IsDirectory(path)) {
        return false;
    }
    if (!_JoinPath(path, COUNTOF(path), dir, L"lua53-64.dll") || !_IsRegularFile(path)) {
        if (!_JoinPath(path, COUNTOF(path), dir, L"lua53-32.dll") || !_IsRegularFile(path)) {
            return false;
        }
    }
    return true;
}

static bool _ScanDrives(WCHAR out[MAX_PATH])
{
    DWORD const drives = GetLogicalDrives();
    WCHAR roots[8][MAX_PATH] = { { L'\0' } };
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) {
            continue;
        }
        WCHAR const drive = (WCHAR)(L'A' + i);
        size_t const count = _CeBridge_BuildRoots(roots, COUNTOF(roots), drive);
        for (size_t r = 0; r < count; ++r) {
            if (_IsCeInstallDir(roots[r])) {
                StringCchCopyW(out, MAX_PATH, roots[r]);
                return true;
            }
            WCHAR pattern[MAX_PATH] = { L'\0' };
            if (!_JoinPath(pattern, COUNTOF(pattern), roots[r], L"Cheat Engine*")) {
                continue;
            }
            WIN32_FIND_DATAW fd = { 0 };
            HANDLE const find = FindFirstFileW(pattern, &fd);
            if (find == INVALID_HANDLE_VALUE) {
                continue;
            }
            bool found = false;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') {
                    continue;
                }
                if (!_CeBridge_IsCeDirName(fd.cFileName, wcslen(fd.cFileName))) {
                    continue;
                }
                WCHAR full[MAX_PATH] = { L'\0' };
                if (_JoinPath(full, COUNTOF(full), roots[r], fd.cFileName) && _IsCeInstallDir(full)) {
                    StringCchCopyW(out, MAX_PATH, full);
                    found = true;
                    break;
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
            if (found) {
                return true;
            }
        }
    }
    return false;
}

// The Cheat Engine that is actually running is the one a script will be sent
// to, so its directory wins over any remembered path (a stale <CE 7.7> entry
// must not shadow a newer installation).
static bool _RunningCeDir(WCHAR out[MAX_PATH])
{
    HANDLE const snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_CeBridge_IsCeExeName(pe.szExeFile, wcslen(pe.szExeFile))) {
                continue;
            }
            HANDLE const proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                            pe.th32ProcessID);
            if (!proc) {
                continue;
            }
            WCHAR exe[MAX_PATH] = { L'\0' };
            DWORD cch = (DWORD)COUNTOF(exe);
            if (QueryFullProcessImageNameW(proc, 0, exe, &cch) && cch > 0) {
                WCHAR* const slash = wcsrchr(exe, L'\\');
                if (slash) {
                    *slash = L'\0';
                    if (_IsCeInstallDir(exe)) {
                        StringCchCopyW(out, MAX_PATH, exe);
                        found = true;
                    }
                }
            }
            CloseHandle(proc);
            if (found) {
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// The Cheat Engine installation that contains the running executable.  The
// program is not necessarily next to the CE executable: CE's own integration
// folder is <CE>\autorun\notepad, so walk up a few levels before giving up.
// This keeps several installed versions apart: a Notepad3 inside
// "...\Cheat Engine 772\..." must deploy to 772, never to another version.
#define CE_MODULE_ANCESTOR_DEPTH 4

static bool _ModuleCeDir(WCHAR out[MAX_PATH])
{
    LPCWSTR const moduleDir = Path_Get(Paths.ModuleDirectory);
    if (!moduleDir || !moduleDir[0]) {
        return false;
    }
    WCHAR dir[MAX_PATH] = { L'\0' };
    if (FAILED(StringCchCopyW(dir, COUNTOF(dir), moduleDir))) {
        return false;
    }
    for (int depth = 0; depth <= CE_MODULE_ANCESTOR_DEPTH; ++depth) {
        if (_IsCeInstallDir(dir)) {
            StringCchCopyW(out, MAX_PATH, dir);
            return true;
        }
        if (!_CeBridge_ParentDir(dir)) {
            break;
        }
    }
    return false;
}

// Preference order, so the script lands in the CE version the user is working
// with instead of a fixed (or previously remembered) installation:
//  1. the installation that contains the program (see _ModuleCeDir)
//  2. the Cheat Engine process that is currently running
//  3. the path remembered in [Settings2] (used by CeLua for the Lua DLL)
//  4. a drive scan over every installed version
static bool _DiscoverCeDir(WCHAR out[MAX_PATH])
{
    if (_ModuleCeDir(out)) {
        return true;
    }
    if (_RunningCeDir(out)) {
        return true;
    }
    if (Settings2.CeInstallPath[0] && _IsCeInstallDir(Settings2.CeInstallPath)) {
        StringCchCopyW(out, MAX_PATH, Settings2.CeInstallPath);
        return true;
    }
    return _ScanDrives(out);
}

static void _PersistCePath(LPCWSTR path)
{
    if (FAILED(StringCchCopyW(Settings2.CeInstallPath, COUNTOF(Settings2.CeInstallPath), path))) {
        return;
    }
    if (OpenSettingsFile("CeBridge")) {
        IniSectionSetString(Constants.Settings2_Section, L"CeInstallPath", Settings2.CeInstallPath);
        CloseSettingsFile("CeBridge", true);
    }
}

static const char* _LoadResourceById(UINT id, size_t* lenOut)
{
    *lenOut = 0;
    HMODULE const hInst = GetModuleHandleW(NULL);
    HRSRC const hRes = FindResourceExW(hInst, RT_RCDATA, MAKEINTRESOURCEW(id),
                                      MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
    if (!hRes) {
        return NULL;
    }
    HGLOBAL const hMem = LoadResource(hInst, hRes);
    if (!hMem) {
        return NULL;
    }
    DWORD const size = SizeofResource(hInst, hRes);
    const char* const data = (const char*)LockResource(hMem);
    if (!data || size == 0) {
        return NULL;
    }
    *lenOut = (size_t)size;
    return data;
}

static const char* _LoadBridgeLua(size_t* lenOut)
{
    return _LoadResourceById(IDR_CE_BRIDGE_LUA, lenOut);
}

// Write the default user-template file only when it does not exist: the whole
// point of that file is that the user edits it, so redeploying must not
// overwrite their work.
static void _DeployDefaultTemplates(LPCWSTR dir)
{
    WCHAR path[MAX_PATH] = { L'\0' };
    if (!_JoinPath(path, COUNTOF(path), dir, L"np3_user_templates.tpl") ||
        _IsRegularFile(path)) {
        return;
    }
    size_t len = 0;
    const char* const data = _LoadResourceById(IDR_CE_DEFAULT_TEMPLATES, &len);
    if (!data || len == 0) {
        return;
    }
    HANDLE const file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, data, (DWORD)len, &written, NULL);
    CloseHandle(file);
}

static bool _SameContent(LPCWSTR path, const char* data, size_t len)
{
    HANDLE const file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD const size = GetFileSize(file, NULL);
    bool same = (size != INVALID_FILE_SIZE) && ((size_t)size == len);
    if (same && len > 0) {
        char* const buf = (char*)AllocMem(len, 0);
        if (!buf) {
            same = false;
        }
        else {
            DWORD read = 0;
            same = ReadFile(file, buf, (DWORD)len, &read, NULL) &&
                   read == (DWORD)len && memcmp(buf, data, len) == 0;
            FreeMem(buf);
        }
    }
    CloseHandle(file);
    return same;
}

static bool _WriteLuaFile(LPCWSTR path, const char* data, size_t len)
{
    if (_SameContent(path, data, len)) {
        return true;
    }
    HANDLE const file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    BOOL const ok = WriteFile(file, data, (DWORD)len, &written, NULL);
    CloseHandle(file);
    return ok && written == (DWORD)len;
}

// One-shot deployment companions: tell the CE-side Lua which editor to launch,
// and retire the legacy external-editor hook (it overrides OnAutoAssemblerEdit
// because autorun scripts load in file-name order).  The old file is kept as a
// backup instead of being deleted.
static void _DeploySideFiles(LPCWSTR autorun)
{
    WCHAR path[MAX_PATH] = { L'\0' };
    WCHAR exe[MAX_PATH] = { L'\0' };
    if (_JoinPath(path, COUNTOF(path), autorun, L"np3_ce_bridge.path") &&
        GetModuleFileNameW(NULL, exe, COUNTOF(exe))) {
        char utf8[MAX_PATH * 3] = { 0 };
        int const n = WideCharToMultiByte(CP_UTF8, 0, exe, -1, utf8, sizeof(utf8), NULL, NULL);
        if (n > 1) {
            HANDLE const file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, NULL);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(file, utf8, (DWORD)(n - 1), &written, NULL);
                CloseHandle(file);
            }
        }
    }
    WCHAR legacy[MAX_PATH] = { L'\0' };
    WCHAR disabled[MAX_PATH] = { L'\0' };
    if (_JoinPath(legacy, COUNTOF(legacy), autorun, L"usenotepad.lua") &&
        _JoinPath(disabled, COUNTOF(disabled), autorun, L"usenotepad.lua.np3-disabled") &&
        _IsRegularFile(legacy) && !_IsRegularFile(disabled)) {
        MoveFileW(legacy, disabled);
    }
    _DeployDefaultTemplates(autorun);
}

// Ask before replacing an existing bridge script.  Only an explicit deploy
// asks; the implicit deployment before a run must never block on a dialog.
// Returns false when the user declined (|*canceled| is then set).
static bool _WriteBridgeFile(HWND hwnd, bool interactive, LPCWSTR path,
                             const char* data, size_t len, bool* canceled)
{
    if (canceled) {
        *canceled = false;
    }
    if (!_IsRegularFile(path) || _SameContent(path, data, len)) {
        return _WriteLuaFile(path, data, len);
    }
    if (!interactive) {
        return _WriteLuaFile(path, data, len);
    }
    WCHAR msg[MAX_PATH * 3] = { L'\0' };
    if (!FormatLngStringW(msg, (int)COUNTOF(msg), IDS_MUI_CE_DEPLOY_OVERWRITE, path)) {
        return _WriteLuaFile(path, data, len);
    }
    WCHAR title[128] = { L'\0' };
    GetLngString(IDS_MUI_CE_DEPLOY_TITLE, title, (int)COUNTOF(title));
    if (title[0] == L'\0') {
        StringCchCopyW(title, COUNTOF(title), L"CE Bridge");
    }
    if (MessageBoxW(hwnd, msg, title,
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        if (canceled) {
            *canceled = true;
        }
        return false;
    }
    return _WriteLuaFile(path, data, len);
}

// Deploy into <CE>\autorun of the detected installation (any CE version).
static bool _DeployBridge(HWND hwnd, LPCWSTR ceDir, bool interactive, bool* canceled)
{
    if (canceled) {
        *canceled = false;
    }
    size_t len = 0;
    const char* const lua = _LoadBridgeLua(&len);
    if (!lua || len == 0) {
        return false;
    }
    WCHAR autorun[MAX_PATH] = { L'\0' };
    WCHAR path[MAX_PATH] = { L'\0' };
    if (!_JoinPath(autorun, COUNTOF(autorun), ceDir, L"autorun") ||
        !_JoinPath(path, COUNTOF(path), ceDir, CE_BRIDGE_LUA_FILE)) {
        return false;
    }
    if (!_IsDirectory(autorun)) {
        CreateDirectoryW(autorun, NULL);
    }
    if (!_WriteBridgeFile(hwnd, interactive, path, lua, len, canceled)) {
        return false;
    }
    _DeploySideFiles(autorun);
    return true;
}

// Last resort when no Cheat Engine folder can be detected or written to: drop
// the script next to the program and let the user copy it into the autorun
// folder of whichever CE version they use.
static bool _ExportBridgeTo(HWND hwnd, LPCWSTR dir, bool interactive,
                           WCHAR* outPath, size_t cchOut, bool* canceled)
{
    if (canceled) {
        *canceled = false;
    }
    if (!dir || !dir[0]) {
        return false;
    }
    size_t len = 0;
    const char* const lua = _LoadBridgeLua(&len);
    if (!lua || len == 0) {
        return false;
    }
    WCHAR path[MAX_PATH] = { L'\0' };
    if (!_JoinPath(path, COUNTOF(path), dir, L"np3_ce_bridge.lua")) {
        return false;
    }
    if (!_WriteBridgeFile(hwnd, interactive, path, lua, len, canceled)) {
        return false;
    }
    _DeployDefaultTemplates(dir);
    if (outPath && cchOut > 0) {
        StringCchCopyW(outPath, cchOut, path);
    }
    return true;
}

// ----------------------------------------------------------------------------
// Pipe client
// ----------------------------------------------------------------------------

void CeBridge_PostLine(HWND hwnd, BYTE kind, char* text)
{
    if (!text) {
        return;
    }
    if (!PostMessageW(hwnd, WM_CEBRIDGE_OUTPUT, (WPARAM)kind, (LPARAM)text)) {
        FreeMem(text);
    }
}

void CeBridge_PostConstLine(HWND hwnd, BYTE kind, const char* text)
{
    size_t const len = strlen(text);
    char* const buf = (char*)AllocMem(len + 1, HEAP_ZERO_MEMORY);
    if (buf) {
        memcpy(buf, text, len);
        CeBridge_PostLine(hwnd, kind, buf);
    }
}

void CeBridge_PostHint(HWND hwnd, const char* text)
{
    if (!text) {
        return;
    }
    size_t const len = strlen(text);
    char* const buf = (char*)AllocMem(len + 1, HEAP_ZERO_MEMORY);
    if (!buf) {
        return;
    }
    memcpy(buf, text, len);
    if (!PostMessageW(hwnd, WM_CEBRIDGE_HINT, 0, (LPARAM)buf)) {
        FreeMem(buf);
    }
}

static bool _WriteAll(HANDLE pipe, const void* data, DWORD len)
{
    const BYTE* p = (const BYTE*)data;
    while (len > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, p, len, &written, NULL) || written == 0) {
            return false;
        }
        p += written;
        len -= written;
    }
    return true;
}

static bool _ReadAll(HANDLE pipe, void* data, DWORD len)
{
    BYTE* p = (BYTE*)data;
    while (len > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, p, len, &read, NULL) || read == 0) {
            return false;
        }
        p += read;
        len -= read;
    }
    return true;
}

// Bounded read: CE's main-thread synchronize() can be delayed indefinitely
// (e.g. while a modal template dialog is open).  A plain ReadFile would then
// block the caller forever; report a timeout instead.
static bool _ReadWithTimeout(HANDLE pipe, void* data, DWORD len, DWORD start, DWORD timeoutMs)
{
    BYTE* p = (BYTE*)data;
    DWORD done = 0;
    while (done < len) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) {
            return false;
        }
        if (avail > 0) {
            DWORD const chunk = (avail < (len - done)) ? avail : (len - done);
            DWORD read = 0;
            if (!ReadFile(pipe, p + done, chunk, &read, NULL) || read == 0) {
                return false;
            }
            done += read;
        }
        else {
            if (_CeBridge_DeadlineReached(GetTickCount(), start, timeoutMs)) {
                return false;
            }
            Sleep(10);
        }
    }
    return true;
}

// The AA request and the cancel request return exactly one character: '1'
// while the CE-side AA state entry exists, '0' after [DISABLE].  The watch
// poll uses the same convention, so an externally removed script also clears
// the flag.
static void _CeBridge_SetAaActiveFromResult(const char* text, DWORD len)
{
    if (!text || (len != 1)) {
        return;
    }
    if (text[0] == '1') {
        InterlockedExchange(&s_ceBridgeAaActive, 1);
    }
    else if (text[0] == '0') {
        InterlockedExchange(&s_ceBridgeAaActive, 0);
    }
}

// Pull buffered AA watch lines once a second.  The CE-side timer does the
// sampling; doing the output inside a normal RUN request keeps print()
// redirection (and thus the output pane) working with an already deployed
// bridge script.  No protocol change is needed.
static const char s_aaWatchPollLua[] =
    "local B=NP3_CE_BRIDGE\n"
    "if B.watchDrain then local s=B.watchDrain(); if s and s~='' then print(s) end end\n"
    "return (B.watchActive and B.watchActive()) and '1' or '0'\n";

static void _BridgeSendQuiet(HWND hwnd, const char* code, DWORD len,
                             bool stopWhenInactive, bool reportErrors)
{
    if (!WaitNamedPipeW(CE_BRIDGE_PIPE_W, CE_BRIDGE_WAIT_MS)) {
        if (reportErrors) {
            CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, s_errNoPipe);
        }
        return;
    }
    HANDLE pipe = CreateFileW(CE_BRIDGE_PIPE_W, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (reportErrors) {
            CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, s_errOpen);
        }
        return;
    }
    size_t frameLen = 0;
    BYTE* frame = (BYTE*)AllocMem(5 + (size_t)len, 0);
    if (!frame || !_CeBridge_EncodeFrame(frame, 5 + (size_t)len,
                                         CE_BRIDGE_CMD_RUN, code, len, &frameLen) ||
        !_WriteAll(pipe, frame, (DWORD)frameLen)) {
        if (reportErrors) {
            CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, s_errWrite);
        }
        if (frame) {
            FreeMem(frame);
        }
        CloseHandle(pipe);
        return;
    }
    FreeMem(frame);

    for (;;) {
        BYTE hdr[5] = { 0 };
        if (!_ReadAll(pipe, hdr, 5)) {
            break;
        }
        BYTE const kind = hdr[0];
        DWORD const n = _CeBridge_DecodeLen(hdr + 1);
        if (n > CE_BRIDGE_MAX_FRAME) {
            break;
        }
        char* const buf = (char*)AllocMem((size_t)n + 1, HEAP_ZERO_MEMORY);
        if (!buf) {
            break;
        }
        if ((n > 0) && !_ReadAll(pipe, buf, n)) {
            FreeMem(buf);
            break;
        }
        buf[n] = '\0';
        if ((kind == CE_BRIDGE_OUTPUT) || (reportErrors && (kind == CE_BRIDGE_ERROR))) {
            CeBridge_PostLine(hwnd, kind, buf);   // takes ownership
        }
        else {
            if (kind == CE_BRIDGE_RESULT) {
                _CeBridge_SetAaActiveFromResult(buf, n);
                if (stopWhenInactive && (n == 1) && (buf[0] == '0')) {
                    InterlockedExchange(&s_ceBridgeWatchCancel, 1);
                }
            }
            FreeMem(buf);
        }
        if (kind == CE_BRIDGE_DONE) {
            break;
        }
    }
    CloseHandle(pipe);
}

// One short control request (apply script / close session).  Unlike the run
// path this returns the RESULT or ERROR text to the caller instead of posting
// it to the output pane, so the editor can report the CE memory-record id.
bool CeBridge_SendRequest(BYTE cmd, const char* payload, DWORD len,
                          char** replyOut, DWORD* replyLenOut)
{
    return CeBridge_SendRequestTimed(cmd, payload, len, replyOut, replyLenOut,
                                     CE_BRIDGE_WAIT_MS, CE_BRIDGE_REPLY_TIMEOUT_MS);
}

// Same as CeBridge_SendRequest but with caller-supplied timeouts.  Shutdown
// needs short bounds: the process is about to exit, so waiting must never
// stall it.
bool CeBridge_SendRequestTimed(BYTE cmd, const char* payload, DWORD len,
                                char** replyOut, DWORD* replyLenOut,
                                DWORD pipeWaitMs, DWORD replyTimeoutMs)
{
    if (replyOut) {
        *replyOut = NULL;
    }
    if (replyLenOut) {
        *replyLenOut = 0;
    }
    if (len > 0 && !payload) {
        return false;
    }
    if (!WaitNamedPipeW(CE_BRIDGE_PIPE_W, pipeWaitMs)) {
        return false;
    }
    HANDLE pipe = CreateFileW(CE_BRIDGE_PIPE_W, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    size_t frameLen = 0;
    BYTE* frame = (BYTE*)AllocMem(5 + (size_t)len, 0);
    if (!frame || !_CeBridge_EncodeFrame(frame, 5 + (size_t)len, cmd, payload, len, &frameLen) ||
        !_WriteAll(pipe, frame, (DWORD)frameLen)) {
        if (frame) {
            FreeMem(frame);
        }
        CloseHandle(pipe);
        return false;
    }
    FreeMem(frame);

    char* reply = NULL;
    DWORD replyLen = 0;
    bool success = false;
    DWORD const replyStart = GetTickCount();
    for (;;) {
        BYTE hdr[5] = { 0 };
        if (!_ReadWithTimeout(pipe, hdr, 5, replyStart, replyTimeoutMs)) {
            break;
        }
        BYTE const kind = hdr[0];
        DWORD const n = _CeBridge_DecodeLen(hdr + 1);
        if (n > CE_BRIDGE_MAX_FRAME) {
            break;
        }
        char* const buf = (char*)AllocMem((size_t)n + 1, HEAP_ZERO_MEMORY);
        if (!buf) {
            break;
        }
        if ((n > 0) && !_ReadWithTimeout(pipe, buf, n, replyStart, replyTimeoutMs)) {
            FreeMem(buf);
            break;
        }
        if ((kind == CE_BRIDGE_RESULT) || (kind == CE_BRIDGE_ERROR)) {
            if (reply) {
                FreeMem(reply);
            }
            reply = buf;          // transfer ownership
            replyLen = n;
        }
        else if (kind == CE_BRIDGE_OUTPUT) {
            // This reader runs on a worker thread: never post UI messages
            // carrying heap buffers from here.
            FreeMem(buf);
        }
        else if (kind == CE_BRIDGE_DONE) {
            success = (n == 1) && (buf[0] == '0');
            FreeMem(buf);
            break;
        }
        else {
            FreeMem(buf);
        }
    }
    CloseHandle(pipe);

    if (replyOut) {
        *replyOut = reply;
    }
    else if (reply) {
        FreeMem(reply);
    }
    if (replyLenOut) {
        *replyLenOut = replyLen;
    }
    return success;
}

typedef struct _CE_BRIDGE_NOREPLY_JOB {
    BYTE  cmd;
    char* payload;
    DWORD len;
} CE_BRIDGE_NOREPLY_JOB;

// Fire-and-forget request.  CE may send response frames back; closing the
// pipe immediately is safe because its sendFrame() wraps writes in pcall().
// Runs on a worker thread so a stuck CE listener can never block document
// switching or application shutdown.
static unsigned __stdcall _NoReplyThread(void* arg)
{
    CE_BRIDGE_NOREPLY_JOB* const job = (CE_BRIDGE_NOREPLY_JOB*)arg;
    if (!job) {
        _endthreadex(0);
        return 0;
    }
    if (WaitNamedPipeW(CE_BRIDGE_PIPE_W, CE_BRIDGE_NOREPLY_WAIT_MS)) {
        HANDLE pipe = CreateFileW(CE_BRIDGE_PIPE_W, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                  OPEN_EXISTING, 0, NULL);
        if (pipe != INVALID_HANDLE_VALUE) {
            size_t frameLen = 0;
            BYTE* frame = (BYTE*)AllocMem(5 + (size_t)job->len, 0);
            if (frame && _CeBridge_EncodeFrame(frame, 5 + (size_t)job->len, job->cmd,
                                               job->payload, job->len, &frameLen)) {
                _WriteAll(pipe, frame, (DWORD)frameLen);
            }
            if (frame) {
                FreeMem(frame);
            }
            CloseHandle(pipe);
        }
    }
    if (job->payload) {
        FreeMem(job->payload);
    }
    FreeMem(job);
    _endthreadex(0);
    return 0;
}

void CeBridge_SendNoReply(BYTE cmd, const char* payload, DWORD len)
{
    if (len > 0 && !payload) {
        return;
    }
    CE_BRIDGE_NOREPLY_JOB* const job =
        (CE_BRIDGE_NOREPLY_JOB*)AllocMem(sizeof(CE_BRIDGE_NOREPLY_JOB), HEAP_ZERO_MEMORY);
    if (!job) {
        return;
    }
    job->cmd = cmd;
    job->len = len;
    if (len > 0) {
        job->payload = (char*)AllocMem((size_t)len + 1, HEAP_ZERO_MEMORY);
        if (!job->payload) {
            FreeMem(job);
            return;
        }
        memcpy(job->payload, payload, (size_t)len);
    }
    HANDLE const thread = (HANDLE)_beginthreadex(NULL, 0, _NoReplyThread, job, 0, NULL);
    if (!thread) {
        if (job->payload) {
            FreeMem(job->payload);
        }
        FreeMem(job);
        return;
    }
    CloseHandle(thread);
}

// ----------------------------------------------------------------------------
// Online syntax check using CE's own autoAssembleCheck (CMD_CHECK_SCRIPT).
// It runs on a worker thread like the session apply path, and reports back via
// a payload-free WM_APP_CE_CHECK_RESULT message so no UI message carries a
// heap pointer owned by another thread.
// ----------------------------------------------------------------------------

typedef struct _CE_CHECK_JOB {
    HWND  hwnd;
    char* payload;
    DWORD len;
} CE_CHECK_JOB;

static volatile LONG s_checkBusy = 0;
// A "Code Review" command waits for the online verdict before opening its
// report, so the user sees local and CE findings in one dialog.
static bool s_reviewPending = false;
static volatile PVOID s_checkReply = NULL;

static void _CeBridge_Utf8ToWide(const char* utf8, WCHAR* out, size_t cch)
{
    if (!out || cch == 0) {
        return;
    }
    out[0] = L'\0';
    if (utf8) {
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)cch);
    }
}

static unsigned __stdcall _CheckThread(void* arg)
{
    CE_CHECK_JOB* const job = (CE_CHECK_JOB*)arg;
    if (!job) {
        InterlockedExchange(&s_checkBusy, 0);
        _endthreadex(0);
        return 0;
    }
    HWND const hwnd = job->hwnd;
    char* reply = NULL;
    DWORD replyLen = 0;
    bool const ok = CeBridge_SendRequest(CE_BRIDGE_CMD_CHECK_SCRIPT,
                                          job->payload, job->len, &reply, &replyLen);
    if (!ok && !reply) {
        reply = (char*)AllocMem(96, HEAP_ZERO_MEMORY);
        if (reply) {
            StringCchCopyA(reply, 96, "ERR:CE bridge unavailable (start or restart Cheat Engine)");
        }
    }
    FreeMem(job->payload);
    FreeMem(job);
    PVOID const stale = InterlockedExchangePointer((PVOID volatile*)&s_checkReply, reply);
    if (stale) {
        FreeMem(stale);
    }
    PostMessageW(hwnd, WM_APP_CE_CHECK_RESULT, 0, 0);
    InterlockedExchange(&s_checkBusy, 0);
    _endthreadex(0);
    return 0;
}

bool CeBridge_CheckScriptOnline(HWND hwnd)
{
    if (InterlockedCompareExchange(&s_checkBusy, 0, 0) != 0) {
        CeOutput_Append(hwnd, CE_BRIDGE_INFO, L"--- CE 在线检查：上一次检查尚未结束 ---");
        return false;
    }
    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc) || doc.len <= 0) {
        CeOutput_Append(hwnd, CE_BRIDGE_INFO, L"--- CE 在线检查：当前文档为空 ---");
        CeLint_ClearExternalDiags();
        return false;
    }
    if ((size_t)doc.len > CE_BRIDGE_MAX_FRAME - 1024) {
        CeScript_FreeDoc(&doc);
        CeOutput_Append(hwnd, CE_BRIDGE_INFO, L"--- CE 在线检查：文档过大 ---");
        return false;
    }
    char* const payload = (char*)AllocMem((size_t)doc.len + 1, HEAP_ZERO_MEMORY);
    if (!payload) {
        CeScript_FreeDoc(&doc);
        return false;
    }
    memcpy(payload, doc.text, (size_t)doc.len);
    DWORD const len = (DWORD)doc.len;
    CeScript_FreeDoc(&doc);

    CE_CHECK_JOB* const job = (CE_CHECK_JOB*)AllocMem(sizeof(CE_CHECK_JOB), HEAP_ZERO_MEMORY);
    if (!job) {
        FreeMem(payload);
        return false;
    }
    job->hwnd = IsWindow(hwnd) ? hwnd : Globals.hwndMain;
    job->payload = payload;
    job->len = len;
    InterlockedExchange(&s_checkBusy, 1);
    HANDLE const thread = (HANDLE)_beginthreadex(NULL, 0, _CheckThread, job, 0, NULL);
    if (!thread) {
        FreeMem(payload);
        FreeMem(job);
        InterlockedExchange(&s_checkBusy, 0);
        return false;
    }
    CloseHandle(thread);
    return true;
}

void CeBridge_ReportCheck(HWND hwnd)
{
    char* const reply = (char*)InterlockedExchangePointer((PVOID volatile*)&s_checkReply, NULL);
    if (!reply) {
        CeOutput_Append(hwnd, CE_BRIDGE_INFO, L"--- CE 在线检查：未收到结果 ---");
        if (s_reviewPending) {
            s_reviewPending = false;
            CeLint_ShowReview(hwnd, true);
        }
        return;
    }
    CE_EXTERNAL_DIAG diags[64] = { 0 };
    int count = 0;
    bool ok = false;
    bool first = true;
    char* line = reply;
    while (line && *line) {
        char* const eol = strpbrk(line, "\r\n");
        if (eol) {
            *eol = '\0';
        }
        size_t const n = strlen(line);
        if (n > 0) {
            if (first) {
                ok = (strncmp(line, "OK:", 3) == 0);
                if (!ok) {
                    WCHAR wide[512] = { L'\0' };
                    WCHAR w[600] = { L'\0' };
                    _CeBridge_Utf8ToWide(line, wide, COUNTOF(wide));
                    StringCchPrintfW(w, COUNTOF(w), L"--- CE 在线检查失败：%s ---", wide);
                    CeOutput_Append(hwnd, CE_BRIDGE_INFO, w);
                }
                first = false;
            }
            else {
                int ln = 0;
                bool error = false;
                char msg[CE_LINT_MSG_CCH] = { 0 };
                if (_CeBridge_ParseCheckRecord(line, n, &ln, &error, msg, sizeof(msg)) &&
                    count < (int)COUNTOF(diags)) {
                    diags[count].line = ln;
                    diags[count].error = error;
                    StringCchCopyA(diags[count].message, CE_LINT_MSG_CCH, msg);
                    ++count;
                    WCHAR wide[512] = { L'\0' };
                    WCHAR w[600] = { L'\0' };
                    _CeBridge_Utf8ToWide(msg, wide, COUNTOF(wide));
                    StringCchPrintfW(w, COUNTOF(w), L"--- CE 检查 第 %d 行：%s ---", ln, wide);
                    CeOutput_Append(hwnd, CE_BRIDGE_INFO, w);
                }
            }
        }
        if (!eol) {
            break;
        }
        line = eol + 1;
        while (*line == '\r' || *line == '\n') {
            ++line;
        }
    }
    CeLint_SetExternalDiags(diags, count);
    if (ok && count == 0) {
        CeOutput_Append(hwnd, CE_BRIDGE_INFO,
                        L"--- CE 在线检查通过：启用段/禁用段均无语法错误 ---");
    }
    else if (count > 0) {
        WCHAR w[160] = { L'\0' };
        StringCchPrintfW(w, COUNTOF(w),
                        L"--- CE 在线检查发现 %d 处问题（已标为波浪线）---", count);
        CeOutput_Append(hwnd, CE_BRIDGE_INFO, w);
    }
    CeLint_Invalidate();   // idle pass re-renders with the external findings
    FreeMem(reply);
    if (s_reviewPending) {
        s_reviewPending = false;
        CeLint_ShowReview(hwnd, true);
    }
}

// One "Code Review" entry for both checks: the local analyzer always runs, and
// CE's own autoAssembleCheck is added when the bridge is reachable.  The report
// is deferred until the online verdict arrived so nothing is shown twice.
void CeBridge_ReviewScript(HWND hwnd)
{
    if (!CeScript_IsActive()) {
        return;
    }
    bool const online = (WaitNamedPipeW(CE_BRIDGE_PIPE_W, 0) != 0);
    if (!online) {
        CeLint_ShowReview(hwnd, false);
        return;
    }
    if (!CeBridge_CheckScriptOnline(hwnd)) {
        CeLint_ShowReview(hwnd, false);
        return;
    }
    s_reviewPending = true;
}

static unsigned __stdcall _WatchThread(void* arg)
{
    HWND const hwnd = (HWND)arg;
    for (;;) {
        for (int i = 0; i < 10; ++i) {
            if (InterlockedCompareExchange(&s_ceBridgeWatchCancel, 0, 0) != 0) {
                break;
            }
            Sleep(100);
        }
        if (InterlockedCompareExchange(&s_ceBridgeWatchCancel, 0, 0) != 0) {
            break;
        }
        if (InterlockedCompareExchange(&s_ceBridgeRunning, 0, 0) == 0) {
            _BridgeSendQuiet(hwnd, s_aaWatchPollLua, (DWORD)strlen(s_aaWatchPollLua), true, false);
        }
    }
    InterlockedExchange(&s_ceBridgeWatchRun, 0);
    _endthreadex(0);
    return 0;
}

static void _StartWatch(HWND hwnd)
{
    InterlockedExchange(&s_ceBridgeWatchCancel, 0);
    if (InterlockedCompareExchange(&s_ceBridgeWatchRun, 1, 0) != 0) {
        return;   // an existing poller keeps running
    }
    uintptr_t const thread = _beginthreadex(NULL, 0, _WatchThread, (void*)hwnd, 0, NULL);
    if (thread == 0) {
        InterlockedExchange(&s_ceBridgeWatchRun, 0);
        return;
    }
    CloseHandle((HANDLE)thread);
}

static void _StopWatch(void)
{
    InterlockedExchange(&s_ceBridgeWatchCancel, 1);
}

static unsigned __stdcall _CancelThread(void* arg)
{
    CE_BRIDGE_JOB* const job = (CE_BRIDGE_JOB*)arg;
    _BridgeSendQuiet(job->hwnd, job->script, job->len, false, true);
    FreeMem(job->script);
    FreeMem(job);
    _endthreadex(0);
    return 0;
}

static unsigned __stdcall _BridgeThread(void* arg)
{
    CE_BRIDGE_JOB* const job = (CE_BRIDGE_JOB*)arg;
    HWND const hwnd = job->hwnd;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    BYTE* frame = NULL;

    // Pure Lua: run in the local Lua runtime, no CE process / pipe involved.
    // done: still posts WM_CEBRIDGE_DONE, frees the job and clears the guard.
    if (job->mode == CE_RUN_LOCAL) {
        CeLua_Execute(hwnd, job->script, job->len);
        goto done;
    }

    if (!WaitNamedPipeW(CE_BRIDGE_PIPE_W, CE_BRIDGE_WAIT_MS)) {
        CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, s_errNoPipe);
        goto done;
    }
    pipe = CreateFileW(CE_BRIDGE_PIPE_W, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, s_errOpen);
        goto done;
    }

    size_t frameLen = 0;
    frame = (BYTE*)AllocMem(5 + (size_t)job->len, 0);
    if (!frame || !_CeBridge_EncodeFrame(frame, 5 + (size_t)job->len,
                                         CE_BRIDGE_CMD_RUN, job->script, job->len, &frameLen) ||
        !_WriteAll(pipe, frame, (DWORD)frameLen)) {
        CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, s_errWrite);
        goto done;
    }
    FreeMem(frame);
    frame = NULL;

    for (;;) {
        BYTE hdr[5] = { 0 };
        if (!_ReadAll(pipe, hdr, 5)) {
            CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, s_errLost);
            break;
        }
        BYTE const kind = hdr[0];
        DWORD const len = _CeBridge_DecodeLen(hdr + 1);
        if (len > CE_BRIDGE_MAX_FRAME) {
            CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, s_errFrame);
            break;
        }
        char* const buf = (char*)AllocMem((size_t)len + 1, HEAP_ZERO_MEMORY);
        if (!buf) {
            break;
        }
        if (len > 0 && !_ReadAll(pipe, buf, len)) {
            FreeMem(buf);
            CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, s_errLost);
            break;
        }
        buf[len] = '\0';
        if (job->aa && (kind == CE_BRIDGE_RESULT) && (len == 1) &&
            ((buf[0] == '0') || (buf[0] == '1'))) {
            // AA state marker: track it instead of printing "[RESULT] 1".
            _CeBridge_SetAaActiveFromResult(buf, len);
            FreeMem(buf);
        }
        else {
            CeBridge_PostLine(hwnd, kind, buf);
        }
        if (kind == CE_BRIDGE_DONE) {
            break;
        }
    }

done:
    if (frame) {
        FreeMem(frame);
    }
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
    }
    PostMessageW(hwnd, WM_CEBRIDGE_DONE, 0, 0);
    FreeMem(job->script);
    FreeMem(job);
    InterlockedExchange(&s_ceBridgeRunning, 0);
    _endthreadex(0);
    return 0;
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

static void _DeployTitle(WCHAR* out, size_t cch)
{
    out[0] = L'\0';
    GetLngString(IDS_MUI_CE_DEPLOY_TITLE, out, (int)cch);
    if (out[0] == L'\0') {
        StringCchCopyW(out, cch, L"CE Bridge");
    }
}

static void _ReportDeployCanceled(HWND hwnd, bool interactive)
{
    WCHAR msg[512] = { L'\0' };
    GetLngString(IDS_MUI_CE_DEPLOY_CANCELLED, msg, (int)COUNTOF(msg));
    if (msg[0] == L'\0') {
        StringCchCopyW(msg, COUNTOF(msg),
                       L"Deployment cancelled: the existing bridge script was kept.");
    }
    CeOutput_Append(hwnd, CE_BRIDGE_INFO, msg);
    if (interactive) {
        WCHAR title[128] = { L'\0' };
        _DeployTitle(title, COUNTOF(title));
        MessageBoxW(hwnd, msg, title, MB_OK | MB_ICONINFORMATION);
    }
}

// Report the deployed/exported location in the merged output panel and, when
// the user asked for it explicitly, in a message box.
static void _ReportDeploy(HWND hwnd, bool interactive, bool deployed, LPCWSTR path)
{
    WCHAR msg[MAX_PATH * 4] = { L'\0' };
    WCHAR title[128] = { L'\0' };
    _DeployTitle(title, COUNTOF(title));

    if (!deployed) {
        if (path && path[0]) {
            FormatLngStringW(msg, (int)COUNTOF(msg), IDS_MUI_CE_DEPLOY_NO_TARGET, path);
        }
        else {
            FormatLngStringW(msg, (int)COUNTOF(msg), IDS_MUI_CE_DEPLOY_NO_TARGET_ANY);
        }
        if (msg[0] == L'\0') {
            StringCchCopyW(msg, COUNTOF(msg),
                L"No Cheat Engine installation was found. Set CeInstallPath in "
                L"[Settings2] of Notepad3.ini.");
        }
        CeOutput_Append(hwnd, CE_BRIDGE_INFO, msg);
        if (interactive) {
            MessageBoxW(hwnd, msg, title, MB_OK | MB_ICONWARNING);
        }
        return;
    }

    FormatLngStringW(msg, (int)COUNTOF(msg), IDS_MUI_CE_DEPLOY_PATH, path ? path : L"");
    if (msg[0] == L'\0') {
        StringCchPrintfW(msg, COUNTOF(msg), L"Bridge script location: %s", path ? path : L"");
    }
    CeOutput_Append(hwnd, CE_BRIDGE_INFO, msg);
    if (!interactive) {
        return;
    }
    if (WaitNamedPipeW(CE_BRIDGE_PIPE_W, 200) != 0) {
        WCHAR info[512] = { L'\0' };
        GetLngString(IDS_MUI_CE_DEPLOY_CONNECTED, info, (int)COUNTOF(info));
        if (info[0] == L'\0') {
            StringCchCopyW(info, COUNTOF(info), L"Connected to the Cheat Engine bridge.");
        }
        MessageBoxW(hwnd, info, title, MB_OK | MB_ICONINFORMATION);
    }
    else {
        WCHAR info[MAX_PATH * 3] = { L'\0' };
        FormatLngStringW(info, (int)COUNTOF(info), IDS_MUI_CE_DEPLOY_RESTART,
                         path ? path : L"");
        if (info[0] == L'\0') {
            StringCchPrintfW(info, COUNTOF(info),
                L"Bridge script deployed to:\r\n%s\r\n\r\nStart or restart Cheat Engine to load it.",
                path ? path : L"");
        }
        MessageBoxW(hwnd, info, title, MB_OK | MB_ICONINFORMATION);
    }
}

bool CeBridge_EnsureDeployed(HWND hwnd, bool interactive)
{
    WCHAR dir[MAX_PATH] = { L'\0' };
    WCHAR path[MAX_PATH] = { L'\0' };
    bool canceled = false;
    if (!_DiscoverCeDir(dir)) {
        bool const exported = _ExportBridgeTo(hwnd, Path_Get(Paths.ModuleDirectory),
                                              interactive, path, COUNTOF(path), &canceled);
        if (!exported && canceled) {
            _ReportDeployCanceled(hwnd, interactive);
            return false;
        }
        _ReportDeploy(hwnd, interactive, false, exported ? path : NULL);
        return false;
    }
    _PersistCePath(dir);
    if (!_JoinPath(path, COUNTOF(path), dir, CE_BRIDGE_LUA_FILE)) {
        path[0] = L'\0';
    }
    if (!_DeployBridge(hwnd, dir, interactive, &canceled)) {
        if (canceled) {
            _ReportDeployCanceled(hwnd, interactive);
            return false;
        }
        // Read-only installation (e.g. below Program Files): hand the script
        // to the user instead of failing silently.
        WCHAR exportedPath[MAX_PATH] = { L'\0' };
        bool const exported = _ExportBridgeTo(hwnd, Path_Get(Paths.ModuleDirectory),
                                              interactive, exportedPath,
                                              COUNTOF(exportedPath), &canceled);
        if (!exported && canceled) {
            _ReportDeployCanceled(hwnd, interactive);
            return false;
        }
        _ReportDeploy(hwnd, interactive, false, exported ? exportedPath : NULL);
        return false;
    }
    _ReportDeploy(hwnd, interactive, true, path);
    return true;
}

bool CeBridge_GetCeDir(WCHAR out[MAX_PATH])
{
    if (!out) {
        return false;
    }
    return _DiscoverCeDir(out);
}

// Open the user-editable default-template file.  Deploy it first when missing,
// so the command always has something to edit even before CE was ever run.
void CeBridge_EditUserTemplates(HWND hwnd)
{
    WCHAR ceDir[MAX_PATH] = { L'\0' };
    if (!CeBridge_GetCeDir(ceDir)) {
        CeOutput_Append(hwnd, CE_BRIDGE_INFO,
                        L"--- 未找到 Cheat Engine 安装目录，无法定位默认模板文件 ---");
        return;
    }
    WCHAR autorun[MAX_PATH] = { L'\0' };
    WCHAR path[MAX_PATH] = { L'\0' };
    if (!_JoinPath(autorun, COUNTOF(autorun), ceDir, L"autorun") ||
        !_JoinPath(path, COUNTOF(path), autorun, L"np3_user_templates.tpl")) {
        return;
    }
    if (!_IsRegularFile(path)) {
        if (!_IsDirectory(autorun)) {
            CreateDirectoryW(autorun, NULL);
        }
        _DeployDefaultTemplates(autorun);
    }
    if (_IsRegularFile(path)) {
        // Open the template in Notepad3 itself, not through the system file
        // association.  Same mechanism as _OpenLocalDoc(): quote the file so
        // a path with spaces stays a single command-line argument.
        WCHAR exePath[MAX_PATH] = { L'\0' };
        if (GetModuleFileNameW(NULL, exePath, COUNTOF(exePath)) > 0) {
            WCHAR params[MAX_PATH + 8] = { L'\0' };
            StringCchPrintfW(params, COUNTOF(params), L"\"%s\"", path);
            if ((INT_PTR)ShellExecuteW(hwnd, L"open", exePath, params, NULL,
                                       SW_SHOWNORMAL) > 32) {
                return;
            }
        }
        ShellExecuteW(hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    }
    else {
        CeOutput_Append(hwnd, CE_BRIDGE_INFO,
                        L"--- 无法创建默认模板文件，请以管理员身份重试 ---");
    }
}

// ----------------------------------------------------------------------------
// The main menu lives in a MUI language DLL.  A DLL built before a CE command
// was added silently hides it, and some languages never had a CE menu at all.
// Every CE entry is therefore labelled from the language resources here and
// created on demand; obsolete entries are dropped.  Missing translations fall
// back to English (the base language), so no item or button can end up blank.
// ----------------------------------------------------------------------------

static bool _MenuHasCommand(HMENU hmenu, UINT cmd)
{
    int const count = GetMenuItemCount(hmenu);
    for (int i = 0; i < count; ++i) {
        if (GetMenuItemID(hmenu, i) == (UINT)cmd) {
            return true;
        }
        HMENU const sub = GetSubMenu(hmenu, i);
        if (sub && _MenuHasCommand(sub, cmd)) {
            return true;
        }
    }
    return false;
}

static HMENU _FindMenuWithCommand(HMENU hmenu, UINT cmd)
{
    int const count = GetMenuItemCount(hmenu);
    for (int i = 0; i < count; ++i) {
        HMENU const sub = GetSubMenu(hmenu, i);
        if (sub && _MenuHasCommand(sub, cmd)) {
            return sub;
        }
    }
    return NULL;
}

static int _FindCommandPosition(HMENU hmenu, UINT cmd)
{
    int const count = GetMenuItemCount(hmenu);
    for (int i = 0; i < count; ++i) {
        if (GetMenuItemID(hmenu, i) == (UINT)cmd) {
            return i;
        }
    }
    return -1;
}

// Delete every occurrence of |cmd|, also inside submenus.
static void _RemoveMenuCommand(HMENU hmenu, UINT cmd)
{
    if (!hmenu || !cmd) {
        return;
    }
    int i = GetMenuItemCount(hmenu);
    while (--i >= 0) {
        HMENU const sub = GetSubMenu(hmenu, i);
        if (sub) {
            _RemoveMenuCommand(sub, cmd);
            continue;
        }
        if (GetMenuItemID(hmenu, i) == (UINT)cmd) {
            DeleteMenu(hmenu, (UINT)i, MF_BYPOSITION);
        }
    }
}

// Removing entries leaves separators behind: drop doubled, leading and
// trailing ones so a trimmed menu still looks intentional.
static void _CollapseMenuSeparators(HMENU hmenu)
{
    int const count = GetMenuItemCount(hmenu);
    for (int i = 0; i < count; ++i) {
        HMENU const sub = GetSubMenu(hmenu, i);
        if (sub) {
            _CollapseMenuSeparators(sub);
        }
    }
    int i = 0;
    int total = GetMenuItemCount(hmenu);
    while (i < total) {
        UINT const state = GetMenuState(hmenu, (UINT)i, MF_BYPOSITION);
        bool const isSep = ((state & (MF_POPUP | MF_SEPARATOR)) == MF_SEPARATOR);
        bool const prevSep = (i > 0) &&
            ((GetMenuState(hmenu, (UINT)(i - 1), MF_BYPOSITION) & MF_SEPARATOR) != 0);
        if (isSep && ((i == 0) || (i == total - 1) || prevSep)) {
            DeleteMenu(hmenu, (UINT)i, MF_BYPOSITION);
            total = GetMenuItemCount(hmenu);
            continue;
        }
        ++i;
    }
}

typedef struct _CE_MENU_SPEC {
    UINT cmd;
    UINT ids;
    bool newGroup;   // start a separator-delimited group
} CE_MENU_SPEC;

// Order mirrors the CE popup: review/format, navigation, generators, table
// conversion, bridge/run commands, then the watch/template extras.
static const CE_MENU_SPEC s_ceMenuSpecs[] = {
    { IDM_CE_REVIEW,           IDS_MUI_CE_MENU_REVIEW,        false },
    { IDM_CE_FORMAT,           IDS_MUI_CE_MENU_FORMAT,        false },
    { IDM_CE_GOTO_DEFINITION,  IDS_MUI_CE_MENU_GOTODEF,       true  },
    { IDM_CE_GOTO_DECLARATION, IDS_MUI_CE_MENU_GOTODECL,      false },
    { IDM_CE_FIND_REFERENCES,  IDS_MUI_CE_MENU_REFERENCES,    false },
    { IDM_CE_DOC_SYMBOLS,      IDS_MUI_CE_MENU_DOCSYMBOLS,    false },
    { IDM_CE_WS_SYMBOLS,       IDS_MUI_CE_MENU_WSSYMBOLS,     false },
    { IDM_CE_SHOW_DOC,         IDS_MUI_CE_MENU_COMPLETEHELP,  false },
    { IDM_CE_GENERATE_PAIRS,   IDS_MUI_CE_MENU_GENPAIRS,      true  },
    { IDM_CE_SETTINGS,         IDS_MUI_CE_MENU_SETTINGS,      false },
    { IDM_CE_TEMPLATE_MANAGE,  IDS_MUI_CE_MENU_TEMPLATES,     false },
    { IDM_CE_COPY_SCRIPT,      IDS_MUI_CE_MENU_COPYSCRIPT,    true  },
    { IDM_CE_COPY_XML,         IDS_MUI_CE_MENU_COPYXML,       false },
    { IDM_CE_PASTE_XML,        IDS_MUI_CE_MENU_PASTEXML,      false },
    { IDM_CE_IMPORT_TABLE,     IDS_MUI_CE_MENU_IMPORT,        true  },
    { IDM_CE_EXPORT_TABLE,     IDS_MUI_CE_MENU_EXPORT,        false },
    { IDM_CE_BRIDGE_RUN,       IDS_MUI_CE_MENU_RUN,           true  },
    { IDM_CE_BRIDGE_DEPLOY,    IDS_MUI_CE_MENU_DEPLOY,        false },
    { IDM_CE_BRIDGE_OUTPUT,    IDS_MUI_CE_MENU_OUTPUT,        false },
    { IDM_CE_LOG_CLEAR,        IDS_MUI_CE_MENU_LOGCLEAR,      false },
    { IDM_CE_LOG_FLOAT,        IDS_MUI_CE_MENU_LOGFLOAT,      false },
    { IDM_CE_AA_CANCEL,        IDS_MUI_CE_MENU_CANCELRUN,     true  },
    { IDM_CE_WATCH_CLEAR,      IDS_MUI_CE_MENU_WATCHCLEAR,    false },
    { IDM_CE_ADD_LIST,         IDS_MUI_CE_MENU_ADDLIST,       false },
    { IDM_CE_EDIT_TEMPLATES,   IDS_MUI_CE_MENU_EDITTEMPLATES, false }
};

static bool _MenuText(UINT ids, LPWSTR out, size_t cch)
{
    out[0] = L'\0';
    if (!GetLngString(ids, out, (int)cch)) {
        return false;
    }
    out[cch - 1] = L'\0';
    return (out[0] != L'\0');
}

// Label an existing item from the language resources, or insert it at its
// place in the fixed order when the language DLL predates the command.
// |lastPos| is the position of the previously handled entry (-1 = none).
static int _SyncMenuItem(HMENU hmenu, const CE_MENU_SPEC* spec, int lastPos)
{
    WCHAR text[512] = { L'\0' };
    if (!_MenuText(spec->ids, text, COUNTOF(text))) {
        return lastPos;
    }
    int const pos = _FindCommandPosition(hmenu, spec->cmd);
    if (pos >= 0) {
        ModifyMenuW(hmenu, (UINT)pos, MF_BYPOSITION | MF_STRING, spec->cmd, text);
        return pos;
    }
    int insertAt = (lastPos >= 0) ? (lastPos + 1) : 0;
    if (spec->newGroup && (insertAt > 0)) {
        InsertMenuW(hmenu, (UINT)insertAt, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
        ++insertAt;
    }
    InsertMenuW(hmenu, (UINT)insertAt, MF_BYPOSITION | MF_STRING, spec->cmd, text);
    return insertAt;
}

static HMENU _EnsureCeSubmenu(HMENU hmenu)
{
    HMENU ce = _FindMenuWithCommand(hmenu, IDM_CE_FORMAT);
    if (ce) {
        return ce;
    }
    WCHAR caption[80] = { L'\0' };
    if (!_MenuText(IDS_MUI_CE_MENU_ROOT, caption, COUNTOF(caption))) {
        StringCchCopyW(caption, COUNTOF(caption), L"&CE");
    }
    ce = CreatePopupMenu();
    if (!ce) {
        return NULL;
    }
    // Insert in front of Help when possible, otherwise append.
    int const count = GetMenuItemCount(hmenu);
    int pos = count;
    for (int i = 0; i < count; ++i) {
        HMENU const sub = GetSubMenu(hmenu, i);
        if (sub && _MenuHasCommand(sub, IDM_HELP_ABOUT)) {
            pos = i;
            break;
        }
    }
    if (!InsertMenuW(hmenu, (UINT)pos, MF_BYPOSITION | MF_POPUP, (UINT_PTR)ce, caption)) {
        DestroyMenu(ce);
        return NULL;
    }
    return ce;
}

void CeBridge_EnsureRunCommands(HMENU hmenu)
{
    if (!hmenu) {
        return;
    }
    // Merged/obsolete commands: one "Run Script" entry decides by itself
    // whether Cheat Engine has to be involved, and check/review are one command.
    _RemoveMenuCommand(hmenu, IDM_CE_CHECK);
    _RemoveMenuCommand(hmenu, IDM_CE_CHECK_ONLINE);
    _RemoveMenuCommand(hmenu, IDM_CE_RUN_LOCAL);
    _RemoveMenuCommand(hmenu, IDM_CE_RUN_CE);

    HMENU const ce = _EnsureCeSubmenu(hmenu);
    if (!ce) {
        return;
    }
    int lastPos = -1;
    for (size_t i = 0; i < COUNTOF(s_ceMenuSpecs); ++i) {
        lastPos = _SyncMenuItem(ce, &s_ceMenuSpecs[i], lastPos);
    }
    _CollapseMenuSeparators(ce);
}

// ----------------------------------------------------------------------------
// Local documentation (Help > 本地文档).  The full keyword reference is
// generated from ce_completions_data.c and embedded in the executable as the
// IDR_CE_LOCALDOC resource — the same mechanism as the CE bridge script, so
// the reference travels with the program.  When no 本地文档.txt exists next to
// the executable it is extracted from that resource into the program directory
// (falling back to %TEMP%), so the file is always available and self-generated.
// F1 over a completion item and the Help ▸ 本地文档 command both open this file;
// F1 additionally jumps to the selected keyword's entry.
// ----------------------------------------------------------------------------

void CeBridge_EnsureLocalDocCommand(HMENU hmenu)
{
    if (!hmenu) {
        return;
    }
    // The keyword/function reference and the generic local documentation used
    // the same file for the same purpose, so only the caret-aware query stays.
    _RemoveMenuCommand(hmenu, IDM_HELP_LOCALDOCUMENTATION);
    // Notepad3-only Help entries have no meaning in CEsript.
    _RemoveMenuCommand(hmenu, IDM_HELP_ONLINEDOCUMENTATION);
    _RemoveMenuCommand(hmenu, IDM_HELP_ADMINEXE);
    _RemoveMenuCommand(hmenu, IDM_HELP_UPDATEWEBSITE);

    HMENU const help = _FindMenuWithCommand(hmenu, IDM_HELP_ABOUT);
    if (!help) {
        return;
    }
    // "Command Line Help" now also carries the shortcut list.
    WCHAR text[512] = { L'\0' };
    if (_MenuText(IDS_MUI_HELP_SHORTCUTS, text, COUNTOF(text))) {
        int const pos = _FindCommandPosition(help, IDM_HELP_CMD);
        if (pos >= 0) {
            ModifyMenuW(help, (UINT)pos, MF_BYPOSITION | MF_STRING, IDM_HELP_CMD, text);
        }
    }
    if (!_MenuHasCommand(help, IDM_CE_DOC_QUERY) &&
        _MenuText(IDS_MUI_CE_MENU_DOCQUERY, text, COUNTOF(text))) {
        int const pos = _FindCommandPosition(help, IDM_HELP_CMD);
        int const insertAt = (pos >= 0) ? (pos + 1) : GetMenuItemCount(help);
        InsertMenuW(help, (UINT)insertAt, MF_BYPOSITION | MF_STRING, IDM_CE_DOC_QUERY, text);
    }
    _CollapseMenuSeparators(hmenu);
}

// Menu command: look up the function/keyword under the caret in the local
// reference.  The identifier at the caret is read directly (no selection
// change); when there is no word there, the reference simply opens at the top.
void CeBridge_QueryLocalDoc(HWND hwnd)
{
    char keyword[256] = { '\0' };
    DocPos const pos = SciCall_GetCurrentPos();
    DocPos const start = SciCall_WordStartPosition(pos, true);
    DocPos const end = SciCall_WordEndPosition(pos, true);
    if ((end > start) && ((end - start) < ((DocPos)COUNTOF(keyword) - 1))) {
        static char buf[256];
        struct Sci_TextRangeFull tr = { { (DocPosCR)start, (DocPosCR)end }, buf };
        buf[0] = '\0';
        DocPos const rlen = SciCall_GetTextRangeFull(&tr);
        if (rlen > 0) {
            buf[(rlen < (DocPos)COUNTOF(buf) - 1) ? rlen : (COUNTOF(buf) - 1)] = '\0';
            StringCchCopyA(keyword, COUNTOF(keyword), buf);
        }
    }
    CeBridge_ShowLocalDocAtKeyword(hwnd, keyword);
}

// Load the embedded 本地文档.txt bytes (UTF-8, BOM included).
static const char* _LoadLocalDocResource(size_t* lenOut)
{
    *lenOut = 0;
    HMODULE const hInst = GetModuleHandleW(NULL);
    HRSRC const hRes = FindResourceExW(hInst, RT_RCDATA, MAKEINTRESOURCEW(IDR_CE_LOCALDOC),
                                       MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
    if (!hRes) {
        return NULL;
    }
    HGLOBAL const hMem = LoadResource(hInst, hRes);
    if (!hMem) {
        return NULL;
    }
    DWORD const size = SizeofResource(hInst, hRes);
    const char* const data = (const char*)LockResource(hMem);
    if (!data || (size == 0)) {
        return NULL;
    }
    *lenOut = (size_t)size;
    return data;
}

// Directory of the running executable.
static bool _ProgramDir(LPWSTR pszDir, size_t cchDir)
{
    WCHAR exePath[MAX_PATH] = { L'\0' };
    if (GetModuleFileNameW(NULL, exePath, COUNTOF(exePath)) == 0) {
        return false;
    }
    StringCchCopyW(pszDir, cchDir, exePath);
    LPWSTR const sep = wcsrchr(pszDir, L'\\');
    if (sep) {
        *sep = L'\0';
    }
    return true;
}

// Existing 本地文档.txt: program dir first, then walking up to the project root.
static bool _LocateLocalDoc(LPCWSTR dir, LPWSTR out, size_t cap)
{
    static const WCHAR* const upDirs[] = { L"", L"..\\", L"..\\..\\", L"..\\..\\..\\" };
    for (size_t i = 0; i < COUNTOF(upDirs); ++i) {
        WCHAR cand[MAX_PATH] = { L'\0' };
        if (SUCCEEDED(StringCchPrintfW(cand, COUNTOF(cand), L"%s\\%s本地文档.txt", dir, upDirs[i])) &&
            (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES)) {
            StringCchCopyW(out, cap, cand);
            return true;
        }
    }
    return false;
}

// Ensure the reference file exists on disk; extract it from the embedded
// resource when missing.  Returns the full path in out.
static bool _EnsureLocalDocPath(HWND hwnd, LPWSTR out, size_t cap)
{
    WCHAR dir[MAX_PATH] = { L'\0' };
    if (!_ProgramDir(dir, COUNTOF(dir))) {
        MessageBoxW(hwnd, L"无法定位程序路径,无法打开本地文档。", L"本地文档", MB_OK | MB_ICONERROR);
        return false;
    }
    if (_LocateLocalDoc(dir, out, cap)) {
        return true;
    }
    size_t len = 0;
    const char* const data = _LoadLocalDocResource(&len);
    if (!data || (len == 0)) {
        MessageBoxW(hwnd, L"本地文档资源缺失,无法生成文档文件。", L"本地文档", MB_OK | MB_ICONERROR);
        return false;
    }
    // Prefer the program directory; fall back to %TEMP% when it is read-only.
    if (SUCCEEDED(StringCchPrintfW(out, cap, L"%s\\本地文档.txt", dir)) && _WriteLuaFile(out, data, len)) {
        return true;
    }
    WCHAR tmp[MAX_PATH + 16] = { L'\0' };
    if (GetTempPathW(COUNTOF(tmp), tmp) > 0 &&
        SUCCEEDED(StringCchPrintfW(out, cap, L"%s本地文档.txt", tmp)) &&
        _WriteLuaFile(out, data, len)) {
        return true;
    }
    MessageBoxW(hwnd, L"无法在程序目录生成本地文档。", L"本地文档", MB_OK | MB_ICONERROR);
    return false;
}

// Open the reference file, optionally jumping to a 1-based line.
static void _OpenLocalDoc(HWND hwnd, LPCWSTR docFile, int line)
{
    WCHAR exePath[MAX_PATH] = { L'\0' };
    GetModuleFileNameW(NULL, exePath, COUNTOF(exePath));
    WCHAR params[MAX_PATH + 32] = { L'\0' };
    if (line > 0) {
        StringCchPrintfW(params, COUNTOF(params), L"/g %i,1 \"%s\"", line, docFile);
    }
    else {
        StringCchPrintfW(params, COUNTOF(params), L"\"%s\"", docFile);
    }
    if ((INT_PTR)ShellExecuteW(hwnd, L"open", exePath, params, NULL, SW_SHOWNORMAL) > 32) {
        return;
    }
    if ((INT_PTR)ShellExecuteW(hwnd, L"open", docFile, NULL, NULL, SW_SHOWNORMAL) <= 32) {
        MessageBoxW(hwnd, L"无法打开本地文档。", L"本地文档", MB_OK | MB_ICONERROR);
    }
}

// 1-based line of the entry "◆ <keyword>" in the reference file, or 0 when the
// keyword has no entry (e.g. a user-defined document symbol).
static int _FindDocLineForKeyword(LPCWSTR docFile, const char* keyword)
{
    if (StrIsEmptyA(keyword)) {
        return 0;
    }
    HANDLE const file = CreateFileW(docFile, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD const size = GetFileSize(file, NULL);
    if ((size == INVALID_FILE_SIZE) || (size == 0)) {
        CloseHandle(file);
        return 0;
    }
    char* const buf = (char*)AllocMem((size_t)size + 1, HEAP_ZERO_MEMORY);
    if (!buf) {
        CloseHandle(file);
        return 0;
    }
    DWORD read = 0;
    if (!ReadFile(file, buf, size, &read, NULL) || (read == 0)) {
        FreeMem(buf);
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);

    char target[288] = { '\0' };
    // ◆ = U+25C6 = E2 97 86
    StringCchPrintfA(target, COUNTOF(target), "\xE2\x97\x86 %s", keyword);
    size_t const targetLen = strlen(target);

    int line = 1;
    int found = 0;
    const char* p = buf;
    // skip a UTF-8 BOM so line 1 is counted correctly
    if ((read >= 3) && ((unsigned char)p[0] == 0xEF) && ((unsigned char)p[1] == 0xBB) && ((unsigned char)p[2] == 0xBF)) {
        p += 3;
    }
    while (*p && !found) {
        const char* eol = strchr(p, '\n');
        size_t const lineLen = eol ? (size_t)(eol - p) : strlen(p);
        if ((lineLen >= targetLen) && (strncmp(p, target, targetLen) == 0)) {
            // require a boundary after the keyword so "read" does not match "readInteger"
            char const next = (lineLen > targetLen) ? p[targetLen] : '\0';
            if (!(isalnum((unsigned char)next) || (next == '_') || (next == ':'))) {
                found = line;
            }
        }
        if (!eol) {
            break;
        }
        ++line;
        p = eol + 1;
    }
    FreeMem(buf);
    return found;
}

void CeBridge_ShowLocalDoc(HWND hwnd)
{
    WCHAR docFile[MAX_PATH] = { L'\0' };
    if (_EnsureLocalDocPath(hwnd, docFile, COUNTOF(docFile))) {
        _OpenLocalDoc(hwnd, docFile, 0);
    }
}

void CeBridge_ShowLocalDocAtKeyword(HWND hwnd, const char* keyword)
{
    WCHAR docFile[MAX_PATH] = { L'\0' };
    if (!_EnsureLocalDocPath(hwnd, docFile, COUNTOF(docFile))) {
        return;
    }
    int const line = StrIsEmptyA(keyword) ? 0 : _FindDocLineForKeyword(docFile, keyword);
    _OpenLocalDoc(hwnd, docFile, line);
}

static bool _StartRun(HWND hwnd, CE_RUN_MODE mode)
{
    if (InterlockedCompareExchange(&s_ceBridgeRunning, 1, 0) != 0) {
        MessageBoxW(hwnd, L"上一次脚本仍在执行。", L"CE 桥接", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    InterlockedExchange(&s_ceBridgeWatchArm, 0);

    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc)) {
        InterlockedExchange(&s_ceBridgeRunning, 0);
        MessageBoxW(hwnd, L"当前文档为空。", L"CE 桥接", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (doc.len <= 0 || (size_t)doc.len > CE_BRIDGE_MAX_FRAME) {
        CeScript_FreeDoc(&doc);
        InterlockedExchange(&s_ceBridgeRunning, 0);
        MessageBoxW(hwnd, L"脚本文档过大或为空。", L"CE 桥接", MB_OK | MB_ICONWARNING);
        return false;
    }

    char* script = (char*)AllocMem((size_t)doc.len + 1, HEAP_ZERO_MEMORY);
    if (script) {
        memcpy(script, doc.text, (size_t)doc.len);
    }
    DWORD len = (DWORD)doc.len;

    // AA documents must be handed to CE's autoAssemble(), not loaded as Lua.
    CE_SECTION sections[4] = { 0 };
    int const sectionCount = CeScript_CollectSections(&doc, sections, COUNTOF(sections));
    bool hasEnable = false;
    bool hasDisable = false;
    bool duplicateSections = false;
    for (int i = 0; i < sectionCount; ++i) {
        bool* const flag = sections[i].enable ? &hasEnable : &hasDisable;
        if (*flag) {
            duplicateSections = true;
        }
        *flag = true;
    }
    bool isAa = (sectionCount > 0);
    if (!isAa) {
        // Marker-less AA scripts still contain CE-only directives (alloc/aobscan).
        for (DocLn line = 0; line < doc.lines && !isAa; ++line) {
            DocPos n = 0;
            const char* const p = CeScript_LinePtr(&doc, line, &n);
            isAa = _CeBridge_IsAaDeclaration(p, (size_t)n);
        }
    }
    CeScript_FreeDoc(&doc);
    if (!script) {
        InterlockedExchange(&s_ceBridgeRunning, 0);
        return false;
    }
    if (isAa && (sectionCount > 0) && (!hasEnable || !hasDisable || duplicateSections)) {
        FreeMem(script);
        InterlockedExchange(&s_ceBridgeRunning, 0);
        MessageBoxW(hwnd, L"AA 脚本必须各包含一个 [ENABLE] 与 [DISABLE] 段。", L"CE 桥接", MB_OK | MB_ICONWARNING);
        return false;
    }

    CE_RUN_MODE const runMode = (mode == CE_RUN_LOCAL) ? CE_RUN_LOCAL
        : ((mode == CE_RUN_CE || isAa || CeLua_IsCeRelated(script, len)) ? CE_RUN_CE : CE_RUN_LOCAL);

    bool const aaExec = (isAa && (runMode == CE_RUN_CE));
    LPCWSTR const currentPath = Path_Get(Paths.CurrentFile);
    if (aaExec && (!currentPath || !currentPath[0])) {
        FreeMem(script);
        InterlockedExchange(&s_ceBridgeRunning, 0);
        MessageBoxW(hwnd, L"启用 AA 脚本前请先保存文件。", L"CE 桥接", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (aaExec) {
        char keyHex[17] = { 0 };
        _CeBridge_AaStateKey(currentPath, keyHex);
        char* const ownerScript = (char*)AllocMem((size_t)len + 1, HEAP_ZERO_MEMORY);
        if (ownerScript) {
            memcpy(ownerScript, script, (size_t)len);
        }
        size_t const cap = (size_t)len + 16384;
        char* const req = (char*)AllocMem(cap, HEAP_ZERO_MEMORY);
        size_t reqLen = 0;
        if (!ownerScript || !req || !_CeBridge_BuildAaRequest(req, cap, script, (size_t)len, keyHex,
                                                              hasDisable, &reqLen) ||
            (reqLen > CE_BRIDGE_MAX_FRAME)) {
            FreeMem(ownerScript);
            if (req) {
                FreeMem(req);
            }
            FreeMem(script);
            InterlockedExchange(&s_ceBridgeRunning, 0);
            MessageBoxW(hwnd, L"无法生成 AA 执行请求（脚本过大）。", L"CE 桥接", MB_OK | MB_ICONERROR);
            return false;
        }
        _CeBridge_AaOwnerSet(keyHex, ownerScript, len);
        FreeMem(ownerScript);
        FreeMem(script);
        script = req;
        len = (DWORD)reqLen;
    }

    CE_BRIDGE_JOB* const job = (CE_BRIDGE_JOB*)AllocMem(sizeof(CE_BRIDGE_JOB), HEAP_ZERO_MEMORY);
    if (!job) {
        FreeMem(script);
        InterlockedExchange(&s_ceBridgeRunning, 0);
        return false;
    }
    job->hwnd   = IsWindow(hwnd) ? hwnd : Globals.hwndMain;
    job->script = script;
    job->len    = len;
    job->mode   = runMode;
    job->aa     = aaExec;

    // The log window is not forced open here: it opens itself as soon as the
    // script prints something (or the user opens it with the shortcut).
    CeOutput_Append(job->hwnd, CE_BRIDGE_INFO,
        (runMode == CE_RUN_LOCAL)
            ? L"--- 本地运行 Lua / Run Lua Locally (no CE link) ---"
            : (aaExec ? L"--- 执行 AA / Run AA (CE autoAssemble; 再次运行可禁用) ---"
                      : L"--- 运行脚本 / Run in Cheat Engine ---"));
    if (runMode == CE_RUN_CE) {
        // Only CE-related scripts deploy/connect the CE bridge.
        CeBridge_EnsureDeployed(job->hwnd, false);
    }

    if (aaExec) {
        InterlockedExchange(&s_ceBridgeWatchArm, 1);
    }
    uintptr_t const thread = _beginthreadex(NULL, 0, _BridgeThread, job, 0, NULL);
    if (thread == 0) {
        InterlockedExchange(&s_ceBridgeWatchArm, 0);
        FreeMem(job->script);
        FreeMem(job);
        InterlockedExchange(&s_ceBridgeRunning, 0);
        MessageBoxW(hwnd, L"无法启动脚本线程。", L"CE 桥接", MB_OK | MB_ICONERROR);
        return false;
    }
    CloseHandle((HANDLE)thread);
    return true;
}

bool CeBridge_RunCurrentScript(HWND hwnd)
{
    return _StartRun(hwnd, CE_RUN_AUTO);
}

bool CeBridge_RunCurrentScriptEx(HWND hwnd, CE_RUN_MODE mode)
{
    return _StartRun(hwnd, mode);
}

// Build a CE-side cancel/disable request from the current document.  Returns
// NULL when the document is empty or the request cannot be built; |empty|
// distinguishes the two cases for the caller's message.  The caller owns the
// returned buffer.
static char* _CeBridge_BuildCancelFromDoc(DWORD* outLen, bool* empty)
{
    if (outLen) {
        *outLen = 0;
    }
    if (empty) {
        *empty = false;
    }

    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc) || (doc.len <= 0)) {
        CeScript_FreeDoc(&doc);
        if (empty) {
            *empty = true;
        }
        return NULL;
    }
    char* const script = (char*)AllocMem((size_t)doc.len + 1, HEAP_ZERO_MEMORY);
    DWORD const len = (DWORD)doc.len;
    if (script) {
        memcpy(script, doc.text, (size_t)doc.len);
    }
    CeScript_FreeDoc(&doc);
    if (!script) {
        return NULL;
    }

    char keyHex[17] = { 0 };
    _CeBridge_AaStateKey(Path_Get(Paths.CurrentFile), keyHex);
    size_t const cap = (size_t)len + 4096;
    char* const req = (char*)AllocMem(cap, HEAP_ZERO_MEMORY);
    size_t reqLen = 0;
    if (!req || !_CeBridge_BuildAaCancelRequest(req, cap, script, (size_t)len,
                                                keyHex, &reqLen)) {
        if (req) {
            FreeMem(req);
        }
        FreeMem(script);
        return NULL;
    }
    FreeMem(script);
    if (outLen) {
        *outLen = (DWORD)reqLen;
    }
    return req;
}

void CeBridge_CancelRun(HWND hwnd)
{
    _StopWatch();
    HWND const main = IsWindow(hwnd) ? hwnd : Globals.hwndMain;
    CeOutput_Append(main, CE_BRIDGE_INFO, L"--- 取消执行 / Cancel (停止刷新并回滚) ---");

    bool empty = false;
    DWORD len = 0;
    char* req = _CeBridge_AaOwnerCancelRequest(&len);
    if (!req) {
        req = _CeBridge_BuildCancelFromDoc(&len, &empty);
    }
    if (!req) {
        CeOutput_Append(main, CE_BRIDGE_ERROR, empty
            ? L"当前文档为空，无法执行禁用段；已停止刷新。"
            : L"无法生成取消请求；已停止刷新。");
        return;
    }

    CE_BRIDGE_JOB* const job = (CE_BRIDGE_JOB*)AllocMem(sizeof(CE_BRIDGE_JOB), HEAP_ZERO_MEMORY);
    if (!job) {
        FreeMem(req);
        return;
    }
    job->hwnd   = main;
    job->script = req;
    job->len    = len;
    job->mode   = CE_RUN_CE;
    uintptr_t const thread = _beginthreadex(NULL, 0, _CancelThread, job, 0, NULL);
    if (thread == 0) {
        FreeMem(req);
        FreeMem(job);
        return;
    }
    CloseHandle((HANDLE)thread);
    InterlockedExchange(&s_ceBridgeAaActive, 0);
    _CeBridge_AaOwnerClear();
}

bool CeBridge_IsAaRunning(void)
{
    return InterlockedCompareExchange(&s_ceBridgeAaActive, 0, 0) != 0;
}

void CeBridge_ShutdownAa(HWND hwnd)
{
    _StopWatch();
    if (!CeBridge_IsAaRunning() && !s_aaOwner.script) {
        return;
    }
    DWORD len = 0;
    char* req = _CeBridge_AaOwnerCancelRequest(&len);
    if (!req) {
        req = _CeBridge_BuildCancelFromDoc(&len, NULL);
    }
    if (!req) {
        return;
    }
    CE_BRIDGE_JOB* const job = (CE_BRIDGE_JOB*)AllocMem(sizeof(CE_BRIDGE_JOB), HEAP_ZERO_MEMORY);
    if (!job) {
        FreeMem(req);
        return;
    }
    job->hwnd   = IsWindow(hwnd) ? hwnd : Globals.hwndMain;
    job->script = req;
    job->len    = len;
    job->mode   = CE_RUN_CE;
    uintptr_t const thread = _beginthreadex(NULL, 0, _CancelThread, job, 0, NULL);
    if (thread == 0) {
        FreeMem(req);
        FreeMem(job);
        return;
    }
    // Bounded wait: a hung / exiting Cheat Engine must not block the close.
    WaitForSingleObject((HANDLE)thread, CE_BRIDGE_SHUTDOWN_WAIT_MS);
    CloseHandle((HANDLE)thread);
    InterlockedExchange(&s_ceBridgeAaActive, 0);
    _CeBridge_AaOwnerClear();
}

void CeBridge_OnDocumentClosed(HWND hwnd)
{
    LPCWSTR const currentPath = Path_Get(Paths.CurrentFile);
    if (!currentPath || !currentPath[0]) {
        return;
    }
    char keyHex[17] = { 0 };
    _CeBridge_AaStateKey(currentPath, keyHex);
    if (!_CeBridge_AaOwnerMatches(keyHex)) {
        return;   // another document owns the AA script; switching must not disable it
    }
    CeBridge_ShutdownAa(hwnd);
}

void CeBridge_OnOutput(WPARAM kind, LPARAM textUtf8)
{
    char* const utf8 = (char*)textUtf8;
    if (!utf8) {
        return;
    }
    int const cch = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (cch > 0) {
        WCHAR* const wide = (WCHAR*)AllocMem((size_t)cch * sizeof(WCHAR), HEAP_ZERO_MEMORY);
        if (wide) {
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, cch);
            CeOutput_Append(Globals.hwndMain, (int)kind, wide);
            FreeMem(wide);
        }
    }
    FreeMem(utf8);
}

void CeBridge_OnDone(WPARAM status)
{
    UNREFERENCED_PARAMETER(status);
    CeOutput_Append(Globals.hwndMain, CE_BRIDGE_INFO, L"--- 完成 / Finished ---");
    if (InterlockedCompareExchange(&s_ceBridgeWatchArm, 0, 1) == 1) {
        _StartWatch(Globals.hwndMain);
    }
}

// Modal hint for a missing environment (e.g. no local Lua runtime).  It is
// shown on the main thread and also kept in the log panel.
void CeBridge_OnHint(const char* textUtf8)
{
    char* const utf8 = (char*)textUtf8;
    if (!utf8) {
        return;
    }
    WCHAR wide[1024] = { L'\0' };
    int const cch = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, COUNTOF(wide));
    if (cch > 0) {
        CeOutput_Append(Globals.hwndMain, CE_BRIDGE_ERROR, wide);
        MessageBoxW(Globals.hwndMain, wide, L"CE 桥接 / CE Bridge",
                    MB_OK | MB_ICONWARNING);
    }
    FreeMem(utf8);
}

void CeBridge_Destroy(void)
{
    _StopWatch();
    _CeBridge_AaOwnerClear();
    CeOutput_Destroy();
}
