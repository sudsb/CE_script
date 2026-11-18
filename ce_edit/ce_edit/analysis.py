"""Document analysis shared by completion, templates and the linter.

Everything here works on plain source strings so it can be unit-tested
without a Tk display. Only ``check_syntax`` needs the tokenizer result, which
the editor already caches.
"""

from __future__ import annotations

import re
from bisect import bisect_right
from collections import Counter
from collections.abc import Mapping
from dataclasses import dataclass
from functools import lru_cache

from .completions import api_names
from .lexer import (
    AA_CMDS,
    ASM_DIR,
    ASM_JMP,
    ASM_MNE,
    ASM_REG,
    ASM_SIZE,
    C_KW,
    C_TYPE,
    LUA_CONST,
    LUA_KW_CTRL,
    LUA_KW_DECL,
    LUA_KW_OP,
    LUA_LIBS,
    LexResult,
    tokenize,
)

WORD_CHAR = re.compile(r"[A-Za-z0-9_$]")
# One *complete* lexical token. Dotted/colon names (``game.exe``, ``control.Max``)
# are matched as a whole so they are never split into fragments such as
# ``game`` + ``exe`` in the completion list.
WORD_TOKEN = re.compile(r"[A-Za-z0-9_$][A-Za-z0-9_$]*(?:[.:][A-Za-z0-9_$]+)*")
IDENT_FULL = re.compile(r"[A-Za-z_$][A-Za-z0-9_$]*\Z")
HEX = re.compile(r"[0-9A-Fa-f]+\Z")

SECTION_RE = re.compile(r"^[ \t]*\[\s*(ENABLE|DISABLE)\s*\][ \t]*$", re.I | re.M)
DEFINITION_RE = re.compile(
    r"^[ \t]*(alloc|globalalloc|sharedalloc|aobscanmodule|aobscanregion|"
    r"aobscan|define|label|registersymbol|unregistersymbol|dealloc)"
    r"\s*\(\s*([A-Za-z_]\w*)\s*[,，)]",
    re.I | re.M,
)
LUA_LOCAL_RE = re.compile(
    r"^[ \t]*local\s+(?:function\s+)?([A-Za-z_]\w*)", re.M
)
LUA_FUNCTION_RE = re.compile(
    r"^[ \t]*function\s+([A-Za-z_]\w*(?:[.:][A-Za-z_]\w*)?)", re.M
)
COLON_LABEL_RE = re.compile(r"^[ \t]*([A-Za-z_]\w*)\s*:(?!:)[ \t]*$", re.M)
ASSIGN_RE = re.compile(r"^[ \t]*([A-Za-z_]\w*)\s*=(?!=)", re.M)

# Comments and quoted strings removed before keyword extraction. Newlines are
# kept so line-oriented checks can still use the same helper.
NOISE = re.compile(
    r"--\[\[[\s\S]*?\]\]|--[^\n]*|//[^\n]*|/\*[\s\S]*?\*/|"
    r"\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'|\[\[[\s\S]*?\]\]"
)
LINE_COMMENT = re.compile(r"--[^\n]*|//[^\n]*|/\*.*?\*/")

# Only a plain alloc needs an explicit dealloc(): per the CE Auto Assembler
# help, globalalloc registers itself and reuses the memory across scripts.
ALLOC_KINDS = frozenset({"alloc"})
CALL_RE = {
    "registersymbol": re.compile(r"\bregistersymbol\s*\(\s*([A-Za-z_]\w*)", re.I),
    "unregistersymbol": re.compile(r"\bunregistersymbol\s*\(\s*([A-Za-z_]\w*)", re.I),
    "dealloc": re.compile(r"\bdealloc\s*\(\s*([A-Za-z_]\w*)", re.I),
}

DEFAULT_CHECK_OPTIONS: dict[str, bool] = {
    "brackets": True,
    "strings": True,
    "sections": True,
    "pairs": True,
    "labels": True,
    "lua_blocks": True,
    "aa_blocks": True,
    "duplicates": True,
    "aa_statements": True,
}

AA_BARE_WORDS = frozenset(
    {
        *AA_CMDS,
        *ASM_DIR,
        *ASM_JMP,
        *ASM_MNE,
        *ASM_SIZE,
        *C_KW,
        *C_TYPE,
        *LUA_CONST,
        *LUA_KW_CTRL,
        *LUA_KW_DECL,
        *LUA_KW_OP,
        *LUA_LIBS,
        "alloc",
        "globalalloc",
        "sharedalloc",
        "dealloc",
        "registersymbol",
        "unregistersymbol",
        "aobscan",
        "aobscanmodule",
        "aobscanregion",
        "define",
        "label",
        "readmem",
        "fullaccess",
        "createthread",
        "loadlibrary",
        "loadbinary",
        "include",
        "assert",
        "db",
        "dw",
        "dd",
        "dq",
        "resb",
        "resw",
        "resd",
        "resq",
        "struct",
        "endstruct",
        "enable",
        "disable",
    }
)


def active_checks(options: Mapping[str, bool] | None = None) -> int:
    """Number of enabled checks, used to show that the linter really ran."""
    opts = {**DEFAULT_CHECK_OPTIONS, **(options or {})}
    return sum(1 for enabled in opts.values() if enabled)

_STOPWORDS = frozenset(
    {
        *LUA_KW_CTRL,
        *LUA_KW_DECL,
        *LUA_KW_OP,
        *LUA_CONST,
        *LUA_LIBS,
        *AA_CMDS,
        *ASM_DIR,
        *ASM_JMP,
        *ASM_MNE,
        *ASM_SIZE,
        *C_KW,
        *C_TYPE,
        "print",
        "alloc",
        "dealloc",
        "registersymbol",
        "unregistersymbol",
        "enable",
        "disable",
        "lua",
        "asm",
        "ccode",
        "local",
        "function",
        "end",
    }
)


@dataclass(frozen=True, slots=True)
class Section:
    """One ``[ENABLE]`` / ``[DISABLE]`` block, as character offsets."""

    name: str
    start: int
    body_start: int
    end: int
    line: int


@dataclass(frozen=True, slots=True)
class Definition:
    """A user-defined symbol found in the document."""

    name: str
    kind: str
    line: int


@dataclass(frozen=True, slots=True)
class Diagnostic:
    """One linter finding."""

    level: str  # "error" | "warning"
    line: int
    message: str
    code: str


@dataclass(frozen=True, slots=True)
class SyntaxReport:
    """Aggregated syntax check result."""

    diagnostics: tuple[Diagnostic, ...]

    @property
    def errors(self) -> tuple[Diagnostic, ...]:
        return tuple(item for item in self.diagnostics if item.level == "error")

    @property
    def warnings(self) -> tuple[Diagnostic, ...]:
        return tuple(item for item in self.diagnostics if item.level == "warning")

    @property
    def ok(self) -> bool:
        return not self.errors and not self.warnings

    @property
    def message(self) -> str:
        if self.errors:
            first = self.errors[0]
            extra = f"（共 {len(self.errors)} 个错误）" if len(self.errors) > 1 else ""
            return f"第 {first.line} 行：{first.message}{extra}"
        if self.warnings:
            first = self.warnings[0]
            extra = f"（共 {len(self.warnings)} 项提示）" if len(self.warnings) > 1 else ""
            return f"第 {first.line} 行：{first.message}{extra}"
        return ""

    def lines(self) -> list[str]:
        return [
            f"[{item.level}] 第 {item.line} 行 {item.message}"
            for item in self.diagnostics
        ]


def line_of(starts: list[int], offset: int) -> int:
    """Return the 1-based line containing a character offset."""
    return max(1, bisect_right(starts, offset))


@lru_cache(maxsize=1)
def _ce_keywords() -> frozenset[str]:
    """Every built-in CE / Lua keyword, compared case-insensitively."""
    return frozenset(
        {name.casefold() for name in api_names()}
        | {name.casefold() for name in _STOPWORDS}
    )


def caret_word(source: str, offset: int | None) -> str:
    """Return the word the caret is sitting on (usually a half-typed prefix)."""
    if offset is None or not source:
        return ""
    for probe in (offset, offset - 1):
        bounds = word_bounds(source, probe)
        if bounds is not None:
            return source[bounds[0] : bounds[1]]
    return ""


def word_bounds(source: str, offset: int) -> tuple[int, int] | None:
    """Return the ``[start, end)`` word range containing ``offset``."""
    if not source or offset < 0 or offset >= len(source):
        return None
    if not WORD_CHAR.fullmatch(source[offset]):
        return None
    start = offset
    while start > 0 and WORD_CHAR.fullmatch(source[start - 1]):
        start -= 1
    end = offset + 1
    while end < len(source) and WORD_CHAR.fullmatch(source[end]):
        end += 1
    return start, end


def sections(source: str) -> list[Section]:
    """Return every ``[ENABLE]`` / ``[DISABLE]`` block in the document."""
    markers = list(SECTION_RE.finditer(source))
    result: list[Section] = []
    for index, marker in enumerate(markers):
        next_start = markers[index + 1].start() if index + 1 < len(markers) else len(source)
        newline = source.find("\n", marker.end())
        body_start = len(source) if newline == -1 else newline + 1
        result.append(
            Section(
                name=marker.group(1).upper(),
                start=marker.start(),
                body_start=min(body_start, next_start),
                end=next_start,
                line=source.count("\n", 0, marker.start()) + 1,
            )
        )
    return result


def section_at(source: str, offset: int) -> Section | None:
    """Return the AA section containing ``offset``, if any."""
    for section in sections(source):
        if section.start <= offset < section.end:
            return section
    return None


def definitions(source: str) -> list[Definition]:
    """Collect user-defined symbol names in order of appearance."""
    starts = [0]
    starts.extend(match.end() for match in re.finditer(r"\n", source))
    found: list[Definition] = []
    seen: set[str] = set()

    def add(name: str, kind: str, offset: int) -> None:
        if name in seen:
            return
        seen.add(name)
        found.append(Definition(name, kind, line_of(starts, offset)))

    for match in DEFINITION_RE.finditer(source):
        add(match.group(2), match.group(1).lower(), match.start(2))
    for match in LUA_LOCAL_RE.finditer(source):
        add(match.group(1), "lua", match.start(1))
    for match in LUA_FUNCTION_RE.finditer(source):
        add(match.group(1), "lua", match.start(1))
    for match in COLON_LABEL_RE.finditer(source):
        add(match.group(1), "label", match.start(1))
    for match in ASSIGN_RE.finditer(source):
        add(match.group(1), "assign", match.start(1))
    return found


def defined_variables(source: str) -> list[str]:
    """Return names usable as ``$ver$`` template variables."""
    return [item.name for item in definitions(source)]


def clean_source(source: str) -> str:
    """Strip comments and string literals, preserving newlines."""

    def blank(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    return NOISE.sub(blank, source)


def document_keywords(
    source: str,
    offset: int | None = None,
    mode: str = "lua",
    limit: int = 200,
) -> list[str]:
    """Extract user keywords from the current input scope for completion.

    The current AA section wins first; symbols defined anywhere in the
    document are appended so an ``alloc`` in ``[ENABLE]`` is offered while
    typing ``[DISABLE]``.
    """
    scope = source
    if offset is not None:
        section = section_at(source, offset)
        if section is not None:
            scope = source[section.body_start : section.end]
    cleaned = clean_source(scope)
    builtins = _ce_keywords()
    defined = [item.name for item in definitions(source)]
    defined_folded = {name.casefold() for name in defined}
    # A half-typed word ("ne", "newm") is never a useful completion: offering
    # it as a document symbol is what made one word look split into many. A
    # real defined symbol at the caret is still kept.
    caret = caret_word(source, offset).casefold()
    excluded = {caret} if caret and caret not in defined_folded else set()
    seen: set[str] = set()
    scoped: list[str] = []

    def eligible(name: str) -> bool:
        folded = name.casefold()
        if len(name) < 2 or folded in seen or folded in excluded:
            return False
        if folded in builtins:
            return False
        return not (HEX.fullmatch(name) or ASM_REG.fullmatch(name))

    for match in WORD_TOKEN.finditer(cleaned):
        token = match.group(0)
        # Module names / file names / member calls are not editor keywords;
        # keeping them as one token is what prevents them being split apart.
        if "." in token or ":" in token:
            continue
        # Numeric literals (0x1000, AOB bytes such as 8B) are not identifiers.
        if not IDENT_FULL.fullmatch(token) or not eligible(token):
            continue
        seen.add(token.casefold())
        scoped.append(token)

    all_folded = set(seen) | defined_folded
    # Drop prefix fragments (ne / newm) when the full symbol (newmem) exists.
    scoped = [
        word
        for word in scoped
        if word.casefold() in defined_folded
        or not any(
            other != word.casefold()
            and other.startswith(word.casefold())
            and len(other) > len(word)
            for other in all_folded
        )
    ]
    result = list(scoped)
    for name in defined:
        folded = name.casefold()
        if (
            folded not in seen
            and len(name) >= 2
            and folded not in excluded
            and folded not in builtins
        ):
            seen.add(folded)
            result.append(name)
    return result[:limit]


def _line_modes(lex: LexResult, count: int) -> list[str]:
    return [
        lex.line_modes[index] if index < len(lex.line_modes) else "lua"
        for index in range(count)
    ]


def check_syntax(
    source: str,
    lex: LexResult | None = None,
    options: Mapping[str, bool] | None = None,
) -> SyntaxReport:
    """Run the configurable linter and return every finding."""
    opts = {**DEFAULT_CHECK_OPTIONS, **(options or {})}
    result = lex or tokenize(source)
    starts = result.line_starts
    lines = source.splitlines()
    modes = _line_modes(result, len(lines))
    found: list[Diagnostic] = []

    def add(level: str, line: int, message: str, code: str) -> None:
        found.append(Diagnostic(level, max(1, line), message, code))

    if opts["brackets"]:
        bad = [token for token in result.tokens if token.kind == "brerr"]
        for token in bad[:12]:
            char = source[token.start : token.end] or "?"
            label = "括号未闭合" if char in "([{" else "多余的括号"
            add("error", line_of(starts, token.start), f"{label}：{char}", "brackets")
        if not bad and not result.ok and result.message:
            add("error", 1, result.message, "brackets")
        # The lexer's bracket check intentionally ignores AA memory operands
        # such as ``[rax+0x10]``; check those per line so a missing ``]`` is
        # reported instead of silently showing "syntax OK".
        for index, line in enumerate(lines, 1):
            if modes[index - 1] != "aa":
                continue
            stripped = LINE_COMMENT.sub(" ", line).strip()
            if not stripped or SECTION_RE.fullmatch(stripped):
                continue
            if stripped.count("[") != stripped.count("]"):
                add("warning", index, "AA 操作数方括号未配对", "brackets")

    if opts["strings"]:
        reported: set[int] = set()
        for token in result.tokens:
            text = source[token.start : token.end]
            line = line_of(starts, token.start)
            if (
                token.kind in ("str", "strd")
                and text[:1] in ("'", '"')
                and (not text.rstrip().endswith(text[0]) or "\n" in text)
                and line not in reported
            ):
                add("error", line, f"未闭合的字符串（{text[0]}）", "strings")
                reported.add(line)
            elif (
                token.kind == "com"
                and text.startswith("--[[")
                and not text.rstrip().endswith("]]")
            ):
                add("error", line, "长注释 --[[ 未闭合", "strings")
        if source.count("/*") > source.count("*/"):
            add("error", len(lines) or 1, "块注释 /* 未闭合", "strings")

    if opts["sections"]:
        names = [section.name for section in sections(source)]
        if "ENABLE" in names and "DISABLE" not in names:
            add("warning", names.index("ENABLE") + 1, "缺少 [DISABLE] 段", "sections")
        if "DISABLE" in names and "ENABLE" not in names:
            add("warning", 1, "缺少 [ENABLE] 段", "sections")
        for name in ("ENABLE", "DISABLE"):
            if names.count(name) > 1:
                add("warning", 1, f"[{name}] 段重复", "sections")

    if opts["pairs"]:
        allocs = [
            item.name
            for item in definitions(source)
            if item.kind in ALLOC_KINDS
        ]
        deallocs = {name.casefold() for name in CALL_RE["dealloc"].findall(source)}
        registered = {
            name.casefold() for name in CALL_RE["registersymbol"].findall(source)
        }
        unregistered = {
            name.casefold() for name in CALL_RE["unregistersymbol"].findall(source)
        }
        for name in allocs:
            if name.casefold() not in deallocs:
                add("warning", 1, f"alloc({name}) 缺少 dealloc({name})", "pairs")
        for name in sorted(registered - unregistered):
            add(
                "warning",
                1,
                f"registersymbol({name}) 缺少 unregistersymbol({name})",
                "pairs",
            )

    if opts["labels"]:
        declared = set(
            re.findall(r"\blabel\s*\(\s*([A-Za-z_]\w*)\s*\)", source, re.I)
        )
        colon = set(COLON_LABEL_RE.findall(source))
        for name in sorted(declared - {item.casefold() for item in colon}):
            add("warning", 1, f"label({name}) 没有对应的 {name}: 标签", "labels")
        known = {item.casefold() for item in colon} | {
            item.name.casefold() for item in definitions(source)
        }
        jump = re.compile(
            r"^[ \t]*(?:jmp|call|j[a-z]{1,4})\s+([A-Za-z_]\w*)[ \t]*$", re.I
        )
        for index, line in enumerate(lines, 1):
            if modes[index - 1] != "aa":
                continue
            match = jump.match(LINE_COMMENT.sub(" ", line))
            if match and match.group(1).casefold() not in known:
                add(
                    "warning",
                    index,
                    f"跳转目标 {match.group(1)} 未定义",
                    "labels",
                )

    if opts["lua_blocks"]:
        depth = 0
        opened: list[tuple[int, str]] = []
        for index, line in enumerate(lines, 1):
            if modes[index - 1] != "lua":
                continue
            cleaned = LINE_COMMENT.sub(" ", line)
            cleaned = re.sub(r"\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", " ", cleaned)
            words = re.findall(r"[A-Za-z_]\w*", cleaned)
            has_loop = "for" in words or "while" in words
            for word in words:
                lowered = word.casefold()
                if lowered in ("function", "if") or (
                    lowered == "do" and not has_loop
                ) or lowered == "repeat":
                    depth += 1
                    opened.append((index, word))
                elif lowered in ("end", "until"):
                    if depth:
                        depth -= 1
                        opened.pop()
                    else:
                        add("warning", index, "多余的 end / until", "lua_blocks")
        if depth:
            line, word = opened[-1]
            add(
                "warning",
                line,
                f"有 {depth} 个 Lua 块缺少 end（最早：{word}）",
                "lua_blocks",
            )

    if opts.get("aa_blocks", True):
        lua_open = len(re.findall(r"\{\s*\$lua\s*\}", source, re.I))
        asm_close = len(re.findall(r"\{\s*\$asm\s*\}", source, re.I))
        ccode_open = len(re.findall(r"\{\s*\$ccode\s*\}", source, re.I))
        if lua_open > asm_close:
            add(
                "warning",
                1,
                f"有 {lua_open - asm_close} 个 {{$lua}} 块缺少 {{$asm}} 结束标记",
                "aa_blocks",
            )
        if ccode_open > asm_close:
            add(
                "warning",
                1,
                f"有 {ccode_open - asm_close} 个 {{$ccode}} 块缺少 {{$asm}} 结束标记",
                "aa_blocks",
            )

    if opts.get("duplicates", True):
        allocations = Counter(
            name.casefold()
            for name in re.findall(
                r"^[ \t]*(?:alloc|globalalloc|sharedalloc)\s*\(\s*([A-Za-z_]\w*)",
                source,
                re.I | re.M,
            )
        )
        for name, amount in sorted(allocations.items()):
            if amount > 1:
                add("warning", 1, f"分配符号 {name} 重复定义 {amount} 次", "duplicates")

    if opts.get("aa_statements", True):
        known_labels = {item.name.casefold() for item in definitions(source)}
        for index, line in enumerate(lines, 1):
            if modes[index - 1] != "aa":
                continue
            stripped = LINE_COMMENT.sub(" ", line).strip()
            if not stripped or SECTION_RE.fullmatch(stripped):
                continue
            if not re.fullmatch(r"[A-Za-z_]\w*", stripped):
                continue
            if stripped.startswith("{$") or stripped.endswith("}"):
                continue
            if stripped.casefold() in AA_BARE_WORDS:
                continue
            # A bare label without a colon, or a half-typed word such as "ne",
            # is not a valid AA statement.
            if stripped.casefold() not in known_labels or len(stripped) < 3:
                add(
                    "warning",
                    index,
                    f"无法识别的 AA 语句：{stripped}",
                    "aa_statements",
                )

    if opts["sections"]:
        for match in re.finditer(r"^[ \t]*\[\s*([A-Za-z_]\w*)\s*\][ \t]*$", source, re.M):
            if match.group(1).upper() not in ("ENABLE", "DISABLE"):
                add(
                    "warning",
                    source.count("\n", 0, match.start()) + 1,
                    f"未知的段标记 [{match.group(1)}]",
                    "sections",
                )

    code_order = {
        "strings": 0,
        "brackets": 1,
        "lua_blocks": 2,
        "sections": 3,
        "pairs": 4,
        "labels": 5,
        "aa_blocks": 6,
        "duplicates": 7,
        "aa_statements": 8,
        "unknown": 9,
    }
    found.sort(
        key=lambda item: (
            item.line,
            item.level != "error",
            code_order.get(item.code, 9),
            item.code,
        )
    )
    return SyntaxReport(tuple(found))
