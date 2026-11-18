"""End-to-end regression for ce_bridge/np3_ce_bridge.lua.

Runs the real bridge script with CE's own lua53-64.dll while stubbing the CE
API and the named pipe, so the wire protocol and the np3 convenience API can
be checked without starting Cheat Engine.  Also verifies that the built
Notepad3.exe embeds the very same script as RCDATA resource 1051.

Usage:
    python test\\TestCeBridgeLua.py ^
        --lua-dll "E:\\Cheat Engine\\Cheat Engine 7.7\\lua53-64.dll" ^
        --exe Bin\\Release_x64_v145\\Notepad3.exe
"""

import argparse
import ctypes
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
BRIDGE = os.path.join(ROOT, 'ce_bridge', 'np3_ce_bridge.lua')
TEMPLATE_FILE = os.path.join(ROOT, 'ce_bridge', 'np3_user_templates.tpl')
PIPE_NAME = 'NP3_CE_BRIDGE'
RESOURCE_ID = 1051
TEMPLATE_RESOURCE_ID = 1053

DRIVER = r"""
local userCode = [===[__USER_CODE__]===]
local reqData = string.char(1) .. string.pack('<I4', #userCode) .. userCode
local inpos = 1
local outbuf = {}
local function readN(n)
  local s = reqData:sub(inpos, inpos + n - 1)
  inpos = inpos + n
  return s
end
-- CE binds LuaPipe methods as C closures over the object: they receive only
-- the real arguments, never 'self'.  Enforce that here so a stray ':'-call
-- (which silently drops frames in CE) fails the regression instead.
local function expectArgs(expected, ...)
  local got = select('#', ...)
  assert(got == expected,
    'CE LuaPipe method got ' .. got .. ' args, expected ' .. expected ..
    ' (colon syntax instead of dot syntax?)')
end
local pipe = {
  Connected = true,
  readByte = function(...)
    expectArgs(0, ...)
    return readN(1):byte(1, 1)
  end,
  readDword = function(...)
    expectArgs(0, ...)
    return (string.unpack('<I4', readN(4)))
  end,
  readString = function(n, ...)
    expectArgs(1, n, ...)
    assert(type(n) == 'number', 'readString(size): size is not a number')
    return readN(n)
  end,
  writeByte = function(b, ...)
    expectArgs(1, b, ...)
    assert(type(b) == 'number', 'writeByte(byte): byte is not a number')
    outbuf[#outbuf + 1] = string.char(b)
    return 1
  end,
  writeDword = function(d, ...)
    expectArgs(1, d, ...)
    assert(type(d) == 'number', 'writeDword(value): value is not a number')
    outbuf[#outbuf + 1] = string.pack('<I4', d)
    return 4
  end,
  writeString = function(s, ...)
    expectArgs(1, s, ...)
    assert(type(s) == 'string', 'writeString(text): text is not a string')
    outbuf[#outbuf + 1] = s
    return #s
  end,
  lock = function(...)
    expectArgs(0, ...)
    error('LuaPipe:lock() is forbidden: CE raises "Criticalsection leave without enter"')
  end,
  unlock = function(...)
    expectArgs(0, ...)
    error('LuaPipe:unlock() is forbidden: CE raises "Criticalsection leave without enter"')
  end,
  destroy = function(...) expectArgs(0, ...) end,
}
local acceptCount = 0
local server = {
  valid = true,
  acceptConnection = function(split, ...)
    expectArgs(1, split, ...)
    assert(split == true, 'acceptConnection(split): split flag missing')
    acceptCount = acceptCount + 1
    if acceptCount == 1 then return pipe end
    NP3_CE_BRIDGE.running = false
    return nil
  end,
}
local pipeName = nil
_G.createPipe = function(name, i, o, m) pipeName = name; return server end
_G.createTimer = function() return {} end
_G.createThread = function(f) f(); return {} end
_G.sleep = function() end
_G.print = function() end
_G.synchronize = nil
_G.checkSynchronize = nil
_G.inMainThread = function() return true end
_G.getOpenedProcessID = function() return 1234 end
_G.readBytes = function(addr, count, asTable) return { 1, 2, 3 } end
_G.writeBytes = function() return true end
_G.getAddress = function(name) return 4096 end
_G.getNameFromAddress = function() return 'stub' end
_G.autoAssemble = function() return true end
_G.debug_setBreakpoint = function(addr, cb) if cb then cb() end; return 7 end
_G.createDisassembler = function()
  return {
    disassemble = function(addr, ...)
      expectArgs(1, addr, ...)
      assert(type(addr) == 'number', 'disassemble(address): address is not a number')
      return 'mov eax,eax'
    end,
    getLastDisassembleData = function(...)
      expectArgs(0, ...)
      return { bytes = { 0x90 }, parameters = '' }
    end,
  }
end

__SETUP__

local chunk = assert(load([===[__BRIDGE_SRC__]===], '=np3_ce_bridge'))
chunk()
assert(pipeName == 'NP3_CE_BRIDGE', 'wrong pipe name: ' .. tostring(pipeName))
assert(acceptCount == 2, 'listener did not finish')
assert(type(np3) == 'table', 'np3 convenience API missing')

local resp = table.concat(outbuf)
local frames = {}
local pos = 1
while pos + 5 <= #resp do
  local kind = resp:byte(pos)
  local len = (string.unpack('<I4', resp, pos + 1))
  local text = resp:sub(pos + 5, pos + 4 + len)
  frames[#frames + 1] = { kind = kind, text = text }
  pos = pos + 5 + len
end
assert(pos == #resp + 1, 'trailing bytes in response stream')
__ASSERTIONS__
"""

SCENARIOS = [
    (
        'print + return value',
        "print('hello', 42) return 7",
        """
assert(#frames == 4, 'frame count: ' .. #frames)
assert(frames[1].kind == 4, 'missing INFO banner')
assert(frames[2].kind == 0 and frames[2].text == 'hello\\t42', 'bad print frame: ' .. frames[2].text)
assert(frames[3].kind == 1 and frames[3].text == '7', 'bad result frame: ' .. frames[3].text)
assert(frames[4].kind == 3 and frames[4].text == '0', 'bad DONE frame')
""",
        '',
    ),
    (
        'CE listener thread + synchronize',
        "print('hello', 42) return 7",
        """
assert(#frames == 4, 'synchronize frame count: ' .. #frames)
assert(frames[1].kind == 4, 'missing INFO banner')
assert(frames[2].kind == 0 and frames[2].text == 'hello\\t42', 'bad print frame: ' .. frames[2].text)
assert(frames[3].kind == 1 and frames[3].text == '7', 'bad result frame: ' .. frames[3].text)
assert(frames[4].kind == 3 and frames[4].text == '0', 'bad DONE frame')
""",
        """
_G.inMainThread = function() return false end
_G.synchronize = function(f) return f() end
""",
    ),
    (
        'runtime error',
        "error('boom')",
        """
assert(#frames == 3, 'frame count: ' .. #frames)
assert(frames[2].kind == 2 and frames[2].text:find('boom', 1, true), 'missing error text')
assert(frames[3].kind == 3 and frames[3].text == '1', 'failed run must report non-zero DONE')
""",
        '',
    ),
    (
        'np3 target-process helpers',
        "local d = np3.disassemble(4096, 1) return np3.process().pid + #np3.read(4096, 3) + (d:find('mov', 1, true) and 1 or 0)",
        """
assert(#frames == 3, 'frame count: ' .. #frames)
assert(frames[1].kind == 4, 'missing INFO banner')
assert(frames[2].kind == 1 and frames[2].text == '1238', 'np3 helpers: ' .. frames[2].text)
assert(frames[3].kind == 3 and frames[3].text == '0', 'np3 run must succeed')
""",
        '',
    ),
    (
        'np3 hook output + event log',
        # The hook callback replaces print() with its own wrapper: the output
        # is always buffered in the event log, and it is only streamed to the
        # pipe (i.e. to Notepad3) when the bridge mirrors to CE as well.
        "NP3_CE_BRIDGE.echoToCe = true; np3.hook(4096, function() print('hit') end) return np3.dumpEvents()",
        """
assert(#frames == 4, 'frame count: ' .. #frames)
assert(frames[2].kind == 0 and frames[2].text == 'hit', 'hook print not streamed: ' .. frames[2].text)
assert(frames[3].kind == 1 and frames[3].text == 'hit', 'hook log not readable: ' .. frames[3].text)
assert(frames[4].kind == 3 and frames[4].text == '0', 'hook run must succeed')
""",
        '',
    ),
]


def make_lua(bridge_src, code, assertions, setup):
    if ']===' in bridge_src or ']===' in code or ']===' in setup:
        raise RuntimeError('long-bracket delimiter collision')
    driver = DRIVER.replace('__BRIDGE_SRC__', bridge_src)
    driver = driver.replace('__USER_CODE__', code)
    driver = driver.replace('__SETUP__', setup)
    return driver.replace('__ASSERTIONS__', assertions).encode('utf-8')


def run_scenarios(lua_dll, bridge_src):
    lib = ctypes.CDLL(lua_dll)
    lib.luaL_newstate.restype = ctypes.c_void_p
    lib.luaL_openlibs.argtypes = [ctypes.c_void_p]
    lib.luaL_loadstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.luaL_loadstring.restype = ctypes.c_int
    lib.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                  ctypes.POINTER(ctypes.c_size_t)]
    lib.lua_tolstring.restype = ctypes.c_char_p
    lib.lua_close.argtypes = [ctypes.c_void_p]
    try:
        lib.lua_pcallk.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
        lib.lua_pcallk.restype = ctypes.c_int
        pcall = lambda state: lib.lua_pcallk(state, 0, 0, 0, None, None)
    except AttributeError:
        lib.lua_pcall.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        lib.lua_pcall.restype = ctypes.c_int
        pcall = lambda state: lib.lua_pcall(state, 0, 0, 0)

    passed = 0
    for name, code, assertions, setup in SCENARIOS:
        state = lib.luaL_newstate()
        if not state:
            raise RuntimeError('luaL_newstate failed')
        try:
            lib.luaL_openlibs(state)
            driver = make_lua(bridge_src, code, assertions, setup)
            status = lib.luaL_loadstring(state, driver)
            if status == 0:
                status = pcall(state)
            if status != 0:
                err = lib.lua_tolstring(state, -1, ctypes.byref(ctypes.c_size_t()))
                raise AssertionError(name + ': ' + (err or b'unknown').decode('utf-8', 'replace'))
            print('  PASS ' + name)
            passed += 1
        finally:
            lib.lua_close(state)
    return passed


def check_embedded_resource(exe, src, resource_id=RESOURCE_ID, label='bridge script'):
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
    kernel32.LoadLibraryExW.restype = ctypes.c_void_p
    kernel32.FindResourceExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.c_void_p, ctypes.c_uint16]
    kernel32.FindResourceExW.restype = ctypes.c_void_p
    kernel32.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.LoadResource.restype = ctypes.c_void_p
    kernel32.LockResource.argtypes = [ctypes.c_void_p]
    kernel32.LockResource.restype = ctypes.c_void_p
    kernel32.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.SizeofResource.restype = ctypes.c_uint32
    kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]

    module = kernel32.LoadLibraryExW(os.path.abspath(exe), None, 0x00000002)  # LOAD_LIBRARY_AS_DATAFILE
    if not module:
        raise OSError('cannot load ' + exe)
    try:
        res = kernel32.FindResourceExW(module, 10, resource_id, 0)  # RT_RCDATA, LANG_NEUTRAL
        if not res:
            raise AssertionError('RCDATA resource %d missing from %s' % (resource_id, exe))
        size = kernel32.SizeofResource(module, res)
        handle = kernel32.LoadResource(module, res)
        data = ctypes.string_at(kernel32.LockResource(handle), size)
        expected = src.encode('utf-8')
        # The resource compiler stores the file verbatim (LF/CRLF differences are tolerated).
        if data.replace(b'\r\n', b'\n') != expected.replace(b'\r\n', b'\n'):
            raise AssertionError('embedded %s differs from its source file' % label)
        print('  PASS embedded RCDATA %d (%d bytes)' % (resource_id, size))
        return 1
    finally:
        kernel32.FreeLibrary(module)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua-dll', default=r'E:\Cheat Engine\Cheat Engine 7.7\lua53-64.dll')
    parser.add_argument('--exe', default=os.path.join(ROOT, 'Bin', 'Release_x64_v145', 'Notepad3.exe'))
    args = parser.parse_args()

    with open(BRIDGE, 'r', encoding='utf-8') as handle:
        bridge_src = handle.read()
    with open(TEMPLATE_FILE, 'r', encoding='utf-8') as handle:
        template_src = handle.read()

    if not os.path.isfile(args.lua_dll):
        print('SKIP: CE lua53 DLL not found at ' + args.lua_dll)
        return 0
    print('CE bridge Lua/regression (Lua 5.3 runtime: %s)' % args.lua_dll)
    passed = run_scenarios(args.lua_dll, bridge_src)
    if os.path.isfile(args.exe):
        passed += check_embedded_resource(args.exe, bridge_src)
        passed += check_embedded_resource(args.exe, template_src,
                                          TEMPLATE_RESOURCE_ID, 'default templates')
    else:
        print('SKIP: built exe not found at ' + args.exe)
    print('CE bridge Lua tests: %d passed' % passed)
    return 0


if __name__ == '__main__':
    sys.exit(main())
