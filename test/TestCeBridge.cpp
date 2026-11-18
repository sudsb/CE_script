// CE bridge protocol / CE-directory discovery / local-Lua routing regression
// harness.  The functions under test are extracted verbatim from
// src/CeBridge.c and src/CeLua.c by TestCeBridge.ps1 into
// CeBridgeHelpers.inc, so this file must not include any Notepad3 or Win32 header.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

using BYTE = unsigned char;
using DWORD = unsigned long;
using WCHAR = wchar_t;
using LPCWSTR = const wchar_t*;

#define MAX_PATH 260
#define CE_BRIDGE_MAX_FRAME (8 * 1024 * 1024)
#define CE_BRIDGE_MAX_WATCHES 64
#define CE_BRIDGE_WATCH_NAME 64
#define CE_BRIDGE_WATCH_TYPE 16
#define CE_BRIDGE_WATCH_FMT 8
#define CE_BRIDGE_WATCH_COND 256
#define CE_BRIDGE_WATCH_BLOCK 8
#define CE_BRIDGE_WATCH_COUNT 1024
#define COUNTOF(ar) (sizeof(ar) / sizeof((ar)[0]))
#define HEAP_ZERO_MEMORY 0x8u
#define AllocMem(bytes, flags) std::malloc((bytes))
#define FreeMem(ptr) std::free((ptr))

struct CE_AA_OWNER { char key[17]; char* script; DWORD len; };
static CE_AA_OWNER s_aaOwner = {};

static long StringCchCopyA(char* dst, size_t cch, const char* src)
{
    if (!dst || !src || cch == 0) {
        return -1L;
    }
    size_t i = 0;
    while ((i + 1 < cch) && (src[i] != '\0')) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
    return 0L;
}

#include "CeBridgeHelpers.inc"

static int g_failures = 0;

// wcscpy() is deprecated under /W4 /WX; the harness only needs a plain copy.
static void SetPath(WCHAR* dst, const wchar_t* src)
{
    std::wmemcpy(dst, src, std::wcslen(src) + 1);
}

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

int main()
{
    // --- frame length codec ---
    const BYTE enc[] = { 0x78, 0x56, 0x34, 0x12 };
    CHECK(_CeBridge_DecodeLen(enc) == 0x12345678UL);
    const BYTE zero[] = { 0, 0, 0, 0 };
    CHECK(_CeBridge_DecodeLen(zero) == 0UL);
    const BYTE max[] = { 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(_CeBridge_DecodeLen(max) == 0xFFFFFFFFUL);

    BYTE lenBuf[4] = { 0 };
    _CeBridge_EncodeLen(lenBuf, 0x12345678UL);
    CHECK(lenBuf[0] == 0x78 && lenBuf[1] == 0x56 && lenBuf[2] == 0x34 && lenBuf[3] == 0x12);
    _CeBridge_EncodeLen(lenBuf, 0UL);
    CHECK(_CeBridge_DecodeLen(lenBuf) == 0UL);
    _CeBridge_EncodeLen(lenBuf, 0xFFFFFFFFUL);
    CHECK(_CeBridge_DecodeLen(lenBuf) == 0xFFFFFFFFUL);

    // --- request framing ---
    const char payload[] = { 'a', '\0', 'b', '\0', 'c' };  // embedded NULs survive
    BYTE frame[16] = { 0 };
    size_t frameLen = 0;
    CHECK(_CeBridge_EncodeFrame(frame, sizeof(frame), 1, payload, 5, &frameLen));
    CHECK(frameLen == 10);
    CHECK(frame[0] == 1);
    CHECK(_CeBridge_DecodeLen(frame + 1) == 5UL);
    CHECK(std::memcmp(frame + 5, payload, 5) == 0);

    size_t emptyLen = 0;
    CHECK(_CeBridge_EncodeFrame(frame, 5, 0, nullptr, 0, &emptyLen) && emptyLen == 5);
    CHECK(!_CeBridge_EncodeFrame(frame, 9, 1, payload, 5, &frameLen));   // buffer too small
    CHECK(!_CeBridge_EncodeFrame(frame, sizeof(frame), 1, nullptr, 5, &frameLen));
    CHECK(!_CeBridge_EncodeFrame(frame, sizeof(frame), 1, payload,
                                 CE_BRIDGE_MAX_FRAME + 1, &frameLen));
    CHECK(!_CeBridge_EncodeFrame(nullptr, sizeof(frame), 1, payload, 5, &frameLen));
    CHECK(!_CeBridge_EncodeFrame(frame, sizeof(frame), 1, payload, 5, nullptr));

    // --- CE installation directory names (layout-based discovery) ---
    CHECK(_CeBridge_IsCeDirName(L"Cheat Engine 7.7", 16));
    CHECK(_CeBridge_IsCeDirName(L"cheatengine", 11));
    CHECK(_CeBridge_IsCeDirName(L"Cheat-Engine 7.5", 16));
    CHECK(_CeBridge_IsCeDirName(L"cheat engine", 12));
    CHECK(!_CeBridge_IsCeDirName(L"Cheat", 5));
    CHECK(!_CeBridge_IsCeDirName(L"Other Tools", 11));
    CHECK(!_CeBridge_IsCeDirName(L"", 0));
    CHECK(!_CeBridge_IsCeDirName(nullptr, 0));

    // --- running Cheat Engine executable names (bridge deployment target) ---
    CHECK(_CeBridge_IsCeExeName(L"cheatengine-x86_64.exe", 22));
    CHECK(_CeBridge_IsCeExeName(L"CheatEngine-i386.EXE", 20));
    CHECK(_CeBridge_IsCeExeName(L"cheatengine", 11));
    CHECK(!_CeBridge_IsCeExeName(L"cheatengine-x86_64.dll", 22));
    CHECK(!_CeBridge_IsCeExeName(L"notepad3.exe", 12));
    CHECK(!_CeBridge_IsCeExeName(L"cheat", 5));
    CHECK(!_CeBridge_IsCeExeName(L"", 0));
    CHECK(!_CeBridge_IsCeExeName(nullptr, 0));

    // --- walking up from the program folder to the CE installation ---
    {
        WCHAR path[MAX_PATH];
        SetPath(path, L"E:\\Cheat Engine\\Cheat Engine 772\\autorun\\notepad");
        CHECK(_CeBridge_ParentDir(path));
        CHECK(std::wcscmp(path, L"E:\\Cheat Engine\\Cheat Engine 772\\autorun") == 0);
        CHECK(_CeBridge_ParentDir(path));
        CHECK(std::wcscmp(path, L"E:\\Cheat Engine\\Cheat Engine 772") == 0);

        SetPath(path, L"E:\\CE\\autorun\\notepad\\");   // trailing separator
        CHECK(_CeBridge_ParentDir(path));
        CHECK(std::wcscmp(path, L"E:\\CE\\autorun") == 0);

        SetPath(path, L"E:\\CE");
        CHECK(!_CeBridge_ParentDir(path));
        CHECK(std::wcscmp(path, L"E:\\") == 0);

        SetPath(path, L"E:\\");
        CHECK(!_CeBridge_ParentDir(path));

        SetPath(path, L"notepad");
        CHECK(!_CeBridge_ParentDir(path));
        CHECK(!_CeBridge_ParentDir(nullptr));
    }

    // --- candidate roots on a drive ---
    WCHAR roots[8][MAX_PATH] = { { L'\0' } };
    CHECK(_CeBridge_BuildRoots(roots, 8, L'E') == 6);
    CHECK(std::wcscmp(roots[0], L"E:\\") == 0);
    CHECK(std::wcscmp(roots[1], L"E:\\Cheat Engine\\") == 0);
    CHECK(std::wcscmp(roots[2], L"E:\\Program Files\\") == 0);
    CHECK(std::wcscmp(roots[3], L"E:\\Program Files (x86)\\") == 0);
    CHECK(_CeBridge_BuildRoots(roots, 2, L'C') == 2);
    CHECK(std::wcscmp(roots[0], L"C:\\") == 0);

    // --- CE-related code detection: pure Lua must stay local ---
    const char api[] =
        "getAddress readBytes autoAssemble createMemScan initialize ipairs "
        "string.format print printf";
    CHECK(_CeLua_IsWordInList(api, "getAddress", 10));
    CHECK(!_CeLua_IsWordInList(api, "getAddres", 9));
    CHECK(!_CeLua_IsWordInList(api, "getAddressX", 11));
    CHECK(_CeLua_IsStdLuaName("ipairs", 6));
    CHECK(_CeLua_IsAmbiguousName("initialize", 10));
    CHECK(_CeLua_IsCeMarker("np3", 3));

    const char* pure = "for i, v in ipairs(t) do print(string.format('%d', v)) end";
    CHECK(!_CeLua_ScanUsesCeApi(pure, std::strlen(pure), api));
    const char* localFn = "local function printResult(x) print(x) end";
    CHECK(!_CeLua_ScanUsesCeApi(localFn, std::strlen(localFn), api));
    const char* ambiguous = "initialize()";
    CHECK(!_CeLua_ScanUsesCeApi(ambiguous, std::strlen(ambiguous), api));
    const char* comment = "-- getAddress('game.exe')\nprint('hi')";
    CHECK(!_CeLua_ScanUsesCeApi(comment, std::strlen(comment), api));
    const char* longComment = "--[[ getAddress('game.exe') ]] print('hi')";
    CHECK(!_CeLua_ScanUsesCeApi(longComment, std::strlen(longComment), api));
    const char* quoted = "print('getAddress')";
    CHECK(!_CeLua_ScanUsesCeApi(quoted, std::strlen(quoted), api));

    const char* ceScript = "local a = getAddress('game.exe')";
    CHECK(_CeLua_ScanUsesCeApi(ceScript, std::strlen(ceScript), api));
    const char* ceExtension = "local parts = string.split('a,b', ',')";
    CHECK(_CeLua_ScanUsesCeApi(ceExtension, std::strlen(ceExtension), api));
    const char* ownSplit = "local function split(s) return s end\nprint(split('x'))";
    CHECK(!_CeLua_ScanUsesCeApi(ownSplit, std::strlen(ownSplit), api));
    const char* ceMarker = "print(np3.version)";
    CHECK(_CeLua_ScanUsesCeApi(ceMarker, std::strlen(ceMarker), api));
    const char* aaScript = "[ENABLE]\nalloc(newmem, 8)";
    CHECK(_CeLua_ScanUsesCeApi(aaScript, std::strlen(aaScript), api));
    const char* markerInString = "print('[ENABLE]')";
    CHECK(!_CeLua_ScanUsesCeApi(markerInString, std::strlen(markerInString), api));
    const char* markerInComment = "-- [ENABLE]\nprint('hi')";
    CHECK(!_CeLua_ScanUsesCeApi(markerInComment, std::strlen(markerInComment), api));
    const char* extensionInComment = "-- string.split(a)\nprint('hi')";
    CHECK(!_CeLua_ScanUsesCeApi(extensionInComment, std::strlen(extensionInComment), api));
    CHECK(!_CeLua_ScanUsesCeApi(nullptr, 0, api));

    // --- AA detection: CE-only directives must not swallow plain Lua ---
    auto isAaLine = [](const char* s) { return _CeBridge_IsAaDeclaration(s, std::strlen(s)); };
    CHECK(isAaLine("alloc(newmem,2048)"));
    CHECK(isAaLine("  GLOBALALLOC(x,64)"));
    CHECK(isAaLine("AobScanModule(x,'game.exe','8B 45')"));
    CHECK(isAaLine("registersymbol(playermoney)"));
    CHECK(isAaLine("dealloc(newmem)"));
    CHECK(!isAaLine("assert(x ~= nil)"));          // Lua builtin, not an AA directive
    CHECK(!isAaLine("label(x)"));                  // ambiguous with Lua helpers
    CHECK(!isAaLine("print('alloc(x)')"));
    CHECK(!isAaLine("-- alloc(x,8)"));
    CHECK(!isAaLine("local function alloc(x) end"));
    CHECK(!isAaLine("[ENABLE]"));
    CHECK(!_CeBridge_IsAaDeclaration(nullptr, 0));

    // --- AA one-click: Lua long-bracket level must survive ]=*] in scripts ---
    CHECK(_CeBridge_AaBracketLevel(nullptr, 0) == 0);
    const char plainAa[] = "[ENABLE]\nalloc(newmem,2048)\n[DISABLE]\ndealloc(newmem)";
    CHECK(_CeBridge_AaBracketLevel(plainAa, std::strlen(plainAa)) == 0);
    const char closePlain[] = "print(']]')";
    CHECK(_CeBridge_AaBracketLevel(closePlain, std::strlen(closePlain)) == 1);
    const char closeEq[] = "x = ']=]' .. ']]'";
    CHECK(_CeBridge_AaBracketLevel(closeEq, std::strlen(closeEq)) == 2);
    const char closeEq3[] = "']===]' ]=]";
    CHECK(_CeBridge_AaBracketLevel(closeEq3, std::strlen(closeEq3)) == 4);
    const char unclosed[] = "']===== not closed";
    CHECK(_CeBridge_AaBracketLevel(unclosed, std::strlen(unclosed)) == 0);

    // --- AA state key: stable per document, distinct across documents ---
    char keyA[17] = { 0 }, keyB[17] = { 0 }, keyC[17] = { 0 };
    _CeBridge_AaStateKey(L"C:\\scripts\\hook.cea", keyA);
    _CeBridge_AaStateKey(L"C:\\scripts\\hook.cea", keyB);
    _CeBridge_AaStateKey(L"C:\\scripts\\other.cea", keyC);
    CHECK(std::strlen(keyA) == 16);
    CHECK(std::strcmp(keyA, keyB) == 0);
    CHECK(std::strcmp(keyA, keyC) != 0);
    _CeBridge_AaStateKey(nullptr, keyB);
    _CeBridge_AaStateKey(L"", keyC);
    CHECK(std::strcmp(keyB, keyC) == 0);

    // --- AA request: wraps the script for CE's native autoAssemble ---
    {
        const char aa[] =
            "[ENABLE]\nalloc(newmem,2048)\nalloc(playermoney,8)\n"
            "// ]=] must not close the Lua long string\n"
            "label(returnhere)\n[DISABLE]\ndealloc(newmem)";
        const char keyHex[] = "0123456789abcdef";
        char req[16384] = { 0 };
        size_t reqLen = 0;
        CHECK(_CeBridge_BuildAaRequest(req, sizeof(req), aa, std::strlen(aa), keyHex, true, &reqLen));
        CHECK(reqLen == std::strlen(req));
        CHECK(std::strstr(req, "local NP3_KEY='0123456789abcdef'") != nullptr);
        CHECK(std::strstr(req, "local NP3_HAS_DISABLE=true") != nullptr);
        CHECK(std::strstr(req, "[==[\n") != nullptr);
        CHECK(std::strstr(req, "]==]") != nullptr);
        CHECK(std::strstr(req, aa) != nullptr);
        CHECK(std::strstr(req, "getOpenedProcessID()") != nullptr);
        CHECK(std::strstr(req, "autoAssembleCheck(NP3_S,true)") != nullptr);
        CHECK(std::strstr(req, "autoAssembleCheck(NP3_S,false)") != nullptr);
        CHECK(std::strstr(req, "autoAssemble(NP3_S)") != nullptr);
        CHECK(std::strstr(req, "autoAssemble(NP3_S,st.info)") != nullptr);
        CHECK(std::strstr(req, "B.aaState[NP3_KEY]={info=info}") != nullptr);
        CHECK(std::strstr(req, "local NP3_SYMBOLS={'playermoney'}") != nullptr);
        CHECK(std::strstr(req, "B.watchStart(NP3_SYMBOLS,NP3_KEY,NP3_WATCHES,NP3_AUTO_WATCH)") != nullptr);
        CHECK(std::strstr(req, "local NP3_AUTO_WATCH=true") != nullptr);
        CHECK(std::strstr(req, "local NP3_WATCHES={{name='playermoney',type='qword',fmt='',count=0,exclude=false,cond='',reg=false},}") != nullptr);
        CHECK(std::strstr(req, "B.watchActive") != nullptr);
        CHECK(std::strstr(req, "B.watchStop()") != nullptr);
        CHECK(std::strstr(req, "B.watchDrain") != nullptr);
        // One-character RESULT: CE-side AA state marker for the close prompt.
        CHECK(std::strstr(req, "return '1'") != nullptr);
        CHECK(std::strstr(req, "return '0'") != nullptr);
        CHECK(!_CeBridge_BuildAaRequest(req, 32, aa, std::strlen(aa), keyHex, true, &reqLen));
        CHECK(!_CeBridge_BuildAaRequest(nullptr, sizeof(req), aa, std::strlen(aa), keyHex, true, &reqLen));
        CHECK(!_CeBridge_BuildAaRequest(req, sizeof(req), nullptr, 4, keyHex, true, &reqLen));
        CHECK(!_CeBridge_BuildAaRequest(req, sizeof(req), aa, std::strlen(aa), nullptr, true, &reqLen));
        CHECK(!_CeBridge_BuildAaRequest(req, sizeof(req), aa, std::strlen(aa), keyHex, true, nullptr));

        char req2[16384] = { 0 };
        CHECK(_CeBridge_BuildAaRequest(req2, sizeof(req2), aa, std::strlen(aa), keyHex, false, &reqLen));
        CHECK(std::strstr(req2, "local NP3_HAS_DISABLE=false") != nullptr);
    }

    // --- AA watch: collect user data symbols, skip newmem/labels/comments ---
    CHECK(_CeBridge_AaNameEqualsCI("playermoney", "PlayerMoney"));
    CHECK(!_CeBridge_AaNameEqualsCI("playermoney", "player"));
    CHECK(_CeBridge_AaNameEqualsCI("", ""));
    {
        const char script[] =
            "[ENABLE]\n"
            "alloc(newmem,2048)\n"
            "alloc(playermoney,8)\n"
            "label(returnhere)\n"
            "registersymbol(playermoney)\n"
            "// alloc(commented,8)\n"
            "aobscanmodule(hookAddr,game.exe,8B 45 08)\n"
            "[DISABLE]\n"
            "dealloc(newmem)\n";
        char syms[24][64] = { { 0 } };
        int const n = _CeBridge_CollectAaSymbols(script, std::strlen(script), syms, 24);
        CHECK(n == 2);
        CHECK(std::strcmp(syms[0], "playermoney") == 0);
        CHECK(std::strcmp(syms[1], "hookAddr") == 0);
        CHECK(_CeBridge_CollectAaSymbols(nullptr, 0, syms, 24) == 0);
        char one[1][64] = { { 0 } };
        CHECK(_CeBridge_CollectAaSymbols(script, std::strlen(script), one, 1) == 1);
    }

    // --- manual watch directives: name/type/format/count, unwatch, auto off ---
    {
        const char script[] =
            "[ENABLE]\n"
            "alloc(buf,64)\n"
            "define(hp,123)\n"
            "// np3watch hp dword hex\n"
            "; np3watch pos float\n"
            "// np3watch buf byte hex 16\n"
            "// np3unwatch hookAddr\n"
            "// np3watch off\n"
            "// np3watch on\n";
        char names[8][CE_BRIDGE_WATCH_NAME] = { { 0 } };
        char types[8][CE_BRIDGE_WATCH_TYPE] = { { 0 } };
        char formats[8][CE_BRIDGE_WATCH_FMT] = { { 0 } };
        char conds[8][CE_BRIDGE_WATCH_COND] = { { 0 } };
        int counts[8] = { 0 };
        int excludes[8] = { 0 };
        int regs[8] = { 0 };
        int autoWatch = 1;
        int const n = _CeBridge_CollectWatchDirectives(script, std::strlen(script), names, types,
                                                       formats, conds, counts, excludes, regs, 8, &autoWatch);
        CHECK(n == 4);
        CHECK(autoWatch == 1);
        CHECK(std::strcmp(names[0], "hp") == 0);
        CHECK(std::strcmp(types[0], "dword") == 0);
        CHECK(std::strcmp(formats[0], "hex") == 0);
        CHECK(counts[0] == 0 && excludes[0] == 0);
        CHECK(conds[0][0] == '\0' && regs[0] == 0);
        CHECK(std::strcmp(names[1], "pos") == 0);
        CHECK(std::strcmp(types[1], "float") == 0);
        CHECK(formats[1][0] == '\0');
        CHECK(std::strcmp(names[2], "buf") == 0);
        CHECK(std::strcmp(types[2], "byte") == 0);
        CHECK(counts[2] == 16);
        CHECK(std::strcmp(names[3], "hookAddr") == 0);
        CHECK(excludes[3] == 1);
        CHECK(conds[3][0] == '\0' && regs[3] == 0);
        CHECK(_CeBridge_CollectWatchDirectives(nullptr, 0, names, types, formats, conds, counts,
                                               excludes, regs, 8, &autoWatch) == 0);

        char one[1][CE_BRIDGE_WATCH_NAME] = { { 0 } };
        char oneType[1][CE_BRIDGE_WATCH_TYPE] = { { 0 } };
        char oneFmt[1][CE_BRIDGE_WATCH_FMT] = { { 0 } };
        char oneCond[1][CE_BRIDGE_WATCH_COND] = { { 0 } };
        int oneCount[1] = { 0 };
        int oneExclude[1] = { 0 };
        int oneReg[1] = { 0 };
        CHECK(_CeBridge_CollectWatchDirectives(script, std::strlen(script), one, oneType, oneFmt,
                                               oneCond, oneCount, oneExclude, oneReg, 1, &autoWatch) == 1);

        // case-insensitive directive/type/format aliases, later line wins
        const char aliases[] =
            "// NP3WATCH HP u32 HEXADECIMAL 2\n"
            "// np3watch hp ptr binary 4\n";
        autoWatch = 1;
        int const an = _CeBridge_CollectWatchDirectives(aliases, std::strlen(aliases), names, types,
                                                       formats, conds, counts, excludes, regs, 8, &autoWatch);
        CHECK(an == 1);
        CHECK(std::strcmp(names[0], "hp") == 0);
        CHECK(std::strcmp(types[0], "ptr") == 0);
        CHECK(std::strcmp(formats[0], "binary") == 0);
        CHECK(counts[0] == 4);

        // registers: recognised names switch the entry to type 'reg'
        const char regScript[] =
            "// np3watch rax\n"
            "// np3watch eax dec\n"
            "// np3watch al\n"
            "// np3watch rip\n"
            "// np3watch flags\n"
            "// np3watch r10d\n"
            "// np3watch ah\n";
        autoWatch = 1;
        int const rn = _CeBridge_CollectWatchDirectives(regScript, std::strlen(regScript), names, types,
                                                       formats, conds, counts, excludes, regs, 8, &autoWatch);
        CHECK(rn == 7);
        CHECK(regs[0] == 1 && std::strcmp(types[0], "reg") == 0);
        CHECK(std::strcmp(names[1], "eax") == 0 && std::strcmp(formats[1], "dec") == 0);
        CHECK(regs[2] == 1 && regs[3] == 1 && regs[4] == 1 && regs[5] == 1 && regs[6] == 1);

        char rb[8]; unsigned long long rm; int rs;
        CHECK(_CeBridge_WatchRegister("eax", rb, &rm, &rs) && std::strcmp(rb, "rax") == 0 &&
              rm == 0xFFFFFFFFull && rs == 0);
        CHECK(_CeBridge_WatchRegister("ah", rb, &rm, &rs) && std::strcmp(rb, "rax") == 0 &&
              rm == 0xFFull && rs == 8);
        CHECK(_CeBridge_WatchRegister("sil", rb, &rm, &rs) && std::strcmp(rb, "rsi") == 0 && rm == 0xFFull);
        CHECK(_CeBridge_WatchRegister("r10d", rb, &rm, &rs) && std::strcmp(rb, "r10") == 0 && rm == 0xFFFFFFFFull);
        CHECK(_CeBridge_WatchRegister("flags", rb, &rm, &rs) && std::strcmp(rb, "eflags") == 0);
        CHECK(_CeBridge_WatchRegister("rax", rb, &rm, &rs) && std::strcmp(rb, "rax") == 0 &&
              rm == ~(unsigned long long)0);
        CHECK(!_CeBridge_WatchRegister("axx", rb, &rm, &rs));
        CHECK(!_CeBridge_WatchRegister("hp", rb, &rm, &rs));

        // conditions are translated to Lua expressions: "!=" -> "~=",
        // "$10" -> "0x10", everything else passes through
        const char condScript[] =
            "// np3watch hp dword hex if hp > 0\n"
            "// np3watch rax if rax != 0x1234\n"
            "// np3watch pos if $10 == pos\n"
            "// np3watch bad if hp >> 0\n"
            "// np3watch bad2 if hp 0\n";
        autoWatch = 1;
        int const cn = _CeBridge_CollectWatchDirectives(condScript, std::strlen(condScript), names, types,
                                                       formats, conds, counts, excludes, regs, 8, &autoWatch);
        CHECK(cn == 5);
        CHECK(std::strcmp(conds[0], "hp > 0") == 0);
        CHECK(std::strcmp(conds[1], "rax ~= 0x1234") == 0 && regs[1] == 1);
        CHECK(std::strcmp(conds[2], "0x10 == pos") == 0);
        CHECK(std::strcmp(conds[3], "hp >> 0") == 0);   // kept; Lua decides validity
        CHECK(std::strcmp(conds[4], "hp 0") == 0);

        // multi-condition expressions: || -> or, && -> and, ! -> not
        const char multiScript[] =
            "// np3watch rcx if rax == 3 || rbx != 4\n"
            "// np3watch rdx if rax == 1 && (rbx > 2 || !ecx)\n"
            "// np3watch rsi if not flags and hp >= 10\n"
            "// np3watch rdi if myTable.hp > 0\n";
        autoWatch = 1;
        int const mn = _CeBridge_CollectWatchDirectives(multiScript, std::strlen(multiScript), names, types,
                                                       formats, conds, counts, excludes, regs, 8, &autoWatch);
        CHECK(mn == 4);
        CHECK(std::strcmp(conds[0], "rax == 3 or rbx ~= 4") == 0);
        CHECK(std::strcmp(conds[1], "rax == 1 and (rbx > 2 or not ecx)") == 0);
        CHECK(std::strcmp(conds[2], "not flags and hp >= 10") == 0);
        CHECK(std::strcmp(conds[3], "myTable.hp > 0") == 0);

        // unbalanced parentheses or an unterminated quote drop the condition
        const char badCondScript[] =
            "// np3watch hp if (hp > 0\n"
            "// np3watch mp if hp == 'x\n";
        autoWatch = 1;
        int const bn = _CeBridge_CollectWatchDirectives(badCondScript, std::strlen(badCondScript), names, types,
                                                       formats, conds, counts, excludes, regs, 8, &autoWatch);
        CHECK(bn == 2);
        CHECK(conds[0][0] == '\0');
        CHECK(conds[1][0] == '\0');

        // spacing around the operator is optional
        const char tightScript[] =
            "// np3watch rax if rax ==0x2\n"
            "// np3watch rbx if rbx==0x2\n"
            "// np3watch rcx if rcx\t>=\t1\n"
            "// np3watch rdx if rdx <=-1\n";
        autoWatch = 1;
        int const tn = _CeBridge_CollectWatchDirectives(tightScript, std::strlen(tightScript), names, types,
                                                       formats, conds, counts, excludes, regs, 8, &autoWatch);
        CHECK(tn == 4);
        CHECK(std::strcmp(conds[0], "rax ==0x2") == 0);
        CHECK(std::strcmp(conds[1], "rbx==0x2") == 0);
        CHECK(std::strcmp(conds[2], "rcx >= 1") == 0);
        CHECK(std::strcmp(conds[3], "rdx <=-1") == 0);

        // blocks: begin/end inherit the block condition
        const char blockScript[] =
            "// np3watch begin if rax == 0\n"
            "// np3watch hp dword\n"
            "// np3watch pos float if pos > 2\n"
            "// np3watch end\n"
            "// np3watch buf byte\n";
        autoWatch = 1;
        int const blockN = _CeBridge_CollectWatchDirectives(blockScript, std::strlen(blockScript), names, types,
                                                          formats, conds, counts, excludes, regs, 8, &autoWatch);
        CHECK(blockN == 3);
        CHECK(std::strcmp(names[0], "hp") == 0 && std::strcmp(conds[0], "rax == 0") == 0);
        CHECK(std::strcmp(names[1], "pos") == 0 && std::strcmp(conds[1], "rax == 0 and pos > 2") == 0);
        CHECK(std::strcmp(names[2], "buf") == 0 && conds[2][0] == '\0');

        // nested blocks join with " and "
        const char nested[] =
            "// np3watch begin if eax != 0\n"
            "// np3watch begin if flags == 0\n"
            "// np3watch hp\n"
            "// np3watch end\n"
            "// np3watch end\n";
        autoWatch = 1;
        int const nn = _CeBridge_CollectWatchDirectives(nested, std::strlen(nested), names, types,
                                                       formats, conds, counts, excludes, regs, 8, &autoWatch);
        CHECK(nn == 1);
        CHECK(std::strcmp(conds[0], "eax ~= 0 and flags == 0") == 0);

        // block shorthand: the "np3watch" prefix may be omitted inside a block
        const char shortBlock[] =
            "// np3watch begin if rax == 2\n"
            "// rcx\n"
            "// rdx hex\n"
            "// r8d if r8d != 0\n"
            "// begin if rbx > 1\n"
            "// rsi dec\n"
            "// end\n"
            "// end\n"
            "// hp dword\n";
        autoWatch = 1;
        int const sbn = _CeBridge_CollectWatchDirectives(shortBlock, std::strlen(shortBlock), names, types,
                                                        formats, conds, counts, excludes, regs, 8, &autoWatch);
        CHECK(sbn == 4);   // hp is outside any block, so it stays a plain comment
        CHECK(std::strcmp(names[0], "rcx") == 0 && regs[0] == 1);
        CHECK(std::strcmp(conds[0], "rax == 2") == 0);
        CHECK(std::strcmp(names[1], "rdx") == 0 && std::strcmp(formats[1], "hex") == 0);
        CHECK(std::strcmp(conds[1], "rax == 2") == 0);
        CHECK(std::strcmp(names[2], "r8d") == 0 && std::strcmp(conds[2], "rax == 2 and r8d ~= 0") == 0);
        CHECK(std::strcmp(names[3], "rsi") == 0 && std::strcmp(conds[3], "rax == 2 and rbx > 1") == 0);

        // shorthand lines are only cleared inside a block and never when they
        // are ordinary comments or code
        {
            bool remove = false;
            int delta = 0;
            const char l1[] = "// np3watch begin if rax == 2";
            _CeBridge_ClassifyWatchLine(l1, std::strlen(l1), false, &remove, &delta);
            CHECK(remove && delta == 1);
            const char l2[] = "// rcx";
            _CeBridge_ClassifyWatchLine(l2, std::strlen(l2), true, &remove, &delta);
            CHECK(remove && delta == 0);
            _CeBridge_ClassifyWatchLine(l2, std::strlen(l2), false, &remove, &delta);
            CHECK(!remove);
            const char l3[] = "// note hp here";
            _CeBridge_ClassifyWatchLine(l3, std::strlen(l3), true, &remove, &delta);
            CHECK(!remove);   // unknown token -> ordinary comment
            const char l4[] = "// 123";
            _CeBridge_ClassifyWatchLine(l4, std::strlen(l4), true, &remove, &delta);
            CHECK(!remove);
            const char l5[] = "// end";
            _CeBridge_ClassifyWatchLine(l5, std::strlen(l5), true, &remove, &delta);
            CHECK(remove && delta == -1);
            const char l6[] = "// np3watch end";
            _CeBridge_ClassifyWatchLine(l6, std::strlen(l6), false, &remove, &delta);
            CHECK(remove && delta == -1);
            const char l7[] = "// np3unwatch hp";
            _CeBridge_ClassifyWatchLine(l7, std::strlen(l7), false, &remove, &delta);
            CHECK(remove && delta == 0);
            const char l8[] = "mov eax,1";
            _CeBridge_ClassifyWatchLine(l8, std::strlen(l8), true, &remove, &delta);
            CHECK(!remove);
            const char l9[] = "// begin";
            _CeBridge_ClassifyWatchLine(l9, std::strlen(l9), false, &remove, &delta);
            CHECK(!remove);
        }

        // declaration-based inference: define literal > data label > alloc size
        {
            char inferred[CE_BRIDGE_WATCH_TYPE] = { 0 };
            int inferredCount = -1;
            const char withDefine[] = "alloc(hp,4)\ndefine(hp, 1.5)\n";
            _CeBridge_InferSymbolType(withDefine, std::strlen(withDefine), "hp", inferred, &inferredCount);
            CHECK(std::strcmp(inferred, "double") == 0);
            CHECK(inferredCount == 0);

            const char withAlloc[] = "alloc(buf,64)\n";
            inferred[0] = '\0';
            _CeBridge_InferSymbolType(withAlloc, std::strlen(withAlloc), "buf", inferred, &inferredCount);
            CHECK(std::strcmp(inferred, "byte") == 0);
            CHECK(inferredCount == 64);

            const char withLabel[] = "pos: dd 0\n";
            inferred[0] = '\0';
            _CeBridge_InferSymbolType(withLabel, std::strlen(withLabel), "pos", inferred, &inferredCount);
            CHECK(std::strcmp(inferred, "dword") == 0);

            const char withString[] = "define(name, \"abc\")\n";
            inferred[0] = '\0';
            _CeBridge_InferSymbolType(withString, std::strlen(withString), "name", inferred, &inferredCount);
            CHECK(std::strcmp(inferred, "string") == 0);

            const char withHex[] = "define(wide, 0x123456789)\n";
            inferred[0] = '\0';
            _CeBridge_InferSymbolType(withHex, std::strlen(withHex), "wide", inferred, &inferredCount);
            CHECK(std::strcmp(inferred, "qword") == 0);

            const char ghost[] = "// define(ghost, 1)\n";
            inferred[0] = '\0';
            _CeBridge_InferSymbolType(ghost, std::strlen(ghost), "ghost", inferred, &inferredCount);
            CHECK(inferred[0] == '\0');
        }

        // directive specs are forwarded to the generated CE request
        char req[16384] = { 0 };
        size_t reqLen = 0;
        const char withSpec[] = "[ENABLE]\nalloc(hp,8)\n// np3watch hp dword hex\n";
        CHECK(_CeBridge_BuildAaRequest(req, sizeof(req), withSpec, std::strlen(withSpec),
                                       "0123456789abcdef", false, &reqLen));
        CHECK(std::strstr(req, "local NP3_AUTO_WATCH=true") != nullptr);
        CHECK(std::strstr(req, "{name='hp',type='dword',fmt='hex',count=0,exclude=false,cond='',reg=false},") != nullptr);

        // register + condition specs are forwarded with their resolved fields
        char reqReg[16384] = { 0 };
        const char regSpec[] = "[ENABLE]\n// np3watch eax\n// np3watch hp dword hex if hp > 0\n";
        CHECK(_CeBridge_BuildAaRequest(reqReg, sizeof(reqReg), regSpec, std::strlen(regSpec),
                                       "0123456789abcdef", false, &reqLen));
        CHECK(std::strstr(reqReg, "{name='eax',type='reg',fmt='',count=0,exclude=false,cond='',"
                                  "reg=true,rb='rax',rm=4294967295,rs=0},") != nullptr);
        CHECK(std::strstr(reqReg, "{name='hp',type='dword',fmt='hex',count=0,exclude=false,"
                                  "cond='hp > 0',reg=false},") != nullptr);
        CHECK(std::strstr(reqReg, "regValue") != nullptr);
        CHECK(std::strstr(reqReg, "evalCond") != nullptr);
        CHECK(std::strstr(reqReg, "getRegisters") != nullptr);

        // gating one register on another: "if rax == 2" only emits rcx then
        char reqGate[16384] = { 0 };
        const char gateSpec[] = "[ENABLE]\n// np3watch rcx if rax == 2\n";
        CHECK(_CeBridge_BuildAaRequest(reqGate, sizeof(reqGate), gateSpec, std::strlen(gateSpec),
                                       "0123456789abcdef", false, &reqLen));
        CHECK(std::strstr(reqGate, "{name='rcx',type='reg',fmt='',count=0,exclude=false,"
                                   "cond='rax == 2',reg=true,rb='rcx',rm=-1,rs=0},") != nullptr);

        // automatic symbols receive inferred types too (large buffers keep the
        // runtime probe to avoid flooding the output pane)
        char reqAuto[16384] = { 0 };
        const char autoScript[] = "[ENABLE]\nalloc(hp,8)\n";
        CHECK(_CeBridge_BuildAaRequest(reqAuto, sizeof(reqAuto), autoScript,
                                       std::strlen(autoScript), "0123456789abcdef", false, &reqLen));
        CHECK(std::strstr(reqAuto, "{name='hp',type='qword',fmt='',count=0,exclude=false,cond='',reg=false},") != nullptr);

        char reqBig[16384] = { 0 };
        const char bigScript[] = "[ENABLE]\nalloc(buf,64)\n";
        CHECK(_CeBridge_BuildAaRequest(reqBig, sizeof(reqBig), bigScript,
                                       std::strlen(bigScript), "0123456789abcdef", false, &reqLen));
        CHECK(std::strstr(reqBig, "local NP3_WATCHES={}") != nullptr);

        // np3watch off: automatic symbols are not watched, manual ones remain
        char reqOff[16384] = { 0 };
        const char offScript[] = "[ENABLE]\nalloc(buf,64)\n// np3watch off\n// np3watch hp dword hex\n";
        CHECK(_CeBridge_BuildAaRequest(reqOff, sizeof(reqOff), offScript,
                                       std::strlen(offScript), "0123456789abcdef", false, &reqLen));
        CHECK(std::strstr(reqOff, "local NP3_AUTO_WATCH=false") != nullptr);
        CHECK(std::strstr(reqOff, "{name='buf'") == nullptr);
        CHECK(std::strstr(reqOff, "{name='hp',type='dword',fmt='hex',count=0,exclude=false,cond='',reg=false},") != nullptr);
    }

    // --- one-key clear: classify watch directive comment lines ---
    {
        auto isDir = [](const char* s) {
            return _CeBridge_IsWatchDirectiveLine(s, std::strlen(s)) ? 1 : 0;
        };
        CHECK(isDir("// np3watch hp"));
        CHECK(isDir("   ; np3watch begin if rax==0"));
        CHECK(isDir("//NP3WATCH end"));
        CHECK(isDir("\t// np3unwatch hp"));
        CHECK(isDir("// np3nowatch hp"));
        CHECK(!isDir("// np3watchx hp"));
        CHECK(!isDir("// watch hp"));
        CHECK(!isDir("np3watch hp"));
        CHECK(!isDir("// alloc(x,8)"));
        CHECK(!isDir(""));
        CHECK(!_CeBridge_IsWatchDirectiveLine(nullptr, 0));
    }

    // --- AA cancel: stop the watch and disable, never enable a script ---
    {
        const char aa[] = "[ENABLE]\nalloc(x,8)\n[DISABLE]\ndealloc(x)";
        char req[4096] = { 0 };
        size_t reqLen = 0;
        CHECK(_CeBridge_BuildAaCancelRequest(req, sizeof(req), aa, std::strlen(aa),
                                             "0123456789abcdef", &reqLen));
        CHECK(reqLen == std::strlen(req));
        CHECK(std::strstr(req, "B.watchStop()") != nullptr);
        CHECK(std::strstr(req, "local st=B.aaState and B.aaState['0123456789abcdef']") != nullptr);
        CHECK(std::strstr(req, "autoAssemble(NP3_S,st.info)") != nullptr);
        CHECK(std::strstr(req, "B.aaState['0123456789abcdef']=nil") != nullptr);
        CHECK(std::strstr(req, "return '0'") != nullptr);
        CHECK(std::strstr(req, aa) != nullptr);
        CHECK(!_CeBridge_BuildAaCancelRequest(req, 32, aa, std::strlen(aa),
                                              "0123456789abcdef", &reqLen));
        CHECK(!_CeBridge_BuildAaCancelRequest(nullptr, sizeof(req), aa, std::strlen(aa),
                                              "0123456789abcdef", &reqLen));
    }

    // --- AA owner: cancel must use the enabling document's key and script ---
    {
        const char ownerScript[] = "[ENABLE]\nalloc(x,8)\n[DISABLE]\ndealloc(x)";
        _CeBridge_AaOwnerSet("1111111111111111", ownerScript, (DWORD)std::strlen(ownerScript));
        CHECK(_CeBridge_AaOwnerMatches("1111111111111111"));
        CHECK(!_CeBridge_AaOwnerMatches("2222222222222222"));
        DWORD cancelLen = 0;
        char* const cancel = _CeBridge_AaOwnerCancelRequest(&cancelLen);
        CHECK(cancel != nullptr);
        if (cancel) {
            CHECK(std::strstr(cancel, "B.aaState['1111111111111111']") != nullptr);
            CHECK(std::strstr(cancel, "[DISABLE]") != nullptr);
            FreeMem(cancel);
        }
        _CeBridge_AaOwnerClear();
        CHECK(!_CeBridge_AaOwnerMatches("1111111111111111"));
    }

    if (g_failures != 0) {
        std::printf("CE bridge tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("CE bridge tests: all passed\n");
    return 0;
}
