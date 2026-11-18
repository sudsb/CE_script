"""User-configurable code templates for Cheat Engine scripts.

A template is a named snippet containing ``$ver$`` placeholders. The variable
is resolved from the selection, the clipboard (only when it names a symbol
already defined in the document) or the first ``alloc``/``label`` definition.
Templates can also be bound to a trigger function so accepting a completion
for ``registersymbol`` produces ``registersymbol(axcc)``.
"""

from __future__ import annotations

import re
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from typing import Any

from .analysis import SECTION_RE, clean_source, defined_variables, definitions

PLACEHOLDER = re.compile(r"\$[A-Za-z_]\w*\$")
# Any ``[NAME]`` marker line, used so users can target custom labelled blocks.
_ANY_SECTION_RE = re.compile(r"^\[([^\]]+)\]$")
ALLOC_LINE = re.compile(
    r"^[ \t]*(?:alloc|globalalloc|sharedalloc)\s*\(\s*([A-Za-z_]\w*)\s*[,，]",
    re.I,
)
CALL_STMT = re.compile(
    r"\b(alloc|globalalloc|sharedalloc|registersymbol|unregistersymbol|"
    r"dealloc|label|aobscanmodule|aobscan)\s*\(\s*([A-Za-z_]\w*)",
    re.I,
)

DEFAULT_ARG_FUNCTIONS: tuple[str, ...] = (
    "registersymbol",
    "unregistersymbol",
    "dealloc",
    "label",
)

# Section value that means "insert at the current caret / cursor position"
# instead of a fixed or named [SECTION] block.
CARET = "CARET"


@dataclass(frozen=True, slots=True)
class TemplateRule:
    """One user-defined snippet rule."""

    name: str
    trigger: str
    body: str
    section: str = "DISABLE"
    auto: bool = False

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "trigger": self.trigger,
            "body": self.body,
            "section": self.section.upper(),
            "auto": self.auto,
        }

    @classmethod
    def from_dict(cls, payload: Any) -> TemplateRule | None:
        if not isinstance(payload, dict):
            return None
        name = str(payload.get("name", "")).strip()
        body = str(payload.get("body", "")).strip()
        if not name or not body:
            return None
        raw_section = str(payload.get("section", "DISABLE")).strip()
        upper = raw_section.upper()
        if upper in ("CARET", "光标", "CURSOR", "光标位置"):
            section = CARET
        elif upper in ("ENABLE", "DISABLE"):
            section = upper
        elif raw_section:
            # Any other (custom) section name is preserved verbatim so users can
            # target their own [LABEL]/[LUA] blocks.
            section = upper
        else:
            section = "DISABLE"
        return cls(
            name=name[:60],
            trigger=str(payload.get("trigger", "")).strip()[:60],
            body=body[:2000],
            section=section,
            auto=bool(payload.get("auto", False)),
        )


@dataclass(frozen=True, slots=True)
class PairSpec:
    """A CE statement and the counterpart that belongs to another section.

    Per the official Auto Assembler help, ``dealloc`` frees memory allocated
    by ``alloc`` and is used in the disable part of a cheat-table script, while
    ``unregistersymbol`` removes a symbol added by ``registersymbol``. Dark
    Byte's canonical script registers in ``[ENABLE]`` and unregisters plus
    deallocates in ``[DISABLE]``.
    """

    statement: str
    counterpart: str
    section: str


# Built-in pairing rules. They are authoritative: user templates may add
# extra auto-generated lines but cannot break the CE pairing semantics.
PAIR_SPECS: tuple[PairSpec, ...] = (
    PairSpec("alloc", "registersymbol", "ENABLE"),
    PairSpec("registersymbol", "unregistersymbol", "DISABLE"),
    PairSpec("alloc", "dealloc", "DISABLE"),
)

DEFAULT_TEMPLATES: tuple[TemplateRule, ...] = (
    TemplateRule("注册符号", "registersymbol", "registersymbol($ver$)", "ENABLE", True),
    TemplateRule(
        "取消注册", "unregistersymbol", "unregistersymbol($ver$)", "DISABLE", True
    ),
    TemplateRule("释放内存", "dealloc", "dealloc($ver$)", "DISABLE", True),
    TemplateRule(
        "模板函数1",
        "dealloc",
        "unregistersymbol($ver$)\ndealloc($ver$)",
        "DISABLE",
        False,
    ),
)


def default_template_dicts() -> list[dict[str, Any]]:
    return [rule.to_dict() for rule in DEFAULT_TEMPLATES]


def parse_templates(payload: Any) -> tuple[TemplateRule, ...]:
    """Validate a list of template dictionaries from the config file."""
    if not isinstance(payload, list):
        return DEFAULT_TEMPLATES
    rules = [TemplateRule.from_dict(item) for item in payload[:50]]
    parsed = tuple(rule for rule in rules if rule is not None)
    return parsed or DEFAULT_TEMPLATES


def render(rule: TemplateRule, variable: str) -> str:
    """Replace every ``$name$`` placeholder with ``variable``."""
    return PLACEHOLDER.sub(variable, rule.body)


def resolve_variable(
    source: str,
    clipboard: str = "",
    selection: str = "",
    *,
    allow_clipboard: bool = True,
    allow_selection: bool = True,
) -> str | None:
    """Choose a defined symbol for ``$ver$``.

    Priority: selected text (double-clicked word), clipboard, then the first
    ``alloc``/``globalalloc`` defined in the document. Clipboard and selection
    are only accepted when the name is actually defined, which prevents
    ``Ctrl+C`` of arbitrary text from leaking into generated code.
    """
    defined = [name.casefold() for name in defined_variables(source)]
    for candidate in (
        selection if allow_selection else "",
        clipboard if allow_clipboard else "",
    ):
        text = candidate.strip().strip("()[],，")
        if IDENT_OK.fullmatch(text) and text.casefold() in defined:
            return text
    for item in definitions(source):
        if item.kind in ("alloc", "globalalloc", "sharedalloc"):
            return item.name
    return None


IDENT_OK = re.compile(r"[A-Za-z_]\w*\Z")


def completion_argument(
    function_name: str,
    source: str,
    clipboard: str = "",
    selection: str = "",
    functions: Iterable[str] = DEFAULT_ARG_FUNCTIONS,
    *,
    enabled: bool = True,
    allow_clipboard: bool = True,
) -> str | None:
    """Return the variable to place inside ``function_name(...)``."""
    if not enabled:
        return None
    allowed = {name.casefold() for name in functions}
    if function_name.casefold() not in allowed:
        return None
    return resolve_variable(
        source,
        clipboard,
        selection,
        allow_clipboard=allow_clipboard,
    )


def alloc_name(line: str) -> str | None:
    """Return the symbol of a completed ``alloc(name, size)`` line."""
    match = ALLOC_LINE.match(line)
    return match.group(1) if match else None


COUNTERPART_LINE = re.compile(
    r"^(registersymbol|unregistersymbol|dealloc)\s*\(", re.I
)
COUNTERPART_SECTION = {
    "registersymbol": "ENABLE",
    "unregistersymbol": "DISABLE",
    "dealloc": "DISABLE",
}


def _section_for_line(line: str, configured: str) -> str:
    """CE counterpart statements always belong to their canonical section.

    A misconfigured template (for example ``dealloc`` assigned to
    ``[ENABLE]``) must never place cleanup code in the enable block.
    """
    match = COUNTERPART_LINE.match(line.strip())
    if match:
        return COUNTERPART_SECTION.get(match.group(1).casefold(), configured)
    return configured


def _normalize_line(line: str) -> str:
    """Canonical form for duplicate detection (ignore spacing / case)."""
    return re.sub(r"\s+", "", line).casefold()


def _section_ranges(
    lines: list[str],
) -> tuple[list[tuple[int, str]], dict[str, tuple[int, int]]]:
    """Return section markers and the ``(body_start, body_end)`` line range."""
    markers: list[tuple[int, str]] = []
    for index, line in enumerate(lines):
        match = SECTION_RE.fullmatch(line.strip())
        if match:
            markers.append((index, match.group(1).upper()))
    ranges: dict[str, tuple[int, int]] = {}
    for position, (index, name) in enumerate(markers):
        end = markers[position + 1][0] if position + 1 < len(markers) else len(lines)
        if name not in ranges:
            ranges[name] = (index + 1, end)
    return markers, ranges


def _present_in_section(source: str, section: str, line: str) -> bool:
    """Whether ``line`` already exists *inside* the given section."""
    lines = source.splitlines()
    _markers, ranges = _section_ranges(lines)
    span = ranges.get(section)
    candidates = lines[span[0] : span[1]] if span else lines
    target = _normalize_line(line)
    return any(_normalize_line(item) == target for item in candidates)


def _call_statements(source: str, variable: str) -> set[str]:
    """Statement kinds that mention ``variable`` in executable code."""
    folded = variable.casefold()
    cleaned = clean_source(source)
    return {
        match.group(1).casefold()
        for match in CALL_STMT.finditer(cleaned)
        if match.group(2).casefold() == folded
    }


def auto_pair_blocks(
    source: str,
    variable: str,
    rules: Iterable[TemplateRule],
) -> dict[str, str]:
    """Build missing CE counterpart lines grouped by their target section.

    ``alloc`` is paired with ``dealloc`` and (implicitly registered with)
    ``registersymbol``, which in turn is paired with ``unregistersymbol``.
    Registrations belong to ``[ENABLE]``; unregistration and deallocation
    belong to ``[DISABLE]``.
    """
    if not variable:
        return {}
    kinds = _call_statements(source, variable) or {"alloc"}
    if "alloc" in kinds:
        # A plain alloc is registered in the enable section; that registration
        # then needs its own counterpart in the disable section.
        kinds.add("registersymbol")

    buckets: dict[str, list[str]] = {}

    def add(section: str, line: str) -> None:
        bucket = buckets.setdefault(section, [])
        target = _normalize_line(line)
        if all(_normalize_line(item) != target for item in bucket):
            bucket.append(line)

    for spec in PAIR_SPECS:
        if spec.statement not in kinds:
            continue
        line = f"{spec.counterpart}({variable})"
        if not _present_in_section(source, spec.section, line):
            add(spec.section, line)

    # User templates can contribute extra lines to a section; the built-in
    # pairing above always wins and duplicates are suppressed.
    for rule in rules:
        if not rule.auto:
            continue
        for line in render(rule, variable).splitlines():
            stripped = line.strip()
            if not stripped:
                continue
            section = _section_for_line(stripped, rule.section.upper())
            if not _present_in_section(source, section, stripped):
                add(section, stripped)

    return {
        section: "\n".join(lines)
        for section, lines in buckets.items()
        if lines
    }


def insert_sections(source: str, blocks: Mapping[str, str]) -> str:
    """Insert generated lines inside their labelled ``[SECTION]`` blocks.

    Any section name is supported (``ENABLE``/``DISABLE`` plus user-defined
    labels); missing sections are appended as a fresh ``[NAME]`` block.
    """
    if not blocks:
        return source
    lines = source.splitlines()
    trailing = "\n" if source.endswith("\n") else ""
    bodies = {
        name: [line.strip() for line in body.splitlines() if line.strip()]
        for name, body in blocks.items()
    }
    bodies = {name: body for name, body in bodies.items() if body}
    if not bodies:
        return source

    markers, ranges = _section_ranges(lines)
    if not markers:
        # No sections yet: wrap the existing code in [ENABLE]/[DISABLE] when a
        # CE block is requested, then append any custom [SECTION] at the end.
        has_ce = ("ENABLE" in bodies) or ("DISABLE" in bodies)
        if has_ce:
            output = ["[ENABLE]", *lines]
            if bodies.get("ENABLE"):
                output.extend(["", *bodies["ENABLE"]])
            output.extend(["[DISABLE]"])
            if bodies.get("DISABLE"):
                output.extend(["", *bodies["DISABLE"]])
        else:
            output = list(lines)
        for name, body in bodies.items():
            if name in ("ENABLE", "DISABLE"):
                continue
            if output and output[-1].strip():
                output.append("")
            output.extend([f"[{name}]", *body, ""])
        while output and not output[-1].strip():
            output.pop()
        return "\n".join(output) + trailing

    insertions: list[tuple[int, list[str]]] = []
    missing: dict[str, list[str]] = {}
    for name, body in bodies.items():
        span = ranges.get(name)
        if span is None:
            missing[name] = body
            continue
        start, end = span
        position = end
        while position > start and not lines[position - 1].strip():
            position -= 1
        insertions.append((position, body))

    # Insert from the bottom up so earlier positions stay valid.
    for position, body in sorted(insertions, reverse=True):
        lines[position:position] = ["", *body]

    if missing:
        if lines and lines[-1].strip():
            lines.append("")
        for name, body in missing.items():
            lines.extend([f"[{name}]", *body, ""])
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(lines) + trailing


def insert_into_section(source: str, section: str, text: str) -> str:
    """Insert rendered ``text`` at the end of the named ``[SECTION]`` block.

    Custom (non ``ENABLE``/``DISABLE``) section names are supported; a missing
    section is created at the end of the document. Falls back to appending when
    ``text`` is empty. Returns the (possibly unchanged) source string.
    """
    if not text:
        return source
    target = section.strip().strip("[]").upper()
    if not target or target == CARET:
        return source
    block = [line for line in text.splitlines() if line.strip()]
    if not block:
        return source
    lines = source.splitlines()
    trailing = "\n" if source.endswith("\n") else ""
    # Detect every ``[NAME]`` marker independently of the CE pair logic so users
    # can target their own labelled blocks (matching is case-insensitive).
    markers: list[tuple[int, str]] = []
    for index, line in enumerate(lines):
        match = _ANY_SECTION_RE.fullmatch(line.strip())
        if match:
            markers.append((index, match.group(1).upper()))
    ranges: dict[str, tuple[int, int]] = {}
    for position, (index, name) in enumerate(markers):
        end = markers[position + 1][0] if position + 1 < len(markers) else len(lines)
        if name not in ranges:
            ranges[name] = (index + 1, end)
    span = ranges.get(target)
    out = list(lines)
    if span is None:
        if out and out[-1].strip():
            out.append("")
        out.extend([f"[{target}]", *block, ""])
    else:
        start, end = span
        position = end
        while position > start and not lines[position - 1].strip():
            position -= 1
        out[position:position] = [*block, ""]
    return "\n".join(out) + trailing
