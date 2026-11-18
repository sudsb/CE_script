"""Tests for spacing, indentation and comment formatting."""

from __future__ import annotations

import pytest

from ce_edit.formatter import (
    auto_indent,
    comment_mark,
    format_aa_line,
    format_document,
    format_line,
    format_xml_doc,
    toggle_comment_lines,
)


@pytest.mark.parametrize(
    ("src", "expected"),
    [
        ("local x=1+2*3", "local x = 1 + 2 * 3"),
        ('print( "a".."b" )', 'print("a" .. "b")'),
        ("if(x==y)then", "if(x == y)then"),
        ("local t={1,2,3}", "local t = {1, 2, 3}"),
        ("foo ( a,b )", "foo (a, b)"),
        ("x = - 1", "x = - 1"),
        ("y=#t", "y = #t"),
        ("s='a' --tail", "s = 'a' --tail"),
        ("local s = 'a\\nb'", "local s = 'a\\nb'"),
        ("x = [[raw text]]", "x = [[raw text]]"),
    ],
)
def test_format_line(src: str, expected: str) -> None:
    assert format_line(src) == expected


@pytest.mark.parametrize(
    ("src", "expected"),
    [
        ("mov eax, ebx // trailing", "mov eax,ebx // trailing"),
        ("  mov eax, [eax+10]", "  mov eax,[eax+10]"),
        ("dd 1,2,3", "dd 1,2,3"),
        ("label(return)", "label(return)"),
        ("aobscanmodule(A, game.exe, 48 8B ?? ?? ?? ??)", "aobscanmodule(A,game.exe,48 8B ?? ?? ?? ??)"),
    ],
)
def test_format_aa_line(src: str, expected: str) -> None:
    assert format_aa_line(src) == expected


@pytest.mark.parametrize(
    ("line", "expected"),
    [
        ("if x then", "  "),
        ("for i=1,3 do", "  "),
        ("while x do", "  "),
        ("repeat", "  "),
        ("function f()", "  "),
        ("local t = {", "  "),
        ("else", "  "),
        ("elseif y then", "  "),
        ("end", ""),
        ("until x", ""),
        ("local x=1", ""),
        ("-- c", ""),
        ("  local x = 1", "  "),
    ],
)
def test_auto_indent(line: str, expected: str) -> None:
    assert auto_indent(line) == expected


def test_comment_mark() -> None:
    assert comment_mark("lua") == "--"
    assert comment_mark("aa") == "//"
    assert comment_mark("c") == "//"


def test_toggle_comment_lines_comments_and_restores() -> None:
    src = ["local a = 1", "", "print(a)"]
    commented = toggle_comment_lines(src, "lua")
    assert commented == ["-- local a = 1", "", "-- print(a)"]
    assert toggle_comment_lines(commented, "lua") == src


def test_toggle_comment_lines_mixed_selection_keeps_existing() -> None:
    src = ["mov eax,1", "// x", ""]
    assert toggle_comment_lines(src, "aa") == ["// mov eax,1", "// x", ""]
    assert toggle_comment_lines(["// mov eax,1", "// x", ""], "aa") == [
        "mov eax,1",
        "x",
        "",
    ]


def test_toggle_comment_lines_indentation() -> None:
    assert toggle_comment_lines(["  -- indented", "  b"], "lua") == [
        "  -- indented",
        "  -- b",
    ]


def test_toggle_comment_lines_blank_block() -> None:
    assert toggle_comment_lines(["", "  "], "lua") == ["", "  "]


def test_format_document_lua() -> None:
    src = (
        "local x=1\n"
        "if x>0 then\n"
        "print( 'hi' )\n"
        "for i=1,10 do\n"
        "x=x+i\n"
        "end\n"
        "end\n"
    )
    expected = (
        "local x = 1\n"
        "if x > 0 then\n"
        "  print('hi')\n"
        "  for i = 1, 10 do\n"
        "    x = x + i\n"
        "  end\n"
        "end\n"
    )
    assert format_document(src) == expected


def test_format_document_aa_keeps_instruction_indentation() -> None:
    src = "[ENABLE]\nmov eax, ebx\n  jmp return\n"
    assert format_document(src) == "[ENABLE]\nmov eax,ebx\n  jmp return\n"


def test_format_document_preserves_string_bodies() -> None:
    src = 'local s = [[\nif x then\n  print(1)\nend\n]]\n'
    assert format_document(src) == src


def test_format_document_empty() -> None:
    assert format_document("") == ""
    assert format_document("   \n\n") == "   \n\n"


def test_format_xml_doc_indents_and_preserves_script() -> None:
    src = (
        '<CheatTable><CheatEntries><CheatEntry><ID>1</ID>'
        "<AssemblerScript>[ENABLE]\nmov eax, 1\n</AssemblerScript>"
        "</CheatEntry></CheatEntries></CheatTable>"
    )
    result = format_document(src)
    lines = result.splitlines()
    assert lines[0] == "<CheatTable>"
    assert lines[1] == "  <CheatEntries>"
    assert "    <CheatEntry>" in lines
    assert "      <AssemblerScript>[ENABLE]" in lines
    assert "mov eax, 1" in lines
    assert "      </AssemblerScript>" in lines


def test_format_xml_doc_collapses_tag_whitespace() -> None:
    assert format_xml_doc("<a >\n  <b />\n</a >") == "<a>\n  <b/>\n</a>"


def test_format_document_detects_single_line_xml() -> None:
    src = "<CheatTable><CheatEntries></CheatEntries></CheatTable>"
    assert format_document(src).startswith("<CheatTable>\n  <CheatEntries>")
