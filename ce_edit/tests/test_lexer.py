"""Tests for the mixed Lua / Auto Assembler / C tokenizer."""

from __future__ import annotations

import pytest

from ce_edit.lexer import (
    compute_line_starts,
    detect_mode,
    is_in_string_or_comment,
    looks_aa,
    mode_at_offset,
    tokenize,
)


def kinds(src: str) -> list[str | None]:
    return [token.kind for token in tokenize(src).tokens]


def texts(src: str) -> list[str]:
    return [src[token.start : token.end] for token in tokenize(src).tokens]


def test_lua_basic_tokens() -> None:
    src = "local x = 1 -- hi"
    assert kinds(src) == ["kwd", None, "op", "num", "com"]
    assert texts(src) == ["local", "x", "=", "1", "-- hi"]


def test_lua_control_flow_and_builtin() -> None:
    src = 'if x >= 2 then print("ok") end'
    assert kinds(src) == [
        "kw",
        None,
        "op",
        "num",
        "kw",
        "builtin",
        "br0",
        "str",
        "br0",
        "kw",
    ]


def test_lua_function_and_library_tokens() -> None:
    src = "function foo(a)\n  local s = string.format('%d', a)\n  return s\nend"
    assert kinds(src) == [
        "kwd",
        "fn",
        "br0",
        None,
        "br0",
        "kwd",
        None,
        "op",
        "lib",
        "punct",
        "builtin",
        "br0",
        "str",
        "punct",
        None,
        "br0",
        "kw",
        None,
        "kw",
    ]


def test_aa_basic_tokens() -> None:
    src = (
        "[ENABLE]\n"
        "alloc(newmem, 0x1000)\n"
        "newmem:\n"
        "  mov eax, [eax+10]\n"
        "  jmp return\n"
    )
    assert kinds(src) == [
        "aamark",
        "aacmd",
        "aabr",
        "aasym",
        "punct",
        "num",
        "aabr",
        "asmlab",
        "punct",
        "mne",
        "reg",
        "punct",
        "aabr",
        "reg",
        "op",
        "num",
        "aabr",
        "jmp",
        "aasym",
    ]
    assert tokenize(src).line_modes == ["aa"] * 6


def test_aa_aobscan_wildcards_and_module() -> None:
    src = "aobscanmodule(INJECT, game.exe, 48 8B 05 ?? ?? ?? ??)\n"
    assert kinds(src) == [
        "aacmd",
        "aabr",
        "aasym",
        "punct",
        "mod",
        "punct",
        "num",
        "num",
        "num",
        "wild",
        "wild",
        "wild",
        "wild",
        "aabr",
    ]


def test_aa_comment_styles() -> None:
    assert kinds("{$asm}\nmov eax,1 // note\n") == [
        "aadir",
        "mne",
        "reg",
        "punct",
        "num",
        "com",
    ]
    assert kinds("{$asm}\nmov eax,1 -- note\n") == [
        "aadir",
        "mne",
        "reg",
        "punct",
        "num",
        "com",
    ]
    assert kinds("{$asm}\n{ block }\n") == ["aadir", "com"]


def test_c_snippet() -> None:
    src = "{$c}\nint main() { return 0; }"
    assert tokenize(src).line_modes[0] == "c"
    assert kinds(src) == [
        "aadir",
        "size",
        "call",
        "aabr",
        "aabr",
        "aabr",
        "kw",
        "num",
        "punct",
        "aabr",
    ]


def test_embedded_modes() -> None:
    src = "{$lua}\nprint(1)\n{$asm}\nmov eax, 1\n"
    result = tokenize(src)
    assert result.line_modes == ["lua", "lua", "aa", "aa", "aa"]
    assert "builtin" in [token.kind for token in result.tokens]
    assert "mne" in [token.kind for token in result.tokens]


def test_directives_switch_modes() -> None:
    assert tokenize("{$asm}\nmov eax,1\n{$lua}\nprint(1)\n").line_modes == [
        "aa",
        "aa",
        "lua",
        "lua",
        "lua",
    ]


def test_long_strings_and_comments() -> None:
    src = "s = [[long]]\nt = [[\nbody\n]]\n--[[ block ]]\n--[==[ odd ]==]\n"
    assert "str" in kinds(src)
    assert kinds(src).count("com") == 2
    assert tokenize(src).line_modes == ["lua"] * 7


def test_quoted_string_escapes() -> None:
    src = 'x = "a\\nb\\x41"'
    result = tokenize(src)
    assert [token.kind for token in result.tokens] == [
        None,
        "op",
        "str",
        "esc",
        "str",
        "esc",
        "str",
    ]


def test_assembly_inside_lua_long_string() -> None:
    src = (
        "local script = [[\n"
        "[ENABLE]\n"
        "alloc(newmem)\n"
        "mov eax, 1\n"
        "]]\n"
        "print(script)\n"
    )
    result = tokenize(src)
    assert result.line_modes[:4] == ["lua", "aa", "aa", "aa"]
    assert result.line_modes[-1] == "lua"
    assert "aamark" in [token.kind for token in result.tokens]
    assert "mne" in [token.kind for token in result.tokens]


def test_bracket_pairs() -> None:
    src = 'print("x")'
    result = tokenize(src)
    opening = src.index("(")
    closing = src.index(")")
    assert result.pairs[opening] == closing
    assert result.pairs[closing] == opening


def test_label_and_vararg() -> None:
    assert kinds("::start::\ngoto start\n") == ["lbl", "kw", None]
    assert kinds("function f(...)\nend\n") == [
        "kwd",
        "fn",
        "br0",
        "vararg",
        "br0",
        "kw",
    ]


@pytest.mark.parametrize(
    ("src", "message"),
    [
        ("f(1", '括号未闭合：缺少 ")"'),
        ("f(1]", '括号不匹配：多余的 "]"'),
        ("local x = (1 + [2}", '括号不匹配：多余的 "}"'),
    ],
)
def test_bracket_errors(src: str, message: str) -> None:
    result = tokenize(src)
    assert result.ok is False
    assert result.message == message
    assert "brerr" in [token.kind for token in result.tokens]


def test_unterminated_strings_are_not_lint_errors() -> None:
    assert tokenize('x = "abc\n').ok is True
    assert tokenize("x = [[abc\n").ok is True
    assert tokenize("--[[ abc\n").ok is True


@pytest.mark.parametrize(
    ("src", "mode"),
    [
        ("", "lua"),
        ("print(1)\n", "lua"),
        ("local x = 1\n", "lua"),
        ("[ENABLE]\nmov eax,1\n", "aa"),
        ("[DISABLE]\n", "aa"),
        ("// comment first\nexit(1)\n", "aa"),
        ("{$c}\nint main() {}\n", "aa"),
        ("aobscanmodule(A, game.exe, 00)\n", "aa"),
        ("-- comment\n-- another\nlocal x = 1\n", "lua"),
    ],
)
def test_detect_mode(src: str, mode: str) -> None:
    assert detect_mode(src) == mode


@pytest.mark.parametrize(
    ("body", "expected"),
    [
        ("[ENABLE]\nmov eax,1", True),
        ("alloc(newmem)", True),
        ("mov eax, 1", True),
        ("local x = 1", False),
        ("hello world", False),
    ],
)
def test_looks_aa(body: str, expected: bool) -> None:
    assert looks_aa(body) is expected


def test_offsets_and_modes() -> None:
    src = "{$lua}\nlocal x = 1\n{$asm}\nmov eax, 1\n"
    result = tokenize(src)
    assert compute_line_starts(src) == [0, 7, 19, 26, 37]
    assert mode_at_offset(result, 0) == "lua"
    assert mode_at_offset(result, 10) == "lua"
    assert mode_at_offset(result, len(src) - 2) == "aa"


def test_string_and_comment_offsets() -> None:
    src = 'x = "abc" -- tail\n'
    result = tokenize(src)
    assert is_in_string_or_comment(result.tokens, src.index("abc")) is True
    assert is_in_string_or_comment(result.tokens, src.index("tail")) is True
    assert is_in_string_or_comment(result.tokens, 0) is False
