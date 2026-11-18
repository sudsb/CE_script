"""Tests for document analysis and the strengthened syntax checker."""

from __future__ import annotations

from ce_edit.analysis import (
    DEFAULT_CHECK_OPTIONS,
    active_checks,
    caret_word,
    check_syntax,
    defined_variables,
    definitions,
    document_keywords,
    section_at,
    sections,
    word_bounds,
)
from ce_edit.app import WELCOME

ALLOC_DOC = "[ENABLE]\nalloc(axcc，8)\nregistersymbol(axcc)\n[DISABLE]\nunregistersymbol(axcc)\ndealloc(axcc)\n"


def test_word_bounds() -> None:
    assert word_bounds("alloc(axcc, 8)", 7) == (6, 10)
    assert word_bounds("alloc(axcc, 8)", 5) is None
    assert word_bounds("$symbol", 2) == (0, 7)
    assert word_bounds("", 0) is None


def test_sections_and_section_at() -> None:
    found = sections(ALLOC_DOC)
    assert [section.name for section in found] == ["ENABLE", "DISABLE"]
    assert section_at(ALLOC_DOC, found[0].body_start + 2) is not None
    assert section_at(ALLOC_DOC, found[1].body_start + 2).name == "DISABLE"


def test_definitions_support_fullwidth_comma() -> None:
    names = [item.name for item in definitions(ALLOC_DOC)]
    assert "axcc" in names
    assert "alloc" not in names
    assert defined_variables(ALLOC_DOC)[0] == "axcc"


def test_document_keywords_never_split_one_word() -> None:
    source = (
        "[ENABLE]\n"
        "aobscanmodule(INJECT, game.exe, 48 8B 05 ?? ?? ?? ??)\n"
        "alloc(newmem, 0x1000)\n"
        "label(return)\n"
        "registersymbol(INJECT)\n"
        "newmem:\n"
        "  mov [player_hp_base], eax\n"
        "  db 'http://example.com'\n"
        "[DISABLE]\n"
    )
    keywords = document_keywords(source, source.index("newmem:\n  mov") + 10)
    for fragment in ("game", "exe", "x1000", "http", "example", "com"):
        assert fragment not in keywords
    assert {"INJECT", "newmem", "player_hp_base"} <= set(keywords)
    assert all("." not in word for word in keywords)


def test_dotted_names_are_not_split_into_fragments() -> None:
    source = (
        "[ENABLE]\n"
        "aobscanmodule(INJECT, game.exe, 48 8B 05)\n"
        "control.Max = 100\n"
        "registersymbol(INJECT)\n"
        "[DISABLE]\n"
    )
    keywords = document_keywords(source, 20)
    assert "game" not in keywords
    assert "exe" not in keywords
    assert "control" not in keywords
    assert "Max" not in keywords
    assert "8B" not in keywords
    assert "INJECT" in keywords


def test_document_keywords_prefer_current_scope() -> None:
    source = "[ENABLE]\nalloc(alphaVar, 8)\n[DISABLE]\nlocal betaVar = 1\nbetaVar"
    keywords = document_keywords(source, source.index("betaVar") + 2)
    assert "betaVar" in keywords
    assert "alphaVar" in keywords
    assert "alloc" not in keywords


def test_half_typed_word_is_not_offered_as_a_keyword() -> None:
    source = "[ENABLE]\nnewmem:\nmov [player_hp], eax\njmp return\nnewm"
    offset = len(source) - 2
    assert caret_word(source, offset) == "newm"
    keywords = document_keywords(source, offset, "aa")
    assert "newm" not in keywords
    assert "ne" not in keywords
    assert "newmem" in keywords
    assert "mov" not in keywords and "eax" not in keywords
    assert "jmp" not in keywords and "return" not in keywords


def test_ce_keywords_in_the_document_are_not_reoffered() -> None:
    source = (
        "[ENABLE]\n"
        "alloc(newmem, 0x100)\n"
        "registersymbol(newmem)\n"
        "mov eax, [rax+0x10]\n"
        "dd 0\n"
        "[DISABLE]\n"
    )
    keywords = set(document_keywords(source, 20, "aa"))
    assert keywords.isdisjoint(
        {"alloc", "registersymbol", "mov", "eax", "rax", "dd", "enable", "disable"}
    )
    assert "newmem" in keywords


def test_linter_reports_bare_aa_statements_and_counts_checks() -> None:
    report = check_syntax("[ENABLE]\nne\n[DISABLE]\n")
    assert any(item.code == "aa_statements" for item in report.diagnostics)
    assert active_checks() == len(DEFAULT_CHECK_OPTIONS)
    assert active_checks({"pairs": False}) == len(DEFAULT_CHECK_OPTIONS) - 1


def test_linter_reports_aa_bracket_and_duplicate_allocation() -> None:
    report = check_syntax(
        "[ENABLE]\nalloc(newmem, 8)\nalloc(newmem, 8)\nmov eax, [rax+0x10\n[DISABLE]\n"
    )
    codes = {item.code for item in report.diagnostics}
    assert "duplicates" in codes
    assert "brackets" in codes


def test_syntax_ok_for_welcome_document() -> None:
    report = check_syntax(WELCOME)
    assert report.ok
    assert report.message == ""
    assert report.lines() == []


def test_syntax_finds_missing_lua_end() -> None:
    report = check_syntax("local x = 1\nif x > 0 then\n  print(x)\n")
    assert not report.ok
    assert any(item.code == "lua_blocks" for item in report.warnings)
    assert "end" in report.message


def test_syntax_finds_unclosed_string_and_bracket() -> None:
    report = check_syntax('print("abc\n')
    assert report.errors
    codes = {item.code for item in report.errors}
    assert "strings" in codes
    assert "brackets" in codes
    assert report.message.startswith("第")


def test_syntax_reports_section_and_pair_problems() -> None:
    report = check_syntax("[ENABLE]\nalloc(testVar, 8)\n")
    codes = {item.code for item in report.diagnostics}
    assert "sections" in codes
    assert "pairs" in codes
    assert any("dealloc(testVar)" in item.message for item in report.diagnostics)


def test_syntax_options_disable_checks() -> None:
    source = "[ENABLE]\nalloc(testVar, 8)\n"
    disabled = {key: False for key in DEFAULT_CHECK_OPTIONS}
    assert check_syntax(source, options=disabled).ok
    assert check_syntax(source, options={"pairs": False}).message != ""


def test_syntax_ignores_aa_lines_for_lua_blocks() -> None:
    source = "[ENABLE]\nmov eax, 1\n[DISABLE]\nret\n"
    report = check_syntax(source)
    assert not any(item.code == "lua_blocks" for item in report.diagnostics)
