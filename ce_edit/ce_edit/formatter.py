"""Spacing, indentation, comment and XML formatting for CE scripts."""

from __future__ import annotations

import re
from bisect import bisect_right
from dataclasses import dataclass

from .lexer import Token, compute_line_starts, tokenize

NUMBER = re.compile(
    r"(?:0[xX][0-9a-fA-F]*(?:\.[0-9a-fA-F]*)?(?:[pP][-+]?\d+)?|"
    r"(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)"
)
LONG_OPEN = re.compile(r"\[(=*)\[")
LONG_COMMENT = re.compile(r"--\[(=*)\[")
FMT_OP2 = frozenset(("==", "~=", "<=", ">=", "//", "..", "<<", ">>"))
FMT_OP1 = frozenset("+-*/%^<>=&|~#")
FMT_PREFIX_KW = frozenset(
    (
        "return and or not then do else elseif if while until in repeat "
        "local for goto break function"
    ).split()
)
CLOSER_RE = re.compile(r"^(?:end|else|elseif|until|\}|\)|\])")
OPENER_AFTER_RE = re.compile(r"(?:^|[\s;()])(?:then|do|else|repeat)$")
FUNCTION_START_RE = re.compile(r"^(?:local\s+)?function\b")
FUNCTION_EXPR_RE = re.compile(r"\bfunction\s*\([^)]*\)\s*$")
XML_START_RE = re.compile(r"^<\s*[A-Za-z_]")
XML_END_RE = re.compile(r"^<\s*\//")
XML_INLINE_RE = re.compile(r"^<[^/!?][^>]*>[\s\S]*</[A-Za-z_][^>]*>$")


@dataclass(slots=True)
class _Piece:
    kind: str
    text: str
    unary: bool = False


def _needs_space(previous: _Piece | None, current: _Piece) -> bool:
    if previous is None:
        return False
    if previous.kind == "open":
        return False
    if current.kind == "close":
        return False
    if current.kind == "punct":
        return False
    if previous.kind == "punct":
        return previous.text in {",", ";"}
    if previous.kind == "op":
        return not previous.unary
    if current.kind == "op":
        return previous.kind == "word" if current.unary else True
    return False


def format_line(src: str) -> str:
    """Insert conservative spacing into one Lua line without deleting input."""
    length = len(src)
    index = 0
    out = ""
    pending = ""
    previous: _Piece | None = None

    def emit(text: str, kind: str, unary: bool = False) -> None:
        nonlocal out, pending, previous
        current = _Piece(kind, text, unary)
        drop = (
            previous is not None
            and (previous.kind == "open" or current.kind == "close")
        )
        space = "" if drop else pending
        if out == "":
            out = space + text
        elif space != "":
            out += space + text
        elif _needs_space(previous, current):
            out += " " + text
        else:
            out += text
        pending = ""
        previous = current

    def tail_comment() -> None:
        nonlocal out, index
        if out == "":
            out = pending + src[index:]
        elif pending != "":
            out += pending + src[index:]
        else:
            out += " " + src[index:]
        index = length

    while index < length:
        char = src[index]
        two = src[index : index + 2]
        if char in " \t":
            start = index
            while index < length and src[index] in " \t":
                index += 1
            pending += src[start:index]
            continue
        if two == "--":
            match = LONG_COMMENT.match(src, index)
            if match:
                close = "]" + match.group(1) + "]"
                found = src.find(close, match.end())
                if found != -1:
                    emit(src[index : found + len(close)], "atom")
                    index = found + len(close)
                    continue
            tail_comment()
            break
        if char in "\"'":
            position = index + 1
            while position < length and src[position] != char:
                if src[position] == "\\":
                    position += 1
                position += 1
            position = min(position + 1, length)
            emit(src[index:position], "atom")
            index = position
            continue
        match = LONG_OPEN.match(src, index)
        if match:
            close = "]" + match.group(1) + "]"
            found = src.find(close, index + len(match.group(0)))
            end = length if found == -1 else found + len(close)
            emit(src[index:end], "atom")
            index = end
            continue
        if src.startswith("...", index):
            emit("...", "atom")
            index += 3
            continue
        if two == "..":
            emit("..", "op")
            index += 2
            continue
        if char.isdigit() or (
            char == "." and src[index + 1 : index + 2].isdigit()
        ):
            match = NUMBER.match(src, index)
            if match:
                number = match.group(0)
                after = index + len(number)
                if number.endswith(".") and src[after : after + 1] == ".":
                    number = number[:-1]
                emit(number, "atom")
                index += len(number)
                continue
        if char.isalpha() or char == "_":
            match = re.match(r"[A-Za-z_]\w*", src[index:])
            if match:
                emit(match.group(0), "word")
                index += match.end()
                continue
        if two == "::":
            emit("::", "punct")
            index += 2
            continue
        if two in FMT_OP2:
            emit(two, "op")
            index += 2
            continue
        if char in "([{":
            emit(char, "open")
            index += 1
            continue
        if char in ")]}":
            emit(char, "close")
            index += 1
            continue
        if char in FMT_OP1:
            unary = char == "#"
            if char in "-+~":
                unary = (
                    previous is None
                    or previous.kind == "op"
                    or previous.kind == "open"
                    or (
                        previous.kind == "punct"
                        and previous.text != "."
                    )
                    or (
                        previous.kind == "word"
                        and previous.text in FMT_PREFIX_KW
                    )
                )
            emit(char, "op", unary)
            index += 1
            continue
        if char in ",;.:":
            emit(char, "punct")
            index += 1
            continue
        emit(char, "atom")
        index += 1
    return pending if out == "" else out


def format_aa_line(src: str) -> str:
    """Normalise AA whitespace while keeping the line easy to read."""
    lead_match = re.match(r"[ \t]*", src)
    lead = lead_match.group(0) if lead_match else ""
    body = src[len(lead) :]
    length = len(body)
    index = 0
    out = ""
    pending = ""

    def push(text: str) -> None:
        nonlocal out, pending
        if pending != "":
            out += " "
            pending = ""
        out += text

    while index < length:
        char = body[index]
        if char in " \t":
            pending += char
            index += 1
            continue
        if char == "/" and body[index + 1 : index + 2] == "/":
            push(body[index:])
            break
        if char == ",":
            push(",")
            index += 1
            while index < length and body[index] in " \t":
                index += 1
            continue
        if char == "[":
            last = out.rstrip(" \t")
            if (
                last
                and re.fullmatch(r"[A-Za-z0-9_)]", last[-1])
                and pending == ""
            ):
                pending = " "
            push("[")
            index += 1
            continue
        end = index
        while end < length and body[end] not in " \t,[](){}:+-*/#":
            end += 1
        if end == index:
            push(char)
            index += 1
            continue
        push(body[index:end])
        index = end
    return (lead + out.rstrip(" \t")) if out else lead


def auto_indent(line: str) -> str:
    """Return the indentation that the next line should start with."""
    lead_match = re.match(r"[ \t]*", line)
    lead = lead_match.group(0) if lead_match else ""
    stripped = re.sub(r"--\[(=*)\[[\s\S]*?\]\1\]", "", line)
    stripped = re.sub(r"--.*$", "", stripped).strip()
    if stripped == "":
        return lead
    if re.search(r"\bend$", stripped) or re.search(r"\buntil\b", stripped):
        return lead
    opener = bool(
        OPENER_AFTER_RE.search(stripped)
        or FUNCTION_START_RE.match(stripped)
        or FUNCTION_EXPR_RE.search(stripped)
        or stripped.endswith(("(", "{", "["))
        or stripped.endswith(",")
    )
    return lead + "  " if opener else lead


def comment_mark(mode: str) -> str:
    """Return the line-comment marker used by a language mode."""
    return "--" if mode == "lua" else "//"


def toggle_comment_lines(lines: list[str], mode: str) -> list[str]:
    """Toggle line comments for a whole block.

    A block uncomments only when every non-blank line is already commented;
    otherwise only the uncommented lines are commented, leaving existing
    comments untouched.
    """
    mark = comment_mark(mode)
    marker = re.compile(r"^(\s*)(?:--|//)\s?")
    nonblank = [line for line in lines if line.strip()]
    all_comment = bool(nonblank) and all(
        marker.match(line) for line in nonblank
    )
    result: list[str] = []
    for line in lines:
        if not line.strip():
            result.append(line)
        elif all_comment:
            result.append(marker.sub(r"\1", line, count=1))
        elif marker.match(line):
            result.append(line)
        else:
            result.append(
                re.sub(
                    r"^(\s*)",
                    lambda match: match.group(1) + mark + " ",
                    line,
                    count=1,
                )
            )
    return result


def looks_xml_doc(src: str) -> bool:
    """Return whether a document is a CE cheat-table XML file."""
    text = src.strip()
    if not text:
        return False
    if text.startswith("<?xml"):
        return True
    if not re.match(r"^<[A-Za-z_][^>]*>", text):
        return False
    return bool(re.search(r"</[A-Za-z_][^>]*>\s*$", text))


def format_xml_doc(src: str) -> str:
    """Re-indent XML tags while leaving script bodies untouched."""
    text = src.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r">[ \t]*<(?=[A-Za-z_/!?])", ">\n<", text)
    lines = text.split("\n")
    out: list[str] = []
    depth = 0
    for raw in lines:
        text = raw.strip()
        if text == "":
            out.append("")
            continue
        if not text.startswith("<"):
            out.append(raw)
            continue
        line = (
            re.sub(r"<\s+", "<", text)
            .replace("</ ", "</")
            .replace(" />", "/>")
        )
        line = re.sub(r"\s+>", ">", line)
        is_pi = line.startswith("<?")
        is_close = line.startswith("</")
        self_close = line.endswith("/>")
        inline = bool(XML_INLINE_RE.match(line))
        if is_close:
            depth = max(0, depth - 1)
        out.append("  " * depth + line)
        if (
            not is_pi
            and not is_close
            and not self_close
            and not inline
            and re.match(r"^<[^/!?]", line)
        ):
            depth += 1
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out)


def _protected_lines(src: str, tokens: list[Token]) -> set[int]:
    """Return line indexes that must not be reformatted (string bodies)."""
    starts = compute_line_starts(src)
    protected: set[int] = set()
    for token in tokens:
        if token.kind not in {"str", "strd", "com", "comdoc"}:
            continue
        first = bisect_right(starts, token.start) - 1
        last = bisect_right(starts, max(token.start, token.end - 1)) - 1
        if last > first:
            start = first if token.kind in {"str", "strd"} else first + 1
            protected.update(range(start, last + 1))
    return protected


def _lua_is_opener(previous: str) -> bool:
    body = re.sub(r"--\[(=*)\[[\s\S]*?\]\1\]", "", previous)
    body = re.sub(r"--.*$", "", body).strip()
    return bool(
        OPENER_AFTER_RE.search(body)
        or FUNCTION_START_RE.match(body)
        or FUNCTION_EXPR_RE.search(body)
        or body.endswith(("(", "{", "["))
        or body.endswith(",")
    )


def format_document(
    src: str,
    line_modes: list[str] | None = None,
    tokens: list[Token] | None = None,
) -> str:
    """Format a whole mixed Lua / AA / C script."""
    if not src.strip():
        return src
    if looks_xml_doc(src):
        return format_xml_doc(src)
    if line_modes is None or tokens is None:
        result = tokenize(src)
        modes: list[str] = result.line_modes
        lex_tokens: list[Token] = result.tokens
    else:
        modes = line_modes
        lex_tokens = tokens
    protected = _protected_lines(src, lex_tokens)
    lines = src.split("\n")
    out: list[str] = []
    previous_indent = ""
    previous_body = ""
    for number, raw in enumerate(lines):
        mode = modes[number] if number < len(modes) else "lua"
        if number in protected:
            out.append(raw.rstrip())
            previous_indent = raw[: len(raw) - len(raw.lstrip(" \t"))]
            previous_body = ""
            continue
        if mode == "aa":
            lead_match = re.match(r"[ \t]*", raw)
            lead = lead_match.group(0) if lead_match else ""
            out.append(lead + format_aa_line(raw[len(lead) :]))
            previous_indent = lead
            previous_body = ""
            continue
        if mode == "c":
            out.append(raw.rstrip())
            lead_match = re.match(r"[ \t]*", raw)
            previous_indent = lead_match.group(0) if lead_match else ""
            previous_body = ""
            continue
        formatted = format_line(raw)
        content = formatted.lstrip(" \t")
        trimmed = content.strip()
        indent = previous_indent
        if _lua_is_opener(previous_body) and not CLOSER_RE.match(trimmed):
            indent += "  "
        elif CLOSER_RE.match(trimmed):
            indent = indent[:-2] if len(indent) >= 2 else ""
        out.append(indent + content)
        previous_indent = indent
        previous_body = trimmed
    return "\n".join(out)
