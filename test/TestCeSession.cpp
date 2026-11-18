// encoding: UTF-8
// Focused harness for the CE editor session <-> CE address-list bridge.
// Pure helpers are extracted from src/CeSession.c and src/CeBridge.c by
// TestCeSession.ps1, so no Win32 or Scintilla linkage is needed here.
#include <cstdio>
#include <cstring>
#include <cstdlib>

typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef char CHAR;
typedef int BOOL;

#define CE_BRIDGE_MAX_FRAME (8 * 1024 * 1024)
#define CE_SESSION_APPLY_SCRIPT (3)
#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

#include "CeSessionHelpers.inc"

static int g_failures = 0;
#define CHECK(expr) do { \
    if (!(expr)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++g_failures; \
    } \
} while (0)

int main()
{
    // 1. Request frame: [cmd][uint32 LE len][payload], binary safe.
    {
        BYTE buf[64] = { 0 };
        const char payload[] = { 'a', 'b', 'c', '\0', 'd', 'e', 'f' };
        size_t outLen = 0;
        CHECK(_CeBridge_EncodeFrame(buf, sizeof(buf), CE_SESSION_APPLY_SCRIPT,
                                    payload, (DWORD)sizeof(payload), &outLen));
        CHECK(outLen == 5 + sizeof(payload));
        CHECK(buf[0] == CE_SESSION_APPLY_SCRIPT);
        CHECK(buf[1] == 7 && buf[2] == 0 && buf[3] == 0 && buf[4] == 0);
        CHECK(std::memcmp(buf + 5, payload, sizeof(payload)) == 0);
        CHECK(!_CeBridge_EncodeFrame(buf, 11, CE_SESSION_APPLY_SCRIPT,
                                     payload, (DWORD)sizeof(payload), &outLen));
    }

    // 2. APPLY payload: "sessionId|memrecId\n<script UTF-8>", binary safe.
    {
        char buf[128] = { 0 };
        const char script[] = "[ENABLE]\nalloc(newmem,8)\n\0tail";
        size_t scriptLen = sizeof(script) - 1; // include the embedded NUL
        size_t outLen = 0;
        CHECK(_CeSession_BuildApplyPayload(buf, sizeof(buf), "s-42", 0,
                                           script, scriptLen, &outLen));
        CHECK(outLen == 7 + scriptLen);
        CHECK(std::memcmp(buf, "s-42|0\n", 7) == 0);
        CHECK(std::memcmp(buf + 7, script, scriptLen) == 0);
        CHECK(!_CeSession_BuildApplyPayload(buf, 6, "s-42", 0,
                                            script, scriptLen, &outLen));
    }

    // 3. Session meta file parsing (key=value lines, UTF-8 values).
    {
        CE_SESSION_META meta;
        std::memset(&meta, 0, sizeof(meta));
        const char text[] =
            "version=1\n"
            "id=s-42\n"
            "memrecId=0\n"
            "description=Code Injection\n"
            "address=00401000\n"
            "templateCaption=Code Injection\n";
        CHECK(_CeSession_ParseMeta(text, sizeof(text) - 1, &meta));
        CHECK(std::strcmp(meta.id, "s-42") == 0);
        CHECK(meta.memrecId == 0);
        CHECK(std::strcmp(meta.description, "Code Injection") == 0);
        CHECK(std::strcmp(meta.address, "00401000") == 0);
    }
    {
        CE_SESSION_META meta;
        std::memset(&meta, 0, sizeof(meta));
        const char text[] = "version=1\nid=s-7\nmemrecId=123\n";
        CHECK(_CeSession_ParseMeta(text, sizeof(text) - 1, &meta));
        CHECK(meta.memrecId == 123);
        CHECK(meta.format == 0); // no request: an existing record's script
    }
    {
        // Generated template code asks for a one-time format after the load.
        CE_SESSION_META meta;
        std::memset(&meta, 0, sizeof(meta));
        const char text[] = "version=1\nid=s-9\nmemrecId=0\nformat=1\n";
        CHECK(_CeSession_ParseMeta(text, sizeof(text) - 1, &meta));
        CHECK(meta.format == 1);
        CE_SESSION_META plain;
        std::memset(&plain, 0, sizeof(plain));
        const char off[] = "version=1\nid=s-9\nformat=0\n";
        CHECK(_CeSession_ParseMeta(off, sizeof(off) - 1, &plain));
        CHECK(plain.format == 0);
    }

    // 4. Reply parsing: OK:<id> / ERR:<reason> / malformed.
    {
        int id = -1;
        char err[64] = { 0 };
        CHECK(_CeSession_ParseReply("OK:123", 6, &id, err, sizeof(err)));
        CHECK(id == 123);
        id = -1;
        CHECK(!_CeSession_ParseReply("ERR:no pipe", 11, &id, err, sizeof(err)));
        CHECK(std::strstr(err, "no pipe") != nullptr);
        CHECK(!_CeSession_ParseReply("garbage", 7, &id, err, sizeof(err)));
    }

    // 5. Sidecar path for a session document.
    {
        char out[64] = { 0 };
        CHECK(_CeSession_MetaPathFromScript("C:\\tmp\\script.cea", out, sizeof(out)));
        CHECK(std::strcmp(out, "C:\\tmp\\script.cea.np3ce") == 0);
        CHECK(!_CeSession_MetaPathFromScript("C:\\tmp\\script.cea", out, 8));
    }

    // 5b. Sidecar memrecId rewrite: only that key changes.
    {
        char out[256] = { 0 };
        const char text[] =
            "version=1\n"
            "id=s-42\n"
            "memrecId=0\n"
            "description=Code Injection\n"
            "format=1\n";
        CHECK(_CeSession_ReplaceMetaMemrecId(text, sizeof(text) - 1, 77, out, sizeof(out)));
        CHECK(std::strcmp(out,
            "version=1\n"
            "id=s-42\n"
            "memrecId=77\n"
            "description=Code Injection\n"
            "format=1\n") == 0);
        // No memrecId key yet: append it, keep everything else.
        const char none[] = "version=1\nid=s-9\n";
        CHECK(_CeSession_ReplaceMetaMemrecId(none, sizeof(none) - 1, 5, out, sizeof(out)));
        CHECK(std::strcmp(out, "version=1\nid=s-9\nmemrecId=5\n") == 0);
        // A too small buffer must fail instead of truncating.
        char tiny[8] = { 0 };
        CHECK(!_CeSession_ReplaceMetaMemrecId(text, sizeof(text) - 1, 77, tiny, sizeof(tiny)));
        // An existing non-zero id is replaced, not appended.
        const char have[] = "memrecId=3\nid=x\n";
        CHECK(_CeSession_ReplaceMetaMemrecId(have, sizeof(have) - 1, 9, out, sizeof(out)));
        CHECK(std::strcmp(out, "memrecId=9\nid=x\n") == 0);
    }

    // 6. Bounded-wait deadline must be wrap-around safe (GetTickCount wraps).
    {
        CHECK(!_CeBridge_DeadlineReached(1000 + 2999, 1000, 3000));
        CHECK(_CeBridge_DeadlineReached(1000 + 3000, 1000, 3000));
        CHECK(!_CeBridge_DeadlineReached(0x10u, 0xFFFFFFF0u, 3000));
        CHECK(_CeBridge_DeadlineReached(0x10u, 0xFFFFFFF0u, 0x20u));
    }

    // 7. CE online-check records: "<line>|<e|w>|<message>".
    {
        int line = 0;
        bool error = false;
        char msg[64] = { 0 };
        const char rec[] = "12|e|Unknown instruction";
        CHECK(_CeBridge_ParseCheckRecord(rec, std::strlen(rec), &line, &error,
                                        msg, sizeof(msg)));
        CHECK(line == 12);
        CHECK(error);
        CHECK(std::strcmp(msg, "Unknown instruction") == 0);
        const char warn[] = "3|w|Missing [DISABLE] section";
        CHECK(_CeBridge_ParseCheckRecord(warn, std::strlen(warn), &line, &error,
                                         msg, sizeof(msg)));
        CHECK(line == 3);
        CHECK(!error);
        CHECK(std::strcmp(msg, "Missing [DISABLE] section") == 0);
        CHECK(!_CeBridge_ParseCheckRecord("bad", 3, &line, &error, msg, sizeof(msg)));
        CHECK(!_CeBridge_ParseCheckRecord("x|e|msg", 7, &line, &error, msg, sizeof(msg)));
    }

    if (g_failures == 0) {
        std::printf("PASS: CE session helpers\n");
    }
    return g_failures == 0 ? 0 : 1;
}
