// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLua.c                                                                     *
*   Local Lua runtime for Cheat Engine scripts that do not touch the CE API.   *
*   A compatible Lua DLL (CE ships lua53-64.dll / lua53-32.dll) is loaded on   *
*   demand, the document is executed in a fresh lua_State and print()/result/  *
*   error lines are streamed into the CE output pane.  No Cheat Engine         *
*   process, autorun script or named pipe is needed for this path.             *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <strsafe.h>
#include <string.h>
#include <wchar.h>

#include "TypeDefs.h"
#include "Notepad3.h"
#include "PathLib.h"
#include "CeBridge.h"
#include "CeLua.h"
#include "StyleLexers/ce_completions_data.h"

// ----------------------------------------------------------------------------
// CE-related code detection.  These helpers are pure C (no Win32) and are
// also extracted verbatim by test/TestCeBridge.ps1 into its regression harness.
// ----------------------------------------------------------------------------

static bool _CeLua_IsIdentStart(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static bool _CeLua_IsIdentChar(char ch)
{
    return _CeLua_IsIdentStart(ch) || (ch >= '0' && ch <= '9');
}

static bool _CeLua_IsWordInList(const char* list, const char* word, size_t len)
{
    if (!list || !word || len == 0) {
        return false;
    }
    const char* p = list;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        const char* const start = p;
        while (*p && *p != ' ') {
            ++p;
        }
        if ((size_t)(p - start) == len && memcmp(start, word, len) == 0) {
            return true;
        }
    }
    return false;
}

// Lua 5.3 globals and library members.  celua.txt also documents these, so
// they end up in CE_KEYWORDS_API; without this list every plain Lua script
// would be misdetected as "requires Cheat Engine".
static bool _CeLua_IsStdLuaName(const char* word, size_t len)
{
    static const char s[] =
        "_G _VERSION assert collectgarbage dofile error getmetatable ipairs load loadfile "
        "next pairs pcall print rawequal rawget rawlen rawset require select setmetatable "
        "tonumber tostring type xpcall "
        "coroutine debug io math os package string table utf8 bit bit32 "
        "create resume yield wrap status running isyieldable "
        "byte char dump find format gmatch gsub len lower match rep reverse sub upper "
        "pack unpack packsize "
        "concat insert move maxn remove sort "
        "abs acos asin atan ceil cos deg exp floor fmod huge ldexp log max maxinteger "
        "min mininteger modf pi pow rad random randomseed sin sqrt tan tointeger ult "
        "clock date difftime execute exit getenv rename setlocale time tmpname "
        "close flush input lines open output read seek setvbuf write "
        "charpattern char codepoint codes offset "
        "gethook getinfo getlocal getregistry getupvalue getuservalue sethook setlocal "
        "setupvalue setuservalue traceback upvalueid upvaluejoin "
        "band bor bxor bnot lshift rshift arshift btest extract lrotate rrotate tobit tohex "
        "config cpath loaded loadlib path preload searchers searchpath ";
    return _CeLua_IsWordInList(s, word, len);
}

// Generic names which CE exposes as class members but which are just as likely
// to be user-defined helpers.  Skipping them keeps pure Lua scripts local;
// scripts using real CE API names are still detected.
static bool _CeLua_IsAmbiguousName(const char* word, size_t len)
{
    static const char s[] =
        "add allocate begin clear clone close connect copy count create delete destroy "
        "disconnect end find free get getName getCount getText getValue hide index insert "
        "initialize label line load lock name new open read refresh remove rename reset "
        "resize resume save set setName setText setValue show size split start stop store "
        "suspend text trim unlock update value wait write "
        "split trim endsWith startsWith replace printf ";
    return _CeLua_IsWordInList(s, word, len);
}

// Unambiguous CE globals/objects, including the ones that live in the lexer
// keyword sets rather than in CE_KEYWORDS_API.
static bool _CeLua_IsCeMarker(const char* word, size_t len)
{
    static const char s[] =
        "np3 MainForm AddressList MemoryRecord memrec syntaxcheck process TrainerOrigin "
        "getLuaEngine targetIs64Bit cheatEngineIs64Bit "
        "autoAssemble autoAssembleCheck assemble deAlloc fullAccess "
        "showMessage messageDialog inputQuery sleep beep pause unpause queue try "
        "synchronize checkSynchronize inMainThread "
        "getAddress getAddressSafe getOpenedProcessID openProcess getProcessIDFromProcessName "
        "getCEVersion getCEName getAutoRunPath "
        "readBytes writeBytes readInteger writeInteger readPointer writePointer "
        "readFloat writeFloat readDouble writeDouble readString writeString "
        "createMemScan createFoundList getCurrentMemscan "
        "createThread createThreadNewState createThreadSuspended createNativeThread "
        "createTimer createPipe connectToPipe createDisassembler createStructure "
        "createTableEntry debug_setBreakpoint debug_removeBreakpoint "
        "registerSymbol unregisterSymbol getSymbolFromAddress getNameFromAddress "
        "getMainForm getMemoryViewForm getAddressList getMemoryRecord getMemoryRecordByID "
        "AOBScan AOBScanModule AOBScanRegion aobscan aobscanmodule "
        "injectDLL injectLibrary enumModules enumExports "
        "disassemble disassembleBytes getStructure getStructureCount "
        "saveCurrentResults waitTillDone getPixel getMousePos setMousePos "
        "getTickCount getSystemMetrics getScreenWidth getScreenHeight "
        "getWindowList getProcessList getThreadList "
        "printf convertToUTF8 invertColor md5file md5memory ";
    return _CeLua_IsWordInList(s, word, len);
}

// Tokenize Lua/C source, skipping comments and string literals.  An identifier
// that is a CE API name (from the generated catalogue) or an unambiguous CE
// marker means the script has to run inside Cheat Engine.
static bool _CeLua_ScanUsesCeApi(const char* s, size_t len, const char* apiWords)
{
    if (!s || len == 0) {
        return false;
    }
    static const char* const sectionMarkers[] = {
        "[ENABLE]", "[DISABLE]", "{$asm}", "{$ccode}", "{$lua}",
        // CE-only library extensions (LuaHandler.pas adds these to string/math).
        "string.split", "string.trim", "string.startsWith", "string.endsWith",
        "string.replace", "math.cosh", "math.sinh", "math.tanh", "math.frexp"
    };

    size_t i = 0;
    int block = 0;  // 1 = --[[ ]], 2 = /* */
    while (i < len) {
        char const ch = s[i];
        if (block == 1) {
            if (i + 1 < len && ch == ']' && s[i + 1] == ']') {
                i += 2;
                block = 0;
            }
            else {
                ++i;
            }
            continue;
        }
        if (block == 2) {
            if (i + 1 < len && ch == '*' && s[i + 1] == '/') {
                i += 2;
                block = 0;
            }
            else {
                ++i;
            }
            continue;
        }
        if (ch == '-' && i + 1 < len && s[i + 1] == '-') {
            i += 2;
            if (i + 1 < len && s[i] == '[' && s[i + 1] == '[') {
                i += 2;
                block = 1;
            }
            else {
                while (i < len && s[i] != '\n' && s[i] != '\r') {
                    ++i;
                }
            }
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '*') {
            i += 2;
            block = 2;
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '/') {
            i += 2;
            while (i < len && s[i] != '\n' && s[i] != '\r') {
                ++i;
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            char const quote = ch;
            ++i;
            while (i < len) {
                if (s[i] == '\\') {
                    i += (i + 1 < len) ? 2 : 1;
                }
                else if (s[i] == quote) {
                    ++i;
                    break;
                }
                else {
                    ++i;
                }
            }
            continue;
        }
        if (ch == '[' && i + 1 < len && s[i + 1] == '[') {
            i += 2;
            while (i + 1 < len && !(s[i] == ']' && s[i + 1] == ']')) {
                ++i;
            }
            i = (i + 1 < len) ? i + 2 : len;
            continue;
        }
        for (size_t m = 0; m < COUNTOF(sectionMarkers); ++m) {
            const char* const marker = sectionMarkers[m];
            size_t const mlen = strlen(marker);
            if ((i + mlen > len) || (memcmp(s + i, marker, mlen) != 0)) {
                continue;
            }
            char const last = marker[mlen - 1];
            if (!_CeLua_IsIdentChar(last) ||
                ((i + mlen == len) || !_CeLua_IsIdentChar(s[i + mlen]))) {
                return true;   // Auto Assembler / table script, not pure Lua
            }
        }
        if (_CeLua_IsIdentStart(ch)) {
            const char* const start = s + i;
            while (i < len && _CeLua_IsIdentChar(s[i])) {
                ++i;
            }
            size_t const wlen = (size_t)(s + i - start);
            if (!_CeLua_IsStdLuaName(start, wlen) &&
                !_CeLua_IsAmbiguousName(start, wlen) &&
                (_CeLua_IsWordInList(apiWords, start, wlen) || _CeLua_IsCeMarker(start, wlen))) {
                return true;
            }
            continue;
        }
        ++i;
    }
    return false;
}

bool CeLua_IsCeRelated(const char* pszScript, size_t len)
{
    return _CeLua_ScanUsesCeApi(pszScript, len, CE_KEYWORDS_API);
}

// ----------------------------------------------------------------------------
// Local Lua runtime.  CE's Lua DLL is an ordinary Lua 5.3 build, so a plain
// luaL_newstate() + luaL_openlibs() gives exactly the standard library a
// standalone script expects.  Only the print() override is CEditor-specific.
// ----------------------------------------------------------------------------

typedef void* lua_State;
typedef int (__cdecl* CE_LUA_CFUNCTION)(lua_State*);

typedef struct _CE_LUA_API {
    void* (__cdecl* newState)(void);
    void (__cdecl* openLibs)(lua_State*);
    int (__cdecl* loadString)(lua_State*, const char*);
    int (__cdecl* pcall)(lua_State*, int, int, int, INT_PTR, void*);
    const char* (__cdecl* tolstring)(lua_State*, int, size_t*);
    void* (__cdecl* touserdata)(lua_State*, int);
    void (__cdecl* setglobal)(lua_State*, const char*);
    void (__cdecl* pushcclosure)(lua_State*, CE_LUA_CFUNCTION, int);
    void (__cdecl* pushlstring)(lua_State*, const char*, size_t);
    void (__cdecl* pushlightuserdata)(lua_State*, void*);
    void (__cdecl* close)(lua_State*);
} CE_LUA_API;

// Only one script run is in flight (guarded by CeBridge.c), so the callback can
// reuse these pointers; they are valid from load until lua_close().
static CE_LUA_API s_lua = { 0 };

static const char s_errNoRuntime[] =
    "Local Lua runtime unavailable: lua53-64.dll / lua53-32.dll was not found and no compatible Lua DLL could be loaded.";
static const char s_hintRuntime[] =
    "Put Notepad3.exe in the Cheat Engine folder, set [Settings2] CeInstallPath, or place a Lua 5.3 DLL on PATH. Scripts using the CE API still run through the CE bridge.";

#define BIND_LUA(h, field, name)  *((FARPROC*)&s_lua.field) = GetProcAddress((h), (name))

static bool _CeLua_BindApi(HMODULE hDll)
{
    memset(&s_lua, 0, sizeof(s_lua));
    BIND_LUA(hDll, newState, "luaL_newstate");
    BIND_LUA(hDll, openLibs, "luaL_openlibs");
    BIND_LUA(hDll, loadString, "luaL_loadstring");
    BIND_LUA(hDll, pcall, "lua_pcallk");
    BIND_LUA(hDll, tolstring, "lua_tolstring");
    BIND_LUA(hDll, touserdata, "lua_touserdata");
    BIND_LUA(hDll, setglobal, "lua_setglobal");
    BIND_LUA(hDll, pushcclosure, "lua_pushcclosure");
    BIND_LUA(hDll, pushlstring, "lua_pushlstring");
    BIND_LUA(hDll, pushlightuserdata, "lua_pushlightuserdata");
    BIND_LUA(hDll, close, "lua_close");
    return s_lua.newState && s_lua.openLibs && s_lua.loadString && s_lua.pcall &&
           s_lua.tolstring && s_lua.touserdata && s_lua.setglobal &&
           s_lua.pushcclosure && s_lua.pushlstring && s_lua.pushlightuserdata &&
           s_lua.close;
}

static bool _CeLua_TryLoad(LPCWSTR path, HMODULE* out)
{
    if (!path || !path[0]) {
        return false;
    }
    HMODULE const h = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        return false;
    }
    if (_CeLua_BindApi(h)) {
        *out = h;
        return true;
    }
    FreeLibrary(h);
    return false;
}

// The folders that are searched for the Lua runtime.  They are the same ones
// the CE bridge uses to find the Cheat Engine installation, so the runtime is
// picked up automatically from the CE version the program belongs to.
static void _CeLua_RuntimeRoots(LPCWSTR* roots, size_t cchRoots, WCHAR* ceDir, size_t cchCeDir)
{
    if (cchCeDir > 0) {
        ceDir[0] = L'\0';
    }
    bool const haveCeDir = CeBridge_GetCeDir(ceDir);
    LPCWSTR const found[] = {
        Path_Get(Paths.ModuleDirectory),
        haveCeDir ? ceDir : L"",
        Settings2.CeInstallPath
    };
    for (size_t i = 0; i < cchRoots && i < COUNTOF(found); ++i) {
        roots[i] = found[i];
    }
    for (size_t i = COUNTOF(found); i < cchRoots; ++i) {
        roots[i] = L"";
    }
}

// Describes where the runtime was looked for, so a missing environment can be
// reported with the paths that were actually tried.
static void _CeLua_RuntimeHint(WCHAR* buf, size_t cch)
{
    WCHAR ceDir[MAX_PATH] = { L'\0' };
    bool const haveCeDir = CeBridge_GetCeDir(ceDir);
    StringCchPrintfW(buf, cch,
        L"未找到本地 Lua 运行时（lua53-64.dll / lua53-32.dll）。\n\n"
        L"已查找的位置：\n"
        L"  程序目录：%s\n"
        L"  程序所属的 CE：%s\n"
        L"  配置的 CeInstallPath：%s\n"
        L"  系统 PATH\n\n"
        L"请把 Notepad3.exe 放进 Cheat Engine 目录（或其 autorun\\notepad 子目录），"
        L"或在 Notepad3.ini 的 [Settings2] 中设置 CeInstallPath。\n"
        L"使用 CE API 的脚本仍可通过 CE 桥接运行。",
        Path_Get(Paths.ModuleDirectory),
        haveCeDir ? ceDir : L"（未检测到）",
        Settings2.CeInstallPath[0] ? Settings2.CeInstallPath : L"（未设置）");
}

static bool _CeLua_LoadRuntime(HMODULE* out)
{
#if defined(_WIN64)
    static const WCHAR* const names[] = { L"lua53-64.dll", L"lua53.dll", L"lua54.dll", L"lua.dll" };
#else
    static const WCHAR* const names[] = { L"lua53-32.dll", L"lua53.dll", L"lua.dll" };
#endif
    WCHAR path[MAX_PATH] = { L'\0' };
    WCHAR ceDir[MAX_PATH] = { L'\0' };
    LPCWSTR roots[3] = { L"", L"", L"" };
    _CeLua_RuntimeRoots(roots, COUNTOF(roots), ceDir, COUNTOF(ceDir));

    for (size_t n = 0; n < COUNTOF(names); ++n) {
        for (size_t r = 0; r < COUNTOF(roots); ++r) {
            if (roots[r][0] &&
                SUCCEEDED(StringCchPrintfW(path, COUNTOF(path), L"%s\\%s", roots[r], names[n])) &&
                _CeLua_TryLoad(path, out)) {
                return true;
            }
        }
        if (_CeLua_TryLoad(names[n], out)) {
            return true;
        }
    }
    return false;
}

static int __cdecl _CeLua_Emit(lua_State* L)
{
    HWND const hwnd = (HWND)s_lua.touserdata(L, 1);
    size_t kindLen = 0;
    size_t textLen = 0;
    const char* const kind = s_lua.tolstring(L, 2, &kindLen);
    const char* const text = s_lua.tolstring(L, 3, &textLen);
    if (hwnd && kind && text) {
        BYTE k = (BYTE)CE_BRIDGE_OUTPUT;
        if (kindLen == 5 && memcmp(kind, "ERROR", 5) == 0) {
            k = (BYTE)CE_BRIDGE_ERROR;
        }
        else if (kindLen == 6 && memcmp(kind, "RESULT", 6) == 0) {
            k = (BYTE)CE_BRIDGE_RESULT;
        }
        else if (kindLen == 4 && memcmp(kind, "INFO", 4) == 0) {
            k = (BYTE)CE_BRIDGE_INFO;
        }
        char* const buf = (char*)AllocMem(textLen + 1, HEAP_ZERO_MEMORY);
        if (buf) {
            memcpy(buf, text, textLen);
            CeBridge_PostLine(hwnd, k, buf);  // ownership moves to the main thread
        }
    }
    return 0;
}

// The wrapper keeps the user's script in a global (no long-bracket escaping),
// overrides print() and guards against infinite loops with a debug hook.
static const char s_luaWrapper[] =
    "local hwnd = np3_hwnd\n"
    "local emit = np3_emit\n"
    "local oldprint = print\n"
    "print = function(...)\n"
    "  local parts = {}\n"
    "  for i = 1, select('#', ...) do parts[i] = tostring(select(i, ...)) end\n"
    "  emit(hwnd, 'OUT', table.concat(parts, '\\t'))\n"
    "end\n"
    "if type(os) == 'table' then\n"
    "  os.exit = function() error('NP3: os.exit() is disabled in the local Lua runtime') end\n"
    "end\n"
    "emit(hwnd, 'INFO', 'Lua ' .. (_VERSION or '?'))\n"
    "local chunk, err = load(np3_code, '=Notepad3', 't')\n"
    "local ok, res\n"
    "if not chunk then\n"
    "  ok, res = false, err\n"
    "else\n"
    "  local hasDebug = type(debug) == 'table' and debug.sethook and debug.gethook\n"
    "  local oldHook, oldMask, oldCount\n"
    "  if hasDebug then\n"
    "    oldHook, oldMask, oldCount = debug.gethook()\n"
    "    local deadline = os.clock() + 30\n"
    "    debug.sethook(function()\n"
    "      if os.clock() > deadline then error('NP3: script execution timed out') end\n"
    "    end, '', 100000)\n"
    "  end\n"
    "  ok, res = pcall(chunk)\n"
    "  if hasDebug then\n"
    "    if oldHook then debug.sethook(oldHook, oldMask or '', oldCount or 0)\n"
    "    else debug.sethook() end\n"
    "  end\n"
    "end\n"
    "print = oldprint\n"
    "np3_code, np3_hwnd, np3_emit = nil, nil, nil\n"
    "if ok then\n"
    "  if res ~= nil then emit(hwnd, 'RESULT', tostring(res)) end\n"
    "  emit(hwnd, 'INFO', 'local Lua finished')\n"
    "else\n"
    "  emit(hwnd, 'ERROR', tostring(res))\n"
    "end\n";

void CeLua_Execute(HWND hwnd, const char* pszScript, size_t len)
{
    HMODULE hDll = NULL;
    if (!_CeLua_LoadRuntime(&hDll)) {
        CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, s_errNoRuntime);
        CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_INFO, s_hintRuntime);
        // Tell the user which folders were searched (modal hint on the main
        // thread), otherwise the missing runtime stays unnoticed.
        WCHAR hint[2048] = { L'\0' };
        _CeLua_RuntimeHint(hint, COUNTOF(hint));
        char utf8[4096] = { L'\0' };
        int const cch = WideCharToMultiByte(CP_UTF8, 0, hint, -1, utf8, (int)COUNTOF(utf8),
                                            NULL, NULL);
        if (cch > 0) {
            CeBridge_PostHint(hwnd, utf8);
        }
        return;
    }

    lua_State* const L = (lua_State*)s_lua.newState();
    if (!L) {
        CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR, "luaL_newstate() failed.");
        FreeLibrary(hDll);
        return;
    }

    s_lua.openLibs(L);
    s_lua.pushlightuserdata(L, (void*)hwnd);
    s_lua.setglobal(L, "np3_hwnd");
    s_lua.pushlstring(L, pszScript, len);
    s_lua.setglobal(L, "np3_code");
    s_lua.pushcclosure(L, &_CeLua_Emit, 0);
    s_lua.setglobal(L, "np3_emit");

    int status = s_lua.loadString(L, s_luaWrapper);
    if (status == 0) {
        status = s_lua.pcall(L, 0, 0, 0, 0, NULL);
    }
    if (status != 0) {
        const char* const err = s_lua.tolstring(L, -1, NULL);
        CeBridge_PostConstLine(hwnd, (BYTE)CE_BRIDGE_ERROR,
                               (err && err[0]) ? err : "Local Lua runtime error.");
    }

    s_lua.close(L);
    FreeLibrary(hDll);
}
