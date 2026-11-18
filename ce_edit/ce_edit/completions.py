"""Completion catalogue and prefix matching for CE Lua / AA scripts."""

from __future__ import annotations

import json
import re
from collections.abc import Iterable
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

DATA_FILE = Path(__file__).resolve().parent / "data" / "completions.json"

WORD_RE = re.compile(
    r"(?:[\[{]?\$?[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*|[=!<>&|~]+)$"
)

LUA_KEYWORDS = frozenset(
    "if then else elseif end for while do repeat until break goto return in "
    "local function and or not true false nil".split()
)

_AA_CATEGORY_HINTS = (
    "Auto Assembler",
    "表脚本标记",
    "扫描",
    "自动汇编",
    "汇编指令",
    "寄存器",
    "汇编尺寸",
    "条件断点",
    "运算符",
    "虚拟键码",
)
_AA_CATEGORIES = frozenset(_AA_CATEGORY_HINTS)
_NON_API_CATEGORIES = frozenset(
    {
        "Lua 关键字",
        "汇编指令",
        "寄存器",
        "虚拟键码",
        "汇编尺寸",
        "运算符",
        "条件断点",
        "键鼠",
    }
)
_IDENTIFIER_RE = re.compile(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*$")
_RECEIVER_RE = re.compile(r"^([A-Za-z_]\w*)\.([A-Za-z_]\w*)")


@dataclass(frozen=True, slots=True)
class Completion:
    """One completion entry shown in the popup and API reference."""

    name: str
    signature: str
    category: str

    @property
    def is_call(self) -> bool:
        """Return whether accepting this entry should add parentheses."""
        if self.name in LUA_KEYWORDS:
            return False
        if not self.name or " " in self.name:
            return False
        if self.name.startswith(("[", "{$")):
            return False
        return "(" in self.signature


def _load_raw() -> list[dict[str, str]]:
    try:
        with DATA_FILE.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
    except (OSError, ValueError):
        return []
    if not isinstance(payload, list):
        return []
    return [item for item in payload if isinstance(item, dict)]


def _dedupe(items: list[Completion]) -> tuple[Completion, ...]:
    seen: set[str] = set()
    result: list[Completion] = []
    for item in items:
        if item.name in seen:
            continue
        seen.add(item.name)
        result.append(item)
    return tuple(result)


@lru_cache(maxsize=1)
def load_completions() -> tuple[Completion, ...]:
    """Load the bundled completion data, adding dotted receiver aliases."""
    real: list[Completion] = []
    for raw in _load_raw():
        name = str(raw.get("name", "")).strip()
        signature = str(raw.get("signature", "")).strip()
        category = str(raw.get("category", "")).strip()
        if not name:
            continue
        real.append(Completion(name, signature, category))

    aliases: list[Completion] = []
    for item in real:
        if "." in item.name:
            continue
        match = _RECEIVER_RE.match(item.signature)
        if not match:
            continue
        receiver, member = match.groups()
        if member != item.name:
            continue
        aliases.append(
            Completion(
                f"{receiver}.{item.name}",
                item.signature,
                item.category,
            )
        )
    return _dedupe(real + aliases)


@lru_cache(maxsize=1)
def category_order() -> tuple[str, ...]:
    """Return categories in their catalogue order."""
    seen: dict[str, None] = {}
    for item in load_completions():
        seen.setdefault(item.category, None)
    return tuple(seen)


@lru_cache(maxsize=1)
def api_names() -> frozenset[str]:
    """Return names that the lexer should colour as CE / Lua built-ins."""
    names: set[str] = set()
    for item in load_completions():
        if item.category in _NON_API_CATEGORIES:
            continue
        if _IDENTIFIER_RE.fullmatch(item.name):
            names.add(item.name)
    return frozenset(names)


def word_prefix(text: str, caret: int) -> str:
    """Return the identifier-like prefix immediately before ``caret``."""
    start = max(0, caret - 240)
    match = WORD_RE.search(text, start, caret)
    return match.group(0) if match else ""


@lru_cache(maxsize=1)
def _category_rank() -> dict[str, int]:
    return {name: index for index, name in enumerate(category_order())}


def _score(item: Completion, prefix: str, mode: str) -> tuple[int, int, int]:
    folded = item.name.casefold()
    needle = prefix.casefold()
    starts = folded.startswith(needle)
    contains = needle in folded
    aa_item = item.category in _AA_CATEGORIES
    preferred = aa_item if mode == "aa" else not aa_item
    if starts and preferred:
        bucket = 0
    elif starts:
        bucket = 1
    elif contains and preferred:
        bucket = 2
    else:
        bucket = 3
    ranks = _category_rank()
    order = ranks.get(item.category, len(ranks))
    return bucket, order, len(item.name)


def _keyword_matches(name: str, needle: str) -> bool:
    """Match a prefix or a word segment, never an arbitrary inner substring.

    ``cc`` must not match ``axcc``: doing so effectively splits one word into
    several fragments. ``ax`` matches ``axcc``, and ``value`` matches
    ``axcc_value`` because it starts after an underscore separator.
    """
    if not needle:
        return True
    folded = name.casefold()
    return folded.startswith(needle) or any(
        char in "_.$" and folded[index + 1 :].startswith(needle)
        for index, char in enumerate(folded)
    )


def filter_completions(
    prefix: str,
    mode: str = "lua",
    limit: int = 60,
    extra: Iterable[str] = (),
) -> list[Completion]:
    """Return mode-aware, prefix-first completion candidates.

    ``extra`` holds keywords extracted from the current document; they are
    placed before the bundled API catalogue so user-defined symbols win.
    """
    needle = prefix.casefold()
    if needle:
        candidates = [
            item
            for item in load_completions()
            if needle in item.name.casefold()
        ]
    else:
        candidates = list(load_completions())
    candidates.sort(key=lambda item: _score(item, prefix, mode))

    dynamic: list[Completion] = []
    seen = {item.name.casefold() for item in candidates}
    for name in extra:
        folded = str(name).casefold()
        if not folded or folded in seen or folded == needle:
            continue
        if not _keyword_matches(folded, needle):
            continue
        seen.add(folded)
        dynamic.append(Completion(str(name), f"{name}  ·  文档符号", "文档符号"))
    dynamic.sort(
        key=lambda item: (
            not item.name.casefold().startswith(needle),
            len(item.name),
            item.name.casefold(),
        )
    )
    return (dynamic + candidates)[:limit]


def find_completion(name: str) -> Completion | None:
    """Return the catalogue entry with exactly ``name``."""
    for item in load_completions():
        if item.name == name:
            return item
    return None


def grouped_reference() -> list[tuple[str, list[Completion]]]:
    """Group catalogue entries by category for the reference panel."""
    groups: dict[str, list[Completion]] = {}
    for item in load_completions():
        groups.setdefault(item.category, []).append(item)
    return list(groups.items())
