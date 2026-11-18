"""Convert between CE scripts and the XML format used by Cheat Engine."""

from __future__ import annotations

import re

ASSEMBLER_RE = re.compile(
    r"<AssemblerScript>([\s\S]*?)</AssemblerScript>", re.IGNORECASE
)
LUA_RE = re.compile(r"<LuaScript>([\s\S]*?)</LuaScript>", re.IGNORECASE)
ADDRESS_RE = re.compile(r"<Address>([\s\S]*?)</Address>", re.IGNORECASE)
OFFSET_RE = re.compile(r"<Offset>([\s\S]*?)</Offset>", re.IGNORECASE)
CHEAT_TABLE_RE = re.compile(r"<CheatTable\b", re.IGNORECASE)
DESCRIPTION_RE = re.compile(r"^\s*(?:--|//)\s*(.+)$", re.MULTILINE)
COMMENT_RE = re.compile(r"^\s*(?:--|//)\s*(.+)$")
AA_HINT_RE = re.compile(
    r"\[ENABLE\]|\[DISABLE\]|"
    r"^\s*(?:alloc|globalalloc|label|registersymbol|aobscan\w*|define|"
    r"dealloc|fullaccess|assert|newmem|createThread)\s*\(",
    re.IGNORECASE | re.MULTILINE,
)
ASM_HINT_RE = re.compile(
    r"^\s*(?:mov|jmp|call|push|pop|lea|cmp|add|sub|nop|ret|xor|db|dd|"
    r"dw|je|jne|jz|jnz|jb|ja|jg|jl|jge|jle|test|shr|shl|inc|dec)\b",
    re.IGNORECASE | re.MULTILINE,
)
LUA_HINT_RE = re.compile(
    r"^\s*(?:function|local|if|for|while|do|end|then|require)\b",
    re.MULTILINE,
)
ADDRESS_LINE_RE = re.compile(
    r'^(?:"[^"]+"\s*\+?\s*[0-9A-Fa-fx]+|0x[0-9A-Fa-f]+|[0-9A-Fa-f]{6,})$',
    re.IGNORECASE,
)
OFFSET_LINE_RE = re.compile(r"^(\+?)([0-9A-Fa-fx]+)$", re.IGNORECASE)


def esc_xml(text: str) -> str:
    """Escape text for an XML text node."""
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )


def unesc_xml(text: str) -> str:
    """Undo the XML entities that CE commonly writes."""
    return (
        text.replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", '"')
        .replace("&apos;", "'")
        .replace("&amp;", "&")
    )


def is_cheat_table(text: str) -> bool:
    """Return whether ``text`` looks like a CE cheat-table XML document."""
    return bool(CHEAT_TABLE_RE.search(text))


def extract_ce_code(xml: str) -> str | None:
    """Extract the editable script from a CE cheat-table XML string."""
    match = ASSEMBLER_RE.search(xml)
    if match:
        return unesc_xml(match.group(1)).strip("\n")
    match = LUA_RE.search(xml)
    if match:
        return unesc_xml(match.group(1)).strip("\n")
    match = ADDRESS_RE.search(xml)
    if match:
        address = unesc_xml(match.group(1)).strip()
        offsets = [
            item.strip()
            for item in OFFSET_RE.findall(xml)
            if item.strip()
        ]
        if offsets:
            return "\n".join([address, *offsets])
        return address
    return None


def _offsets(lines: list[str]) -> list[int]:
    values: list[int] = []
    for line in lines:
        match = OFFSET_LINE_RE.match(line)
        if match is None:
            continue
        raw = match.group(2)
        try:
            value = int(raw, 16) if "x" in raw.casefold() else int(raw, 10)
        except ValueError:
            continue
        values.append(value)
    return values


def _memory_record(lines: list[str]) -> tuple[str, list[int]] | None:
    if not lines:
        return None
    if not all(
        ADDRESS_LINE_RE.match(line) or OFFSET_LINE_RE.match(line)
        for line in lines
    ):
        return None
    return lines[0], _offsets(lines[1:])


def _description(src: str) -> str:
    match = DESCRIPTION_RE.search(src)
    if match is None:
        return "CE脚本"
    return match.group(1).strip().strip("\"'")


def _wrap(description: str, body: str) -> str:
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<CheatTable>\n<CheatEntries>\n<CheatEntry>\n"
        "<ID>1</ID>\n"
        f'<Description>"{esc_xml(description)}"</Description>\n'
        f"{body}"
        "</CheatEntry>\n</CheatEntries>\n</CheatTable>"
    )


def build_ce_xml(src: str) -> str | None:
    """Build a CE cheat-table XML document from the current editor text."""
    if not src.strip():
        return None
    description = _description(src)
    lines = [line.strip() for line in src.splitlines() if line.strip()]
    payload = [
        line for line in lines if not line.startswith(("--", "//"))
    ]
    memory = _memory_record(payload)
    if memory is not None:
        base, offsets = memory
        body = (
            "<VariableType>4 Bytes</VariableType>\n"
            f"<Address>{esc_xml(base)}</Address>\n"
        )
        if offsets:
            body += "<Offsets>\n"
            body += "".join(
                f"<Offset>{offset}</Offset>\n" for offset in offsets
            )
            body += "</Offsets>\n"
        return _wrap(description, body)
    if (
        not AA_HINT_RE.search(src)
        and not ASM_HINT_RE.search(src)
        and LUA_HINT_RE.search(src)
    ):
        body = f"<LuaScript>{esc_xml(src)}</LuaScript>\n"
        return _wrap(description, body)
    body = src
    if not re.search(r"\[ENABLE\]", body, re.IGNORECASE):
        body = "[ENABLE]\n" + body.rstrip() + "\n[DISABLE]\n"
    body = f"<AssemblerScript>{esc_xml(body)}</AssemblerScript>\n"
    return _wrap(description, body)


def has_script(xml: str) -> bool:
    """Return whether the XML contains an editable AA or Lua script."""
    return bool(ASSEMBLER_RE.search(xml) or LUA_RE.search(xml))
