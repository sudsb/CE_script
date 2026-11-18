"""Persistent user preferences stored next to the application data."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .analysis import DEFAULT_CHECK_OPTIONS
from .templates import DEFAULT_ARG_FUNCTIONS, default_template_dicts, parse_templates
from .themes import TOKEN_KINDS, is_hex_colour

APP_NAME = "ce_edit"
MAX_RECENT = 10

BOOL_KEYS = (
    "show_line_numbers",
    "show_reference",
    "auto_format",
    "double_click_word",
    "dynamic_completions",
    "clipboard_variable",
    "auto_arguments",
    "auto_pair_code",
    "vim_mode",
)

DEFAULTS: dict[str, Any] = {
    "theme": "light",
    "font_size": 12,
    "output_font_size": 12,
    "show_line_numbers": True,
    "show_reference": False,
    "auto_format": True,
    "geometry": "",
    "last_dir": "",
    "recent_files": [],
    "highlight": {},
    "double_click_word": True,
    "dynamic_completions": True,
    "clipboard_variable": True,
    "auto_arguments": True,
    "auto_arg_functions": list(DEFAULT_ARG_FUNCTIONS),
    "auto_pair_code": True,
    "vim_mode": False,
    "syntax_checks": dict(DEFAULT_CHECK_OPTIONS),
    "templates": default_template_dicts(),
}


@dataclass(slots=True)
class TokenHighlight:
    """Per-kind highlight override chosen by the user."""

    fg: str | None = None
    bold: bool | None = None
    italic: bool | None = None
    underline: bool | None = None
    bg: str | None = None

    def to_dict(self) -> dict[str, Any]:
        payload: dict[str, Any] = {}
        if self.fg is not None:
            payload["fg"] = self.fg
        if self.bold is not None:
            payload["bold"] = self.bold
        if self.italic is not None:
            payload["italic"] = self.italic
        if self.underline is not None:
            payload["underline"] = self.underline
        if self.bg is not None:
            payload["bg"] = self.bg
        return payload

    @classmethod
    def from_dict(cls, payload: Any) -> TokenHighlight:
        if not isinstance(payload, dict):
            return cls()
        return cls(
            fg=payload.get("fg"),
            bold=payload.get("bold"),
            italic=payload.get("italic"),
            underline=payload.get("underline"),
            bg=payload.get("bg"),
        )


@dataclass(slots=True)
class HighlightScheme:
    """Named colour scheme for syntax tokens."""

    name: str
    tokens: dict[str, TokenHighlight] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "tokens": {
                kind: highlight.to_dict() for kind, highlight in self.tokens.items()
            },
        }

    @classmethod
    def from_dict(cls, payload: Any) -> HighlightScheme:
        if not isinstance(payload, dict):
            return cls(name="default")
        tokens: dict[str, TokenHighlight] = {}
        raw_tokens = payload.get("tokens")
        if isinstance(raw_tokens, dict):
            for kind, value in raw_tokens.items():
                tokens[kind] = TokenHighlight.from_dict(value)
        return cls(name=str(payload.get("name", "default")), tokens=tokens)


def default_config_dir() -> Path:
    """Return the platform-appropriate configuration directory."""
    appdata = os.environ.get("APPDATA")
    if appdata:
        return Path(appdata) / APP_NAME
    return Path.home() / ".config" / APP_NAME


def config_path() -> Path:
    """Return the default configuration file path."""
    return default_config_dir() / "config.json"


def _clean_highlight(value: Any) -> dict[str, dict[str, Any]]:
    """Keep only known token kinds and values Tk can actually render."""
    if not isinstance(value, dict):
        return {}
    clean: dict[str, dict[str, Any]] = {}
    for kind, item in value.items():
        if kind not in TOKEN_KINDS or not isinstance(item, dict):
            continue
        payload = TokenHighlight.from_dict(item).to_dict()
        for key in ("bold", "italic", "underline"):
            if key in payload:
                payload[key] = bool(payload[key])
        for key in ("fg", "bg"):
            colour = payload.get(key)
            if colour is not None and (
                not isinstance(colour, str) or not is_hex_colour(colour)
            ):
                payload.pop(key, None)
        if payload:
            clean[kind] = payload
    return clean


def _clean_settings(value: Any) -> dict[str, Any]:
    """Validate the editor-behaviour and template settings."""
    clean: dict[str, Any] = {
        "auto_arg_functions": list(DEFAULT_ARG_FUNCTIONS),
        "syntax_checks": dict(DEFAULT_CHECK_OPTIONS),
        "templates": default_template_dicts(),
    }
    if not isinstance(value, dict):
        return clean
    for key in BOOL_KEYS:
        if key in value:
            clean[key] = bool(value[key])
    functions = value.get("auto_arg_functions")
    if isinstance(functions, (list, tuple)):
        names = [
            str(item).strip()
            for item in functions
            if isinstance(item, str) and item.strip()
        ]
        clean["auto_arg_functions"] = names[:30] or list(DEFAULT_ARG_FUNCTIONS)
    checks = value.get("syntax_checks")
    if isinstance(checks, dict):
        clean["syntax_checks"] = {
            key: bool(checks.get(key, enabled))
            for key, enabled in DEFAULT_CHECK_OPTIONS.items()
        }
    if "templates" in value:
        clean["templates"] = [
            rule.to_dict() for rule in parse_templates(value["templates"])
        ]
    return clean


def load_config(path: Path | None = None) -> dict[str, Any]:
    """Load preferences, falling back to defaults on any read or type error."""
    target = path or config_path()
    config = dict(DEFAULTS)
    config.update(_clean_settings(DEFAULTS))
    try:
        payload = json.loads(target.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return config
    if not isinstance(payload, dict):
        return config
    for key, value in payload.items():
        if key not in DEFAULTS:
            continue
        if key == "highlight":
            config[key] = _clean_highlight(value)
        elif key == "font_size":
            try:
                config[key] = max(7, min(36, int(value)))
            except (TypeError, ValueError):
                pass
        elif key == "output_font_size":
            try:
                config[key] = max(8, min(30, int(value)))
            except (TypeError, ValueError):
                pass
        elif key in BOOL_KEYS:
            config[key] = bool(value)
        elif key == "recent_files":
            config[key] = (
                [str(item) for item in value][:MAX_RECENT]
                if isinstance(value, (list, tuple))
                else []
            )
        elif key in ("theme", "geometry", "last_dir"):
            config[key] = value if isinstance(value, str) else DEFAULTS[key]
        else:
            config[key] = value
    # Validate the structured settings (templates / checks / function list).
    config.update(_clean_settings(payload))
    return config


def save_config(config: dict[str, Any], path: Path | None = None) -> None:
    """Atomically write preferences, silently ignoring unwritable paths."""
    target = path or config_path()
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(".tmp")
        temporary.write_text(
            json.dumps(config, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, target)
    except OSError:
        return
