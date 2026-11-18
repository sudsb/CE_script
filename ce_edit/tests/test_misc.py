"""Tests for themes and animation helpers."""

from __future__ import annotations

from pathlib import Path

import pytest

from ce_edit.animation import (
    blend_colors,
    clamp,
    ease_in_out_cubic,
    ease_out_cubic,
    lerp,
)
from ce_edit.app import _initial_file, main
from ce_edit.themes import (
    DARK,
    LIGHT,
    THEMES,
    apply_highlight_overrides,
    get_theme,
    is_hex_colour,
    token_style,
)
from ce_edit.ui import clamp_rect, context_sequences, glass_palette


def test_themes_registered() -> None:
    assert THEMES == {"light": LIGHT, "dark": DARK}
    assert get_theme("dark") is DARK
    assert get_theme("light") is LIGHT


def test_unknown_theme_falls_back_to_light() -> None:
    assert get_theme("bogus") is LIGHT


def test_token_styles_exist_for_every_kind() -> None:
    kinds = set(LIGHT.tokens) | set(DARK.tokens)
    assert "kw" in kinds
    assert "mne" in kinds
    assert "aamark" in kinds
    for kind in kinds:
        assert kind in LIGHT.tokens
        assert kind in DARK.tokens


def test_token_style_lookup() -> None:
    assert token_style(LIGHT, None) is None
    assert token_style(LIGHT, "missing") is None
    style = token_style(LIGHT, "kw")
    assert style is not None and style.bold is True


def test_clamp() -> None:
    assert clamp(-1) == 0.0
    assert clamp(0.5) == 0.5
    assert clamp(2) == 1.0
    assert clamp(5, 0, 10) == 5


def test_easings_have_fixed_endpoints() -> None:
    assert ease_out_cubic(0) == pytest.approx(0.0)
    assert ease_out_cubic(1) == pytest.approx(1.0)
    assert ease_in_out_cubic(0) == pytest.approx(0.0)
    assert ease_in_out_cubic(0.5) == pytest.approx(0.5)
    assert ease_in_out_cubic(1) == pytest.approx(1.0)
    assert ease_out_cubic(0.5) > 0.5


def test_lerp() -> None:
    assert lerp(0, 10, 0.5) == pytest.approx(5)
    assert lerp(10, 20, -1) == pytest.approx(10)
    assert lerp(10, 20, 2) == pytest.approx(20)


def test_blend_colors() -> None:
    assert blend_colors("#000000", "#ffffff", 0) == "#000000"
    assert blend_colors("#000000", "#ffffff", 1) == "#ffffff"
    assert blend_colors("#000000", "#ffffff", 0.5) == "#7f7f7f"
    assert blend_colors("bad", "#ffffff", 0.5) == "#ffffff"


def test_highlight_overrides_work_with_frozen_slots_theme() -> None:
    original = LIGHT.tokens["kw"]
    theme = apply_highlight_overrides(
        LIGHT, {"kw": {"fg": "#123456", "bold": False}, "bogus": {"fg": "#ffffff"}}
    )
    assert theme is not LIGHT
    assert theme.tokens["kw"].fg == "#123456"
    assert theme.tokens["kw"].bold is False
    assert "bogus" not in theme.tokens
    assert LIGHT.tokens["kw"] is original
    assert all(field in dir(theme) for field in ("key", "tokens"))


def test_highlight_overrides_ignore_invalid_values() -> None:
    theme = apply_highlight_overrides(
        LIGHT, {"kw": {"fg": "not-a-colour", "bold": "yes", "bg": None}}
    )
    assert theme.tokens["kw"].fg == LIGHT.tokens["kw"].fg
    assert theme.tokens["kw"].bold is True
    assert theme.tokens["kw"].bg is LIGHT.tokens["kw"].bg


def test_is_hex_colour() -> None:
    assert is_hex_colour("#aBcDeF")
    assert not is_hex_colour("#abc")
    assert not is_hex_colour("aBcDeF")
    assert not is_hex_colour("#gggggg")


def test_clamp_rect_keeps_left_and_top_within_area() -> None:
    area = (-1920, -200, 0, 880)
    assert clamp_rect(-3000, -500, 400, 300, area, margin=8) == (-1912, -192)
    assert clamp_rect(100, 900, 400, 300, area, margin=8) == (-408, 572)


def test_glass_palette_is_theme_aware() -> None:
    light = glass_palette(LIGHT)
    dark = glass_palette(DARK)
    colours = (
        light.shadow,
        light.edge,
        light.border,
        light.body,
        light.list_bg,
        light.detail,
        light.highlight,
        light.select,
    )
    assert all(is_hex_colour(colour) for colour in colours)
    assert light.body != dark.body
    assert light.border != light.body


def test_context_sequences_include_keyboard_shortcuts() -> None:
    sequences = context_sequences()
    assert "<Button-3>" in sequences
    assert "<Shift-F10>" in sequences


def test_initial_file_accepts_paths_and_uris(tmp_path: Path) -> None:
    target = tmp_path / "table.lua"
    target.write_text("print('ok')", encoding="utf-8")
    assert _initial_file(["--verbose", str(target)]) == str(target.resolve())
    assert _initial_file([target.as_uri()]) == str(target.resolve())
    assert _initial_file([str(tmp_path / "missing.lua")]) is None
    assert _initial_file([str(tmp_path)]) is None


def test_main_passes_external_file_to_window(tmp_path: Path, monkeypatch) -> None:
    target = tmp_path / "script.aa"
    target.write_text("[ENABLE]", encoding="utf-8")
    captured: dict[str, object] = {}

    class _FakeApp:
        def __init__(self, initial_file: str | None = None) -> None:
            captured["initial_file"] = initial_file

        def mainloop(self) -> None:
            captured["loop"] = True

    import ce_edit.app as app_module

    monkeypatch.setattr(app_module, "CeEditApp", _FakeApp)
    main([str(target)])
    assert captured == {"initial_file": str(target.resolve()), "loop": True}
