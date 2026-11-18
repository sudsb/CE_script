"""Tests for persistent configuration handling."""

from __future__ import annotations

import json
from pathlib import Path

from ce_edit.config import DEFAULTS, default_config_dir, load_config, save_config


def test_missing_file_returns_defaults(tmp_path: Path) -> None:
    config = load_config(tmp_path / "missing.json")
    assert config == DEFAULTS
    assert config is not DEFAULTS


def test_partial_file_merges_defaults(tmp_path: Path) -> None:
    target = tmp_path / "config.json"
    target.write_text(json.dumps({"theme": "dark", "font_size": 18}), "utf-8")
    config = load_config(target)
    assert config["theme"] == "dark"
    assert config["font_size"] == 18
    assert config["auto_format"] is True


def test_unknown_keys_are_ignored(tmp_path: Path) -> None:
    target = tmp_path / "config.json"
    target.write_text(json.dumps({"theme": "dark", "bogus": 1}), "utf-8")
    config = load_config(target)
    assert "bogus" not in config
    assert config["theme"] == "dark"


def test_corrupt_file_returns_defaults(tmp_path: Path) -> None:
    target = tmp_path / "config.json"
    target.write_text("{not json", "utf-8")
    assert load_config(target) == DEFAULTS


def test_non_object_file_returns_defaults(tmp_path: Path) -> None:
    target = tmp_path / "config.json"
    target.write_text("[1, 2, 3]", "utf-8")
    assert load_config(target) == DEFAULTS


def test_save_and_reload_roundtrip(tmp_path: Path) -> None:
    target = tmp_path / "nested" / "config.json"
    payload = dict(DEFAULTS)
    payload["theme"] = "dark"
    payload["recent_files"] = ["a.lua", "b.aa"]
    save_config(payload, target)
    assert target.exists()
    assert load_config(target) == payload


def test_save_ignores_unwritable_path(tmp_path: Path) -> None:
    blocker = tmp_path / "file"
    blocker.write_text("x", "utf-8")
    save_config(dict(DEFAULTS), blocker / "config.json")


def test_invalid_values_fall_back_to_safe_defaults(tmp_path: Path) -> None:
    target = tmp_path / "config.json"
    target.write_text(
        json.dumps(
            {
                "font_size": "not-a-number",
                "show_line_numbers": [],
                "recent_files": "oops",
                "geometry": 42,
                "highlight": {
                    "kw": {"fg": "#123456", "bold": "yes"},
                    "unknown_kind": {"fg": "#abcdef"},
                    "mne": {"fg": "not-a-colour"},
                },
            }
        ),
        "utf-8",
    )
    config = load_config(target)
    assert config["font_size"] == DEFAULTS["font_size"]
    assert config["show_line_numbers"] is False
    assert config["recent_files"] == []
    assert config["geometry"] == ""
    assert config["highlight"] == {"kw": {"fg": "#123456", "bold": True}}


def test_new_settings_are_validated(tmp_path: Path) -> None:
    target = tmp_path / "config.json"
    target.write_text(
        json.dumps(
            {
                "double_click_word": False,
                "dynamic_completions": "yes",
                "clipboard_variable": 0,
                "auto_arguments": None,
                "auto_arg_functions": ["registersymbol", 7, ""],
                "syntax_checks": {"pairs": False, "bogus": True},
                "templates": [
                    {"name": "自定义", "body": "print($ver$)", "section": "ENABLE"},
                    {"name": "", "body": ""},
                ],
            }
        ),
        "utf-8",
    )
    config = load_config(target)
    assert config["double_click_word"] is False
    assert config["dynamic_completions"] is True
    assert config["clipboard_variable"] is False
    assert config["auto_arguments"] is False
    assert config["auto_arg_functions"] == ["registersymbol"]
    assert config["syntax_checks"]["pairs"] is False
    assert "bogus" not in config["syntax_checks"]
    assert len(config["templates"]) == 1
    assert config["templates"][0]["section"] == "ENABLE"


def test_broken_templates_fall_back_to_defaults(tmp_path: Path) -> None:
    target = tmp_path / "config.json"
    target.write_text(json.dumps({"templates": "not-a-list"}), "utf-8")
    assert load_config(target)["templates"] == DEFAULTS["templates"]


def test_default_config_dir_honours_appdata(monkeypatch) -> None:
    monkeypatch.setenv("APPDATA", str(Path("C:/fake/appdata")))
    assert default_config_dir() == Path("C:/fake/appdata") / "ce_edit"
