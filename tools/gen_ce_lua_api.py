#!/usr/bin/env python3
"""Extract Cheat Engine's Lua API from ``celua.txt`` into a checked-in catalogue.

``celua.txt`` ships with Cheat Engine (it is not part of this repository), so
this script is only needed when a newer CE build is installed.  The generated
``ce_edit/ce_edit/data/celua_api.json`` is the input of
``tools/gen_ce_keywords.py`` and is committed so that a normal build never
depends on the machine-specific Cheat Engine installation.

    python tools/gen_ce_lua_api.py --source "E:/Cheat Engine/Cheat Engine 77/celua.txt"

The source is also taken from the ``CE_LUA_TXT`` environment variable when
``--source`` is omitted.  Only *callable* API entries are emitted: global
functions, class methods and the Lua library extensions.  Properties and free
text are deliberately skipped so that generic words such as ``Name`` do not
pollute the completion list.
"""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_JSON = ROOT / "ce_edit" / "ce_edit" / "data" / "celua_api.json"

CATEGORY = "CE Lua API"
GLOBAL_CATEGORY = "CE 全局"

_IDENT = r"[A-Za-z_]\w*"
_NAME_RE = re.compile(rf"^({_IDENT}(?:\.{_IDENT})*)\s*\(")
_CLASS_RE = re.compile(rf"^\s*({_IDENT}(?:\s+{_IDENT}){{0,2}})\s+class\b", re.I)
_VARIABLE_RE = re.compile(rf"^\s*({_IDENT})\s*:\s*(.+)$")
_SECTION_RE = re.compile(r"^\s*(properties|methods|global functions?|functions?)\s*:?\s*$", re.I)
_HEADING_RE = re.compile(r"^\s*[A-Za-z][A-Za-z0-9 _.()/+-]{2,60}:\s*$")

_DEFAULT_SOURCES = (
    r"E:\Cheat Engine\Cheat Engine 77\celua.txt",
    r"C:\Program Files\Cheat Engine 7.7\celua.txt",
    r"C:\Program Files (x86)\Cheat Engine 7.7\celua.txt",
)


def _closing_paren(text: str, opening: int) -> int:
    """Return the index of the ')' matching text[opening] == '('."""
    depth = 0
    quote = ""
    for i in range(opening, len(text)):
        ch = text[i]
        if quote:
            if ch == quote and text[i - 1] != "\\":
                quote = ""
            continue
        if ch in "'\"":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def _signature(line: str) -> tuple[str, str, str] | None:
    """Parse one 'name(args) : description' line into (name, args, desc)."""
    match = _NAME_RE.match(line)
    if not match:
        return None
    opening = line.index("(", match.start())
    closing = _closing_paren(line, opening)
    if closing < 0:
        return None
    name = match.group(1)
    args = line[opening : closing + 1]
    rest = line[closing + 1 :].strip()
    rest = re.sub(r"^\s*(?:->|[-:]\s*|\u2013|\u2014)\s*", "", rest)
    return name, args, rest.strip()


def _description(rest: str, following: str) -> str:
    text = rest
    if not text and following:
        # A few CE functions put the description on the next indented line.
        candidate = following.strip()
        if candidate and not _NAME_RE.match(candidate) and not _VARIABLE_RE.match(candidate):
            text = candidate
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) > 150:
        text = text[:147].rstrip() + "..."
    return text


def _variant_names(line: str) -> list[str]:
    """Split 'readInteger(address) / readByte(address)' style overload lines."""
    parts = re.split(r"\s+/\s+(?=[A-Za-z_])", line)
    return [part.strip() for part in parts if part.strip()]


def parse(source: Path) -> list[dict[str, str | bool]]:
    text = source.read_bytes().decode("utf-8", "replace")
    lines = text.splitlines()

    entries: dict[str, dict[str, str | bool]] = {}
    in_globals = False
    in_properties = False

    def add(name: str, args: str, desc: str, category: str = CATEGORY) -> None:
        name = name.strip()
        if not re.fullmatch(rf"{_IDENT}(?:\.{_IDENT})*", name):
            return
        if name in entries:
            return
        signature = f"{name}{args}"
        if desc:
            signature += f" -- {desc}"
        entries[name] = {
            "name": name,
            "signature": signature,
            "category": category,
            "is_call": True,
        }

    for index, raw in enumerate(lines):
        line = raw.rstrip()
        stripped = line.strip()
        if not stripped:
            continue

        heading = _HEADING_RE.match(line)
        if heading:
            in_globals = stripped.casefold().startswith("global variables")
            in_properties = False
            continue

        if in_globals:
            variable = _VARIABLE_RE.match(line)
            if variable:
                name, desc = variable.group(1), _description(variable.group(2), "")
                if re.fullmatch(_IDENT, name):
                    entries[name] = {
                        "name": name,
                        "signature": f"{name} -- {desc}" if desc else name,
                        "category": GLOBAL_CATEGORY,
                        "is_call": False,
                    }
            continue

        section = _SECTION_RE.match(line)
        if section:
            in_properties = section.group(1).casefold() == "properties"
            continue
        if in_properties:
            continue

        class_match = _CLASS_RE.match(line)
        if class_match:
            # Class headings introduce a fresh methods/properties block.
            in_properties = False
            continue

        following = lines[index + 1] if index + 1 < len(lines) else ""
        for variant in _variant_names(stripped):
            parsed = _signature(variant)
            if not parsed:
                continue
            name, args, rest = parsed
            add(name, args, _description(rest, following))

            # Keep the class-qualified spelling as well, e.g. "string.split".
            if name in {"split", "endsWith", "startsWith", "trim", "replace"}:
                add(f"string.{name}", args, _description(rest, following))

    return sorted(entries.values(), key=lambda item: str(item["name"]).casefold())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=None, help="path to CE's celua.txt")
    args = parser.parse_args()

    source = args.source or (Path(os.environ["CE_LUA_TXT"]) if os.environ.get("CE_LUA_TXT") else None)
    if source is None:
        source = next((Path(p) for p in _DEFAULT_SOURCES if Path(p).is_file()), None)
    if source is None or not source.is_file():
        parser.error("celua.txt not found; pass --source or set CE_LUA_TXT")

    entries = parse(source)
    OUT_JSON.write_text(
        json.dumps(entries, ensure_ascii=False, indent=1) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(f"wrote {OUT_JSON} ({len(entries)} entries from {source})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
