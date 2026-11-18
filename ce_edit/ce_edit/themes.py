"""Colour themes for the editor and its companion widgets."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, fields, replace
from typing import Any


@dataclass(frozen=True, slots=True)
class TokenStyle:
    """Presentation for one syntax token kind."""

    fg: str
    bold: bool = False
    italic: bool = False
    underline: bool = False
    bg: str | None = None


@dataclass(frozen=True, slots=True)
class Theme:
    """All colours used by one light or dark theme."""

    key: str
    name: str
    bg: str
    panel: str
    surface: str
    border: str
    text: str
    muted: str
    accent: str
    accent_soft: str
    caret: str
    current_line: str
    match_bg: str
    sel_bg: str
    same_word_bg: str
    gutter_bg: str
    gutter_text: str
    gutter_current: str
    find_bg: str
    find_current: str
    status_ok: str
    status_error: str
    shadow: str
    tokens: Mapping[str, TokenStyle]


def _light_tokens() -> dict[str, TokenStyle]:
    return {
        "com": TokenStyle("#6e7781", italic=True),
        "comdoc": TokenStyle("#4d7a55", italic=True),
        "kw": TokenStyle("#cf222e", bold=True),
        "kwd": TokenStyle("#cf222e", bold=True),
        "kwop": TokenStyle("#cf222e"),
        "const": TokenStyle("#0550ae", bold=True),
        "self": TokenStyle("#0550ae", italic=True),
        "num": TokenStyle("#0550ae"),
        "str": TokenStyle("#0a3069"),
        "strd": TokenStyle("#0a3069", bold=True),
        "esc": TokenStyle("#bc4c00", bold=True),
        "fn": TokenStyle("#8250df", bold=True),
        "call": TokenStyle("#8250df"),
        "method": TokenStyle("#8250df", italic=True),
        "builtin": TokenStyle("#1f883d"),
        "lib": TokenStyle("#953800"),
        "prop": TokenStyle("#953800"),
        "op": TokenStyle("#0550ae"),
        "punct": TokenStyle("#57606a"),
        "lbl": TokenStyle("#bc4c00", bold=True),
        "vararg": TokenStyle("#cf222e", bold=True),
        "br0": TokenStyle("#0550ae"),
        "br1": TokenStyle("#8250df"),
        "br2": TokenStyle("#1f883d"),
        "brerr": TokenStyle("#cf222e", underline=True),
        "aamark": TokenStyle("#8250df", bold=True, bg="#efe6ff"),
        "aadir": TokenStyle("#cf222e", bold=True, bg="#ffe9e9"),
        "aacmd": TokenStyle("#1f883d", bold=True),
        "mne": TokenStyle("#0969da", bold=True),
        "jmp": TokenStyle("#bc4c00", bold=True),
        "asmdir": TokenStyle("#bc4c00", bold=True),
        "reg": TokenStyle("#cf222e"),
        "size": TokenStyle("#8250df", italic=True),
        "mod": TokenStyle("#953800", underline=True),
        "wild": TokenStyle("#6e7781", bold=True),
        "asmlab": TokenStyle("#1f883d", bold=True),
        "aasym": TokenStyle("#24292f"),
        "aabr": TokenStyle("#57606a"),
    }


def _dark_tokens() -> dict[str, TokenStyle]:
    return {
        "com": TokenStyle("#8b949e", italic=True),
        "comdoc": TokenStyle("#8ec07c", italic=True),
        "kw": TokenStyle("#ff7b72", bold=True),
        "kwd": TokenStyle("#ff7b72", bold=True),
        "kwop": TokenStyle("#ff7b72"),
        "const": TokenStyle("#79c0ff", bold=True),
        "self": TokenStyle("#79c0ff", italic=True),
        "num": TokenStyle("#79c0ff"),
        "str": TokenStyle("#a5d6ff"),
        "strd": TokenStyle("#a5d6ff", bold=True),
        "esc": TokenStyle("#ffa657", bold=True),
        "fn": TokenStyle("#d2a8ff", bold=True),
        "call": TokenStyle("#d2a8ff"),
        "method": TokenStyle("#d2a8ff", italic=True),
        "builtin": TokenStyle("#7ee787"),
        "lib": TokenStyle("#ffa657"),
        "prop": TokenStyle("#ffa657"),
        "op": TokenStyle("#79c0ff"),
        "punct": TokenStyle("#8b949e"),
        "lbl": TokenStyle("#ffa657", bold=True),
        "vararg": TokenStyle("#ff7b72", bold=True),
        "br0": TokenStyle("#79c0ff"),
        "br1": TokenStyle("#d2a8ff"),
        "br2": TokenStyle("#7ee787"),
        "brerr": TokenStyle("#ff7b72", underline=True),
        "aamark": TokenStyle("#d2a8ff", bold=True, bg="#2a1a3f"),
        "aadir": TokenStyle("#ff7b72", bold=True, bg="#3a1a1a"),
        "aacmd": TokenStyle("#7ee787", bold=True),
        "mne": TokenStyle("#79c0ff", bold=True),
        "jmp": TokenStyle("#ffa657", bold=True),
        "asmdir": TokenStyle("#ffa657", bold=True),
        "reg": TokenStyle("#ff7b72"),
        "size": TokenStyle("#d2a8ff", italic=True),
        "mod": TokenStyle("#ffa657", underline=True),
        "wild": TokenStyle("#8b949e", bold=True),
        "asmlab": TokenStyle("#7ee787", bold=True),
        "aasym": TokenStyle("#c9d1d9"),
        "aabr": TokenStyle("#8b949e"),
    }


LIGHT = Theme(
    key="light",
    name="浅色",
    bg="#ffffff",
    panel="#f6f8fa",
    surface="#ffffff",
    border="#d8dee4",
    text="#24292f",
    muted="#8c959f",
    accent="#0969da",
    accent_soft="#ddf4ff",
    caret="#0969da",
    current_line="#f2f8fe",
    match_bg="#cfe4fb",
    sel_bg="#b6dcfb",
    same_word_bg="#ffe8a3",
    gutter_bg="#f6f8fa",
    gutter_text="#8c959f",
    gutter_current="#0969da",
    find_bg="#fff0b3",
    find_current="#ffd33d",
    status_ok="#1a7f37",
    status_error="#cf222e",
    shadow="#000000",
    tokens=_light_tokens(),
)

DARK = Theme(
    key="dark",
    name="深色",
    bg="#0d1117",
    panel="#161b22",
    surface="#161b22",
    border="#30363d",
    text="#c9d1d9",
    muted="#8b949e",
    accent="#58a6ff",
    accent_soft="#193153",
    caret="#58a6ff",
    current_line="#111a27",
    match_bg="#1f3a5f",
    sel_bg="#264f78",
    same_word_bg="#4a3b00",
    gutter_bg="#161b22",
    gutter_text="#8b949e",
    gutter_current="#58a6ff",
    find_bg="#3d3410",
    find_current="#7a5c00",
    status_ok="#56d364",
    status_error="#ff7b72",
    shadow="#000000",
    tokens=_dark_tokens(),
)

THEMES = {LIGHT.key: LIGHT, DARK.key: DARK}

TOKEN_KINDS = tuple(LIGHT.tokens)


def get_theme(key: str) -> Theme:
    """Return a theme by key, defaulting to the light palette."""
    return THEMES.get(key, LIGHT)


def token_style(theme: Theme, kind: str | None) -> TokenStyle | None:
    """Return the style for a token kind, or None for default text."""
    if kind is None:
        return None
    return theme.tokens.get(kind)


def apply_highlight_overrides(
    theme: Theme, overrides: Mapping[str, Mapping[str, Any]]
) -> Theme:
    """Return a copy of ``theme`` with per-kind highlight overrides applied.

    ``overrides`` maps a token kind (for example ``"mne"`` or ``"aacmd"``) to a
    partial style dict such as ``{"fg": "#ff7b72", "bold": True}``. Unknown
    kinds are ignored.
    """
    tokens = dict(theme.tokens)
    boolean_fields = {"bold", "italic", "underline"}
    colour_fields = {"fg", "bg"}
    for kind, override in overrides.items():
        if kind not in tokens or not isinstance(override, Mapping):
            continue
        base = tokens[kind]
        payload: dict[str, Any] = {}
        for key in (field.name for field in fields(TokenStyle)):
            if key not in override:
                continue
            value = override[key]
            if key in boolean_fields:
                payload[key] = bool(value)
            elif key in colour_fields:
                if key == "bg" and value is None:
                    payload[key] = None
                elif isinstance(value, str) and is_hex_colour(value):
                    payload[key] = value
            elif isinstance(value, str):
                payload[key] = value
        tokens[kind] = replace(base, **payload)
    # ``Theme`` is frozen with slots, so ``theme.__dict__`` does not exist.
    return replace(theme, tokens=tokens)


def is_hex_colour(value: str) -> bool:
    """Return whether ``value`` is a ``#rrggbb`` colour Tk will accept."""
    if len(value) != 7 or not value.startswith("#"):
        return False
    return all(char in "0123456789abcdefABCDEF" for char in value[1:])
