"""Tests for template expansion and symbol resolution."""

from __future__ import annotations

from ce_edit.templates import (
    CARET,
    DEFAULT_TEMPLATES,
    TemplateRule,
    alloc_name,
    auto_pair_blocks,
    completion_argument,
    insert_into_section,
    insert_sections,
    parse_templates,
    render,
    resolve_variable,
)

SOURCE = "[ENABLE]\nalloc(axcc，8)\n[DISABLE]\n"
RULE = TemplateRule(
    "模板函数1",
    "dealloc",
    "unregistersymbol($ver$)\ndealloc($ver$)",
    "DISABLE",
)


def test_render_replaces_every_placeholder() -> None:
    assert render(RULE, "axcc") == "unregistersymbol(axcc)\ndealloc(axcc)"
    custom = TemplateRule("x", "", "$ver$ / $name$", "ENABLE")
    assert render(custom, "value") == "value / value"


def test_alloc_name_accepts_fullwidth_comma() -> None:
    assert alloc_name("alloc(axcc，8)") == "axcc"
    assert alloc_name("  globalalloc(testVar, 0x100)") == "testVar"
    assert alloc_name("mov eax, 1") is None


def test_resolve_variable_prefers_clipboard_when_defined() -> None:
    assert resolve_variable(SOURCE, clipboard="axcc") == "axcc"
    assert resolve_variable(SOURCE, clipboard="unknown") == "axcc"
    assert resolve_variable(SOURCE, selection="axcc") == "axcc"
    assert resolve_variable(SOURCE, clipboard="axcc", allow_clipboard=False) == "axcc"
    assert resolve_variable("", clipboard="axcc") is None


def test_completion_argument_respects_function_and_flag() -> None:
    assert completion_argument("registersymbol", SOURCE, "axcc") == "axcc"
    assert completion_argument("print", SOURCE, "axcc") is None
    assert (
        completion_argument("registersymbol", SOURCE, "axcc", enabled=False) is None
    )


def test_auto_pair_blocks_and_insert() -> None:
    blocks = auto_pair_blocks(SOURCE, "axcc", DEFAULT_TEMPLATES)
    assert blocks["ENABLE"] == "registersymbol(axcc)"
    assert blocks["DISABLE"] == "unregistersymbol(axcc)\ndealloc(axcc)"
    updated = insert_sections(SOURCE, blocks)
    assert "registersymbol(axcc)" in updated
    assert "dealloc(axcc)" in updated
    assert "，" in updated  # full-width comma is preserved
    assert updated.index("[ENABLE]") < updated.index("registersymbol(axcc)")
    assert updated.index("[DISABLE]") < updated.index("unregistersymbol(axcc)")


def test_auto_pair_blocks_skip_existing_lines() -> None:
    complete = (
        "[ENABLE]\nalloc(axcc, 8)\nregistersymbol(axcc)\n"
        "[DISABLE]\nunregistersymbol(axcc)\ndealloc(axcc)\n"
    )
    assert auto_pair_blocks(complete, "axcc", DEFAULT_TEMPLATES) == {}


def test_pair_rules_follow_ce_semantics() -> None:
    # alloc pairs with dealloc; registersymbol pairs with unregistersymbol.
    blocks = auto_pair_blocks(SOURCE, "axcc", ())
    assert blocks["ENABLE"] == "registersymbol(axcc)"
    assert blocks["DISABLE"] == "unregistersymbol(axcc)\ndealloc(axcc)"
    only_register = "[ENABLE]\nregistersymbol(axcc)\n[DISABLE]\n"
    assert auto_pair_blocks(only_register, "axcc", ()) == {
        "DISABLE": "unregistersymbol(axcc)"
    }


def test_generated_lines_stay_inside_their_section() -> None:
    reversed_source = (
        "[DISABLE]\nunregistersymbol(other)\n[ENABLE]\nalloc(axcc, 8)\n"
    )
    result = insert_sections(
        reversed_source, auto_pair_blocks(reversed_source, "axcc", ())
    )
    lines = result.splitlines()
    disable_at = lines.index("[DISABLE]")
    enable_at = lines.index("[ENABLE]")
    assert disable_at < lines.index("unregistersymbol(axcc)") < enable_at
    assert disable_at < lines.index("dealloc(axcc)") < enable_at
    assert lines.index("registersymbol(axcc)") > enable_at


def test_missing_sections_are_created_in_the_right_order() -> None:
    source = "alloc(axcc, 8)\nmov [axcc], eax\n"
    result = insert_sections(source, auto_pair_blocks(source, "axcc", ()))
    lines = result.splitlines()
    assert lines[0] == "[ENABLE]"
    disable_at = lines.index("[DISABLE]")
    assert lines.index("registersymbol(axcc)") < disable_at
    assert lines.index("unregistersymbol(axcc)") > disable_at
    assert lines.index("dealloc(axcc)") > disable_at


def test_existing_line_in_wrong_section_does_not_suppress_generation() -> None:
    source = "[ENABLE]\nalloc(axcc, 8)\ndealloc(axcc)\n[DISABLE]\n"
    blocks = auto_pair_blocks(source, "axcc", ())
    assert "dealloc(axcc)" in blocks.get("DISABLE", "")


def test_misconfigured_template_still_uses_canonical_section() -> None:
    wrong = TemplateRule(
        "配错的释放", "dealloc", "dealloc($ver$)", "ENABLE", True
    )
    source = "[ENABLE]\nregistersymbol(axcc)\n[DISABLE]\n"
    blocks = auto_pair_blocks(source, "axcc", (wrong,))
    assert "dealloc(axcc)" in blocks.get("DISABLE", "")
    assert "dealloc" not in blocks.get("ENABLE", "")


def test_parse_templates_validates_payload() -> None:
    assert parse_templates(None) == DEFAULT_TEMPLATES
    rules = parse_templates(
        [{"name": "自定义", "body": "print($ver$)", "section": "bad", "auto": 1}]
    )
    assert len(rules) == 1
    # Arbitrary (custom) section names are preserved verbatim now.
    assert rules[0].section == "BAD"
    assert rules[0].auto is True
    # The CARET sentinel is accepted via several aliases.
    caret_rule = parse_templates(
        [{"name": "x", "body": "y($ver$)", "section": "光标位置"}]
    )[0]
    assert caret_rule.section == "CARET"
    assert parse_templates([{"name": "", "body": ""}]) == DEFAULT_TEMPLATES


def test_insert_into_section_creates_missing_block() -> None:
    source = "alloc(axcc, 8)\n"
    result = insert_into_section(source, "LUA", "print('hi')")
    assert "[LUA]" in result
    assert "print('hi')" in result
    assert result.index("[LUA]") > result.index("alloc")


def test_insert_into_section_appends_to_existing() -> None:
    source = "[LUA]\nfoo()\n[ENABLE]\nalloc(axcc, 8)\n[DISABLE]\n"
    result = insert_into_section(source, "LUA", "bar()")
    assert result.count("[LUA]") == 1
    lua_at = result.index("[LUA]")
    bar_at = result.index("bar()")
    enable_at = result.index("[ENABLE]")
    assert lua_at < bar_at < enable_at


def test_insert_into_section_ignores_caret_and_empty() -> None:
    source = "alloc(axcc, 8)\n"
    assert insert_into_section(source, "CARET", "x()") == source
    assert insert_into_section(source, "", "") == source


def test_insert_sections_handles_custom_missing_block() -> None:
    source = "alloc(axcc, 8)\n"
    result = insert_sections(source, {"LUA": "custom(axcc)"})
    assert "[LUA]" in result
    assert "custom(axcc)" in result


def test_insert_sections_wraps_ce_blocks_when_no_markers() -> None:
    source = "alloc(axcc, 8)\n"
    result = insert_sections(source, {"ENABLE": "registersymbol(axcc)"})
    lines = result.splitlines()
    assert lines[0] == "[ENABLE]"
    assert "registersymbol(axcc)" in result
    assert result.index("[DISABLE]") > result.index("registersymbol(axcc)")
