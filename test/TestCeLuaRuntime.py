#!/usr/bin/env python3
"""Smoke test for the local Lua runner's wrapper in ``src/CeLua.c``.

The C side loads CE's ``lua53-64.dll`` and executes ``s_luaWrapper`` in a fresh
Lua state.  This harness does the same with ctypes: it reads the wrapper
verbatim from the C source, binds a fake ``np3_emit`` and checks that pure Lua
``print`` output, return values, syntax errors and runtime errors are streamed
back.  It skips when no Lua DLL is available (e.g. on CI).

    python test/TestCeLuaRuntime.py --lua-dll "E:/Cheat Engine/Cheat Engine 7.7/lua53-64.dll"
"""

from __future__ import annotations

import argparse
import ctypes
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "CeLua.c"
DEFAULT_DLLS = (
    r"E:\Cheat Engine\Cheat Engine 7.7\lua53-64.dll",
    r"C:\Program Files\Cheat Engine 7.7\lua53-64.dll",
    "lua53-64.dll",
)


def load_wrapper() -> str:
    """Reassemble the concatenated C literals of s_luaWrapper[]."""
    text = SOURCE.read_text(encoding="utf-8")
    match = re.search(r"static const char s_luaWrapper\[\] =\s*(.*?);", text, re.S)
    if not match:
        raise RuntimeError("s_luaWrapper not found in src/CeLua.c")
    literals = re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1))
    if not literals:
        raise RuntimeError("s_luaWrapper has no string literals")
    return "".join(json.loads('"' + literal + '"') for literal in literals)


def bind(lib: ctypes.WinDLL) -> None:
    lib.luaL_newstate.restype = ctypes.c_void_p
    lib.luaL_openlibs.argtypes = [ctypes.c_void_p]
    lib.luaL_loadstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.luaL_loadstring.restype = ctypes.c_int
    lib.lua_pcallk.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_ssize_t, ctypes.c_void_p,
    ]
    lib.lua_pcallk.restype = ctypes.c_int
    lib.lua_tolstring.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.lua_tolstring.restype = ctypes.c_char_p
    lib.lua_touserdata.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.lua_touserdata.restype = ctypes.c_void_p
    lib.lua_setglobal.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.lua_pushcclosure.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    lib.lua_pushlstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
    lib.lua_pushlightuserdata.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.lua_close.argtypes = [ctypes.c_void_p]


def run(lib: ctypes.WinDLL, wrapper: str, script: str) -> tuple[int, list[tuple[str, str]]]:
    emitted: list[tuple[str, str]] = []

    @ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
    def emit(state: int) -> int:
        if not lib.lua_touserdata(state, 1):
            return 0
        kind = lib.lua_tolstring(state, 2, None)
        text = lib.lua_tolstring(state, 3, None)
        emitted.append(((kind or b"").decode("utf-8", "replace"),
                        (text or b"").decode("utf-8", "replace")))
        return 0

    callback = emit  # keep the CFUNCTYPE object alive for the whole call
    state = lib.luaL_newstate()
    if not state:
        raise RuntimeError("luaL_newstate failed")
    try:
        lib.luaL_openlibs(state)
        lib.lua_pushlightuserdata(state, ctypes.c_void_p(1))
        lib.lua_setglobal(state, b"np3_hwnd")
        source = script.encode("utf-8")
        lib.lua_pushlstring(state, source, len(source))
        lib.lua_setglobal(state, b"np3_code")
        lib.lua_pushcclosure(state, ctypes.cast(callback, ctypes.c_void_p), 0)
        lib.lua_setglobal(state, b"np3_emit")

        code = wrapper.encode("utf-8")
        status = lib.luaL_loadstring(state, code)
        if status == 0:
            status = lib.lua_pcallk(state, 0, 0, 0, 0, None)
        if status != 0:
            error = lib.lua_tolstring(state, -1, None)
            emitted.append(("WRAPPER", (error or b"").decode("utf-8", "replace")))
        return status, emitted
    finally:
        lib.lua_close(state)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lua-dll", default=None, help="path to a Lua 5.3 DLL")
    args = parser.parse_args()

    candidates = [args.lua_dll] if args.lua_dll else list(DEFAULT_DLLS)
    lib = None
    for candidate in candidates:
        try:
            lib = ctypes.WinDLL(candidate)
            break
        except OSError:
            continue
    if lib is None:
        print(f"SKIP: no Lua DLL found (tried {', '.join(candidates)})")
        return 0

    wrapper = load_wrapper()
    bind(lib)

    failures = 0

    def check(condition: bool, message: str) -> None:
        nonlocal failures
        if not condition:
            print("FAIL: " + message)
            failures += 1

    status, out = run(lib, wrapper, "print('hello', 42)\nreturn 7")
    kinds = dict(out)
    check(status == 0, f"wrapper failed: {out}")
    check(kinds.get("OUT") == "hello\t42", f"print() not streamed: {out}")
    check(kinds.get("RESULT") == "7", f"return value not streamed: {out}")
    check(any(k == "INFO" and v.startswith("Lua ") for k, v in out), f"no Lua banner: {out}")

    status, out = run(lib, wrapper, "this is not valid lua !")
    check(status == 0, f"wrapper failed on syntax error: {out}")
    check(any(k == "ERROR" for k, _ in out), f"syntax error not reported: {out}")

    status, out = run(lib, wrapper, "error('boom')")
    check(status == 0, f"wrapper failed on runtime error: {out}")
    check(any(k == "ERROR" and "boom" in v for k, v in out), f"runtime error not reported: {out}")

    status, out = run(lib, wrapper, "print('x')\nlocal t = getAddress('game.exe')")
    check(status == 0, f"wrapper failed on missing CE API: {out}")
    check(any(k == "OUT" and v == "x" for k, v in out), "print before CE call was lost")
    check(any(k == "ERROR" for k, _ in out), "missing CE API should surface as an error")

    # os.exit() would terminate the editor; the wrapper must trap it.
    status, out = run(lib, wrapper, "os.exit()")
    check(status == 0, f"wrapper failed on os.exit(): {out}")
    check(any(k == "ERROR" and "os.exit" in v for k, v in out),
          f"os.exit() was not neutralised: {out}")

    if failures:
        print(f"local Lua runtime: {failures} failure(s)")
        return 1
    print("local Lua runtime: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
