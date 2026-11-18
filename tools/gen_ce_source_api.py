#!/usr/bin/env python3
"""Collect CE Lua names that ``celua.txt`` does not document.

Cheat Engine registers a number of Lua globals and class methods directly from
its Pascal sources.  The checked-in ``celua_api.json`` is generated from CE's
``celua.txt`` and therefore misses them.  This script reads a local copy of the
upstream sources and emits ``ce_edit/ce_edit/data/ce_source_api.json``, which
``tools/gen_ce_keywords.py`` merges into the C completion catalogue.

Sources (cheat-engine/cheat-engine @ ec45d5f4, directory ``Cheat Engine/``):

    LuaCaller.pas, LuaClass.pas, LuaHandler.pas, LuaObject.pas,
    LuaStructure.pas, LuaThread.pas, memscan.pas, lua/lua.pas

Download them into one directory, then run::

    python tools/gen_ce_source_api.py --source-dir path/to/pascal/sources
    python tools/gen_ce_keywords.py

Without ``--source-dir`` (or ``CE_PASCAL_SRC``) the Pascal-derived rows already
in the checked-in JSON are preserved and only the curated Lua 5.3 / CE class
additions are refreshed, so regenerating without network access is idempotent.
"""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_JSON = ROOT / "ce_edit" / "ce_edit" / "data" / "ce_source_api.json"
CELUA_API = ROOT / "ce_edit" / "ce_edit" / "data" / "celua_api.json"
COMPLETIONS = ROOT / "ce_edit" / "ce_edit" / "data" / "completions.json"

_IDENT = re.compile(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*\Z")
_REGISTER = re.compile(r"lua_register\s*\(\s*\w+\s*,\s*'([^']+)'", re.I)
_CLASS_METHOD = re.compile(
    r"luaclass_addClassFunctionToTable\s*\([^;]*?,\s*'([^']+)'", re.I | re.S
)

# CE classes exposed to Lua (celua.txt class headings / Lua* bindings).  Pure
# UI widget names are deliberately left out: they are too generic for a
# completion list, exactly like the properties skipped by gen_ce_lua_api.py.
CE_CLASSES = {
    "MemScan": "内存扫描对象 (createMemScan)",
    "FoundList": "扫描结果列表 (createFoundList)",
    "Structure": "结构体对象",
    "StructureElement": "结构体元素",
    "StructureFrm": "结构体窗口",
    "DissectCode": "代码反汇编统计",
    "Disassembler": "反汇编器 (createDisassembler)",
    "Disassemblerview": "反汇编视图",
    "DisassemblerviewLine": "反汇编视图行",
    "Hexadecimal": "十六进制视图",
    "LuaPipe": "命名管道基类",
    "LuaPipeServer": "命名管道服务端 (createPipe)",
    "LuaPipeClient": "命名管道客户端 (connectToPipe)",
    "LuaServerSocket": "TCP 服务端",
    "LuaClientSocket": "TCP 客户端",
    "Thread": "线程对象 (createThread)",
    "Timer": "定时器对象 (createTimer)",
    "CriticalSection": "临界区 (createCriticalSection)",
    "Event": "事件对象 (createEvent)",
    "Semaphore": "信号量 (createSemaphore)",
    "MultiReadExclusiveWriteSynchronizer": "多读单写同步器",
    "SymbolList": "符号列表",
    "Stringlist": "字符串列表",
    "Strings": "字符串集合",
    "MemoryStream": "内存流",
    "FileStream": "文件流",
    "StringStream": "字符串流",
    "TableFile": "表文件",
    "MemoryRecordHotkey": "内存记录热键",
    "GenericHotkey": "全局热键",
    "CheatComponent": "CE 组件基类",
    "CEForm": "CE 窗体",
    "CEServerInterface": "CEServer 接口",
    "WriteLog": "日志对象",
    "Settings": "CE 设置对象",
    "DotNetDataCollector": ".NET 数据采集器",
    "RemoteThread": "远程线程",
    "RemoteExecutor": "远程执行器",
    "RIPRelativeScanner": "RIP 相对寻址扫描器",
    "D3DHOOK": "D3D Hook 对象",
    "Memoryview": "内存视图",
    "Pagecontrol": "分页控件",
    "TreeNode": "树节点",
    "TreeNodes": "树节点集合",
    "Listview": "列表视图",
    "ListColumn": "列表列",
    "ListColumns": "列表列集合",
    "ListItem": "列表项",
    "ListItems": "列表项集合",
    "SynEdit": "SynEdit 编辑器控件",
    "Menu": "菜单",
    "MenuItem": "菜单项",
    "MainMenu": "主菜单",
    "PopupMenu": "弹出菜单",
    "ProgressBar": "进度条",
    "PaintBox": "绘图面板",
}

# Lua 5.3 library members missing from the old catalogue.  lua/lua.pas is CE's
# Lua 5.3 binding header (LUA_VERSION_NUM = 503); LuaHandler.pas additionally
# installs frexp/cosh/sinh/tanh into math and split/trim/etc. into string.
LUA53_STDLIB = {
    "rawlen": ("rawlen(v)", "语言基础", True),
    "math.acos": ("math.acos(x)", "math 库", True),
    "math.asin": ("math.asin(x)", "math 库", True),
    "math.atan": ("math.atan(y [, x])", "math 库", True),
    "math.maxinteger": ("math.maxinteger -- 最大整数常量", "math 库", False),
    "math.mininteger": ("math.mininteger -- 最小整数常量", "math 库", False),
    "math.ult": ("math.ult(m, n)", "math 库", True),
    "math.ldexp": ("math.ldexp(m, e)", "math 库", True),
    "math.pow": ("math.pow(x, y)", "math 库", True),
    "math.frexp": ("math.frexp(x) -- CE 扩展", "math 库", True),
    "math.cosh": ("math.cosh(x) -- CE 扩展", "math 库", True),
    "math.sinh": ("math.sinh(x) -- CE 扩展", "math 库", True),
    "math.tanh": ("math.tanh(x) -- CE 扩展", "math 库", True),
    "string.dump": ("string.dump(function [, strip])", "string 库", True),
    "string.pack": ("string.pack(fmt, v1, ...)", "string 库", True),
    "string.packsize": ("string.packsize(fmt)", "string 库", True),
    "string.unpack": ("string.unpack(fmt, s [, pos])", "string 库", True),
    "os.setlocale": ("os.setlocale(locale [, category])", "os 库", True),
    "utf8.char": ("utf8.char(...)", "utf8 库", True),
    "utf8.charpattern": ("utf8.charpattern -- UTF-8 字符匹配模式", "utf8 库", False),
    "utf8.codepoint": ("utf8.codepoint(s [, i [, j]])", "utf8 库", True),
    "utf8.codes": ("utf8.codes(s)", "utf8 库", True),
    "utf8.len": ("utf8.len(s [, i [, j]])", "utf8 库", True),
    "utf8.offset": ("utf8.offset(s, n [, i])", "utf8 库", True),
    "coroutine.isyieldable": ("coroutine.isyieldable()", "coroutine 库", True),
    "debug.debug": ("debug.debug()", "debug 库", True),
    "debug.gethook": ("debug.gethook([thread])", "debug 库", True),
    "debug.getinfo": ("debug.getinfo([thread,] f [, what])", "debug 库", True),
    "debug.getlocal": ("debug.getlocal([thread,] f, local)", "debug 库", True),
    "debug.getmetatable": ("debug.getmetatable(value)", "debug 库", True),
    "debug.getregistry": ("debug.getregistry()", "debug 库", True),
    "debug.getupvalue": ("debug.getupvalue(f, up)", "debug 库", True),
    "debug.getuservalue": ("debug.getuservalue(u)", "debug 库", True),
    "debug.sethook": ("debug.sethook([thread,] hook, mask [, count])", "debug 库", True),
    "debug.setlocal": ("debug.setlocal([thread,] level, local, value)", "debug 库", True),
    "debug.setmetatable": ("debug.setmetatable(value, table)", "debug 库", True),
    "debug.setupvalue": ("debug.setupvalue(f, up, value)", "debug 库", True),
    "debug.setuservalue": ("debug.setuservalue(udata, value)", "debug 库", True),
    "debug.traceback": ("debug.traceback([thread,] [message [, level]])", "debug 库", True),
    "debug.upvalueid": ("debug.upvalueid(f, n)", "debug 库", True),
    "debug.upvaluejoin": ("debug.upvaluejoin(f1, n1, f2, n2)", "debug 库", True),
}

# Globals that LuaHandler.pas installs with a generated Lua helper script.
CE_GLOBALS = {
    "printf": ("printf(format, ...) -- 等价于 print(string.format(...))", "CE Lua API", True),
}


def _existing_names() -> set[str]:
    names: set[str] = set()
    for path in (CELUA_API, COMPLETIONS):
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        for item in payload:
            if isinstance(item, dict):
                names.add(str(item.get("name", "")))
    return names


def _pascal_sources(source_dir: Path | None) -> list[tuple[str, str]]:
    if source_dir is None or not source_dir.is_dir():
        return []
    result = []
    for path in sorted(source_dir.rglob("*.pas")):
        if path.name.startswith("_probe"):
            continue
        result.append((path.name, path.read_text(encoding="utf-8", errors="replace")))
    return result


def _preserved_pascal_entries() -> dict[str, dict[str, object]]:
    """Keep source-derived rows when regenerating without the Pascal files."""
    try:
        payload = json.loads(OUT_JSON.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    result: dict[str, dict[str, object]] = {}
    for item in payload:
        if not isinstance(item, dict):
            continue
        name = str(item.get("name", ""))
        if name and str(item.get("source", "")).endswith(".pas"):
            result[name] = item
    return result


def build(source_dir: Path | None) -> list[dict[str, object]]:
    existing = _existing_names()
    entries: dict[str, dict[str, object]] = {}
    for name, item in _preserved_pascal_entries().items():
        if name not in existing and _IDENT.fullmatch(name):
            entries[name] = item

    def add(name: str, signature: str, category: str, is_call: bool, source: str) -> None:
        if not _IDENT.fullmatch(name) or name in existing or name in entries:
            return
        entries[name] = {
            "name": name,
            "signature": signature,
            "category": category,
            "is_call": is_call,
            "source": source,
        }

    for filename, text in _pascal_sources(source_dir):
        for regex in (_REGISTER, _CLASS_METHOD):
            for name in regex.findall(text):
                add(name.strip(), f"{name}(...) -- CE {filename}", "CE Lua API", True, filename)

    for name, desc in CE_CLASSES.items():
        add(name, f"{name} -- {desc}", "对象", False, "celua.txt / LuaClass.pas")

    for name, (signature, category, is_call) in LUA53_STDLIB.items():
        add(name, signature, category, is_call, "lua/lua.pas (Lua 5.3)")

    for name, (signature, category, is_call) in CE_GLOBALS.items():
        add(name, signature, category, is_call, "LuaHandler.pas")

    return sorted(entries.values(), key=lambda item: str(item["name"]).casefold())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=None,
                        help="directory holding the CE Pascal sources (or CE_PASCAL_SRC)")
    parser.add_argument("--out", type=Path, default=OUT_JSON, help="output JSON path")
    args = parser.parse_args()

    source_dir = args.source_dir or (Path(os.environ["CE_PASCAL_SRC"])
                                     if os.environ.get("CE_PASCAL_SRC") else None)
    entries = build(source_dir)
    args.out.write_text(
        json.dumps(entries, ensure_ascii=False, indent=1) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(f"wrote {args.out} ({len(entries)} entries, source-dir={source_dir})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
