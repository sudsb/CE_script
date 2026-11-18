"""Tests for Cheat Engine XML import and export."""

from __future__ import annotations

from ce_edit.ce_xml import (
    build_ce_xml,
    esc_xml,
    extract_ce_code,
    has_script,
    is_cheat_table,
    unesc_xml,
)

AA_XML = (
    '<?xml version="1.0" encoding="utf-8"?>\n'
    "<CheatTable><CheatEntries><CheatEntry>"
    '<Description>"demo"</Description>'
    "<AssemblerScript>[ENABLE]\nmov eax, 1\n[DISABLE]\n</AssemblerScript>"
    "</CheatEntry></CheatEntries></CheatTable>"
)

LUA_XML = (
    "<CheatTable><CheatEntries><CheatEntry>"
    "<LuaScript>print('hi')</LuaScript>"
    "</CheatEntry></CheatEntries></CheatTable>"
)

ADDRESS_XML = (
    "<CheatTable><CheatEntries><CheatEntry>"
    "<Address>game.exe+1234</Address>"
    "<Offsets><Offset>10</Offset><Offset>20</Offset></Offsets>"
    "</CheatEntry></CheatEntries></CheatTable>"
)


def test_escape_roundtrip() -> None:
    raw = '<tag attr="&">'
    escaped = esc_xml(raw)
    assert "<" not in escaped
    assert unesc_xml(escaped) == raw


def test_is_cheat_table() -> None:
    assert is_cheat_table(AA_XML) is True
    assert is_cheat_table("print(1)") is False


def test_extract_assembler_script() -> None:
    assert extract_ce_code(AA_XML) == "[ENABLE]\nmov eax, 1\n[DISABLE]"


def test_extract_lua_script() -> None:
    assert extract_ce_code(LUA_XML) == "print('hi')"


def test_extract_address_with_offsets() -> None:
    assert extract_ce_code(ADDRESS_XML) == "game.exe+1234\n10\n20"


def test_extract_missing_script() -> None:
    assert extract_ce_code("<CheatTable></CheatTable>") is None


def test_unescapes_entities() -> None:
    xml = (
        "<CheatTable><CheatEntries><CheatEntry>"
        "<AssemblerScript>mov eax, &lt;1&gt; &amp; 2</AssemblerScript>"
        "</CheatEntry></CheatEntries></CheatTable>"
    )
    assert extract_ce_code(xml) == "mov eax, <1> & 2"


def test_has_script() -> None:
    assert has_script(AA_XML) is True
    assert has_script(LUA_XML) is True
    assert has_script("<CheatTable></CheatTable>") is False


def test_build_empty_returns_none() -> None:
    assert build_ce_xml("") is None
    assert build_ce_xml("  \n") is None


def test_build_aa_keeps_existing_marks() -> None:
    xml = build_ce_xml("[ENABLE]\nmov eax, 1\n")
    assert xml is not None
    assert "<AssemblerScript>[ENABLE]\nmov eax, 1\n</AssemblerScript>" in xml
    assert "<Description>\"CE脚本\"</Description>" in xml


def test_build_aa_adds_marks() -> None:
    xml = build_ce_xml("mov eax, 1")
    assert xml is not None
    assert "[ENABLE]\nmov eax, 1\n[DISABLE]" in xml


def test_build_lua_script() -> None:
    xml = build_ce_xml("-- desc\nlocal x = 1\nprint(x)\n")
    assert xml is not None
    assert "<LuaScript>" in xml
    assert "<Description>\"desc\"</Description>" in xml


def test_build_memory_record_with_offsets() -> None:
    xml = build_ce_xml('-- HP\n"game.exe"+1A2B3C\n+10\n20\n')
    assert xml is not None
    assert "<VariableType>4 Bytes</VariableType>" in xml
    assert '<Address>"game.exe"+1A2B3C</Address>' in xml
    assert "<Offset>10</Offset>" in xml
    assert "<Offset>20</Offset>" in xml
    assert "<Description>\"HP\"</Description>" in xml


def test_build_memory_record_without_offsets() -> None:
    xml = build_ce_xml("0x123456")
    assert xml is not None
    assert "<Address>0x123456</Address>" in xml
    assert "<Offsets>" not in xml


def test_build_xml_escapes_script_body() -> None:
    xml = build_ce_xml('[ENABLE]\nmov eax, [edx]\ncmp eax, 0x10\n')
    assert xml is not None
    assert "mov eax, [edx]" in xml
    assert "<AssemblerScript>" in xml
