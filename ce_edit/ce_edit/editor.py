"""The text editor widget: gutter, highlighting and editing helpers."""

from __future__ import annotations

import re
import tkinter as tk
from collections.abc import Callable, Mapping
from functools import lru_cache
from tkinter import font as tkfont
from typing import Any

from .analysis import word_bounds
from .formatter import (
    auto_indent,
    format_document,
    toggle_comment_lines,
)
from .lexer import (
    LexResult,
    is_in_string_or_comment,
    mode_at_offset,
    tokenize,
)
from .themes import Theme
from .ui import px
from .vim import VimController

INDENT = "  "
PAD_X = 8
PAIRS = {"(": ")", "[": "]", "{": "}", '"': '"', "'": "'"}
CLOSERS = {")", "]", "}", '"', "'"}
FIND_TAGS = ("find", "find_cur")
WORD_RE = re.compile(r"[A-Za-z_]\w*")


@lru_cache(maxsize=1)
def _families() -> frozenset[str]:
    try:
        return frozenset(name.casefold() for name in tkfont.families())
    except tk.TclError:
        return frozenset()


def code_family() -> str:
    """Return a monospaced family that exists on this platform."""
    names = _families()
    for candidate in ("Cascadia Mono", "Consolas", "JetBrains Mono", "Courier New"):
        if candidate.casefold() in names:
            return candidate
    return "Courier"


class CodeEditor(tk.Frame):
    """A Tk text editor tuned for Cheat Engine scripts."""

    def __init__(
        self,
        master: tk.Misc,
        theme: Theme,
        font_size: int = 12,
        on_change: Callable[[], None] | None = None,
        on_cursor: Callable[[], None] | None = None,
        on_status: Callable[[LexResult], None] | None = None,
        on_complete: Callable[[bool], None] | None = None,
    ) -> None:
        super().__init__(master, bd=0, highlightthickness=0)
        self.theme = theme
        self.font_size = font_size
        self.show_numbers = True
        self._on_change = on_change
        self._on_cursor = on_cursor
        self._on_status = on_status
        self._on_complete = on_complete
        self._highlight_job: str | None = None
        self._lex: LexResult | None = None
        self._lex_source: str | None = None
        self._family = code_family()
        self.completion_hook: Callable[[str], bool] | None = None
        self.after_return: Callable[[], None] | None = None
        self.double_click_word = True
        self._select_term: str | None = None
        self.vim = VimController(self)

        self.columnconfigure(1, weight=1)
        self.rowconfigure(0, weight=1)

        self._gutter_width = px(self, 48)
        self.gutter = tk.Canvas(
            self, width=self._gutter_width, bd=0, highlightthickness=0, takefocus=0
        )
        self.text = tk.Text(
            self,
            undo=True,
            maxundo=2000,
            autoseparators=True,
            wrap="none",
            bd=0,
            highlightthickness=0,
            padx=px(self, PAD_X),
            pady=px(self, 6),
            insertwidth=px(self, 2),
            tabs=("2c",),
            font=self._base_font(),
        )
        self.vsb = tk.Scrollbar(self, orient="vertical", command=self.text.yview)
        self.hsb = tk.Scrollbar(
            self, orient="horizontal", command=self.text.xview
        )

        self.text.configure(
            yscrollcommand=self._yscroll, xscrollcommand=self.hsb.set
        )
        self.text.grid(row=0, column=1, sticky="nsew")
        self.gutter.grid(row=0, column=0, sticky="ns")
        self.vsb.grid(row=0, column=2, sticky="ns")
        self.hsb.grid(row=1, column=1, sticky="ew")

        self._build_fonts()
        self._configure_tags()
        self._bind_events()
        self.apply_theme(theme)

    # ------------------------------------------------------------------ fonts
    def _base_font(self) -> tkfont.Font:
        return tkfont.Font(family=self._family, size=self.font_size)

    def _build_fonts(self) -> None:
        base = self._base_font()
        self._gutter_font = tkfont.Font(
            family=self._family, size=max(8, self.font_size - 1)
        )
        self._variants: dict[tuple[bool, bool, bool], tkfont.Font] = {}

        def variant(bold: bool, italic: bool, underline: bool) -> tkfont.Font:
            key = (bold, italic, underline)
            if key not in self._variants:
                self._variants[key] = tkfont.Font(
                    family=self._family,
                    size=self.font_size,
                    weight="bold" if bold else "normal",
                    slant="italic" if italic else "roman",
                    underline=underline,
                )
            return self._variants[key]

        self._variant = variant
        self.text.configure(font=base)

    def set_font_size(self, size: int) -> None:
        """Change the code font size and refresh dependent geometry."""
        self.font_size = max(7, min(36, size))
        self._build_fonts()
        self._configure_tags()
        self._draw_gutter()
        self._schedule_highlight()

    def set_show_numbers(self, show: bool) -> None:
        """Toggle the line-number gutter with DPI-aware geometry."""
        self.show_numbers = bool(show)
        self.gutter.configure(width=self._gutter_width if show else 0)
        self._draw_gutter()

    def char_count(self) -> int:
        """Count characters without copying the whole document into Python."""
        try:
            count = self.text.count("1.0", "end-1c", "chars")
        except tk.TclError:
            return 0
        return int(count[0] or 0) if count else 0

    def cancel_pending(self) -> None:
        """Cancel scheduled highlighters so they cannot fire after teardown."""
        if self._highlight_job is not None:
            try:
                self.after_cancel(self._highlight_job)
            except tk.TclError:
                pass
            self._highlight_job = None

    def _apply_token_fonts(self) -> None:
        for kind, style in self.theme.tokens.items():
            self.text.tag_configure(
                kind,
                font=self._variant(style.bold, style.italic, style.underline),
            )

    # ------------------------------------------------------------------- tags
    def _configure_tags(self) -> None:
        self.text.configure(
            bg=self.theme.bg,
            fg=self.theme.text,
            insertbackground=self.theme.caret,
            selectbackground=self.theme.sel_bg,
            selectforeground=self.theme.text,
            inactiveselectbackground=self.theme.sel_bg,
        )
        self.text.tag_configure("curline", background=self.theme.current_line)
        self.text.tag_configure("same", background=self.theme.same_word_bg)
        self.text.tag_configure(
            "matchbr",
            background=self.theme.match_bg,
            foreground=self.theme.accent,
        )
        self.text.tag_configure("find", background=self.theme.find_bg)
        self.text.tag_configure(
            "find_cur", background=self.theme.find_current, foreground="#000000"
        )
        for kind, style in self.theme.tokens.items():
            options: dict[str, str | bool] = {"foreground": style.fg}
            if style.bg:
                options["background"] = style.bg
            if style.underline:
                options["underline"] = True
            self.text.tag_configure(kind, **options)
        self._apply_token_fonts()
        for tag in ("same", "matchbr", "find", "find_cur"):
            self.text.tag_raise(tag)
        self._raise_selection()

    def _raise_selection(self) -> None:
        """Keep the drag selection visible above every background tag.

        Tk creates ``sel`` first, so ``curline``, ``same``, ``matchbr`` and
        the find tags all outrank it. Without this, dragging highlights some
        other colour (or nothing at all) while ``tag_ranges("sel")`` is
        correct -- the classic "selection and caret do not match" symptom.
        """
        try:
            self.text.tag_raise("sel")
        except tk.TclError:
            return

    def apply_theme(self, theme: Theme) -> None:
        """Apply a palette to every part of the editor."""
        self.theme = theme
        self.configure(bg=theme.bg)
        self.gutter.configure(bg=theme.gutter_bg)
        self._configure_tags()
        guide = theme.border
        for bar in (self.vsb, self.hsb):
            bar.configure(
                bg=theme.panel,
                activebackground=theme.border,
                troughcolor=theme.bg,
                bd=0,
                highlightthickness=0,
                elementborderwidth=0,
            )
        self.configure(highlightbackground=guide)
        self._draw_gutter()
        self._schedule_highlight()

    def apply_highlight_overrides(
        self, overrides: Mapping[str, Mapping[str, Any]]
    ) -> None:
        """Apply per-kind highlight overrides and re-render the editor.

        ``overrides`` maps a token kind (for example ``"mne"`` or ``"aacmd"``)
        to a partial style dict such as ``{"fg": "#ff7b72", "bold": True}``.
        Unknown kinds are ignored.
        """
        from .themes import apply_highlight_overrides as _apply

        self.theme = _apply(self.theme, overrides)
        self._configure_tags()
        self._schedule_highlight()

    # ----------------------------------------------------------------- events
    def _bind_events(self) -> None:
        self.text.bind("<<Modified>>", self._on_modified)
        self.text.bind("<KeyPress>", self._on_keypress)
        self.text.bind("<Return>", self._on_return)
        self.text.bind("<Tab>", self._on_tab)
        self.text.bind("<Shift-Tab>", self._on_dedent)
        self.text.bind("<ISO_Left_Tab>", self._on_dedent)
        self.text.bind("<BackSpace>", self._on_backspace)
        self.text.bind("<Control-slash>", self._on_toggle_comment)
        self.text.bind("<Control-Key-slash>", self._on_toggle_comment)
        self.text.bind("<Control-d>", self._on_select_next)
        for sequence in ("<Control-Shift-D>", "<Control-Shift-d>"):
            self.text.bind(sequence, self._on_duplicate)
        self.text.bind("<Control-space>", self._on_autocomplete)
        self.text.bind("<Escape>", self._on_escape_key, add="+")
        self.text.bind("<KeyRelease>", self._on_cursor_moved)
        self.text.bind("<ButtonRelease-1>", self._on_cursor_moved)
        self.text.bind("<Double-Button-1>", self._on_double_click)
        self.text.bind("<FocusIn>", lambda _event: self._draw_gutter())
        self.text.bind("<Configure>", lambda _event: self._draw_gutter())
        self.vsb.bind("<Configure>", lambda _event: self._draw_gutter())

    def _yscroll(self, first: float, last: float) -> None:
        self.vsb.set(str(first), str(last))
        self._draw_gutter()

    def _on_modified(self, _event: tk.Event) -> None:
        if not self.text.edit_modified():
            return
        self.text.edit_modified(False)
        self._schedule_highlight()
        self._draw_gutter()
        if self._on_change is not None:
            self._on_change()

    def _on_double_click(self, event: tk.Event) -> str | None:
        """Select the whole word under the pointer, including ``$`` symbols."""
        if not self.double_click_word:
            return None
        try:
            index = self.text.index(f"@{event.x},{event.y}")
            offset = self._offset_of(index)
        except (tk.TclError, ValueError):
            return None
        bounds = word_bounds(self.get_text(), offset)
        if bounds is None:
            return None
        start, end = f"1.0+{bounds[0]}c", f"1.0+{bounds[1]}c"
        self.text.tag_remove("sel", "1.0", "end")
        self.text.tag_add("sel", start, end)
        self.text.mark_set("insert", end)
        self.text.focus_set()
        self._on_cursor_moved()
        return "break"

    def _on_escape_key(self, _event: tk.Event) -> str | None:
        """Escape leaves vim insert/visual mode (or cancels a ``:`` command)."""
        if self.vim.handle_escape():
            return "break"
        return None

    def _on_select_next(self, _event: tk.Event | None = None) -> str:
        """Ctrl+D: add the next occurrence of the same word to the selection."""
        if self.vim.active():
            self.vim.scroll(10)
            return "break"
        text = self.text
        ranges = text.tag_ranges("sel")
        if not ranges:
            source = self.get_text()
            offset = self._offset_of("insert")
            bounds = word_bounds(source, offset)
            if bounds is None and offset > 0:
                bounds = word_bounds(source, offset - 1)
            if bounds is None:
                return "break"
            term = source[bounds[0] : bounds[1]]
            if not term:
                return "break"
            self._select_term = term
            start, end = f"1.0+{bounds[0]}c", f"1.0+{bounds[1]}c"
            text.tag_remove("sel", "1.0", "end")
            text.tag_add("sel", start, end)
            text.mark_set("insert", end)
        else:
            term = self._select_term or str(text.get(ranges[0], ranges[1]))
            if not term:
                return "break"
            self._select_term = term
            pattern = rf"\m{re.escape(term)}\M"
            found = text.search(
                pattern, str(ranges[-1]), stopindex="end", regexp=True
            )
            if not found:
                found = text.search(
                    pattern, "1.0", stopindex=str(ranges[0]), regexp=True
                )
            if not found:
                return "break"
            end = text.index(f"{found}+{len(term)}c")
            text.tag_add("sel", found, end)
            text.mark_set("insert", end)
        text.tag_raise("sel")
        text.see("insert")
        text.focus_set()
        self._on_cursor_moved()
        return "break"

    def _on_cursor_moved(self, event: tk.Event | None = None) -> None:
        if event is not None and getattr(event, "keysym", "") in {
            "Control_L",
            "Control_R",
            "Shift_L",
            "Shift_R",
        }:
            return
        self._paint_current_line()
        self._paint_brackets()
        self._paint_same_word()
        self._draw_gutter()
        if not self.text.tag_ranges("sel"):
            self._select_term = None
        if self._on_cursor is not None:
            self._on_cursor()

    # ------------------------------------------------------------ public text
    def get_text(self) -> str:
        """Return the current document without Tk's trailing newline."""
        return self.text.get("1.0", "end-1c")

    def set_text(self, text: str) -> None:
        """Replace the whole document and reset the undo stack."""
        self.text.delete("1.0", "end")
        self.text.insert("1.0", text)
        self.text.edit_reset()
        self.text.edit_modified(False)
        self.text.mark_set("insert", "1.0")
        self.text.see("1.0")
        self._schedule_highlight()
        self._on_cursor_moved()

    def insert_text(self, text: str) -> None:
        """Insert text at the caret, replacing every selection range."""
        if not self.replace_selections(text):
            self.text.insert("insert", text)
        self.text.see("insert")
        self.text.focus_set()

    def goto_line(self, number: int) -> None:
        """Place the caret on ``number`` and scroll it into view."""
        total = int(self.text.index("end-1c").split(".")[0])
        number = max(1, min(total, number))
        self.text.mark_set("insert", f"{number}.0")
        self.text.see(f"{number}.0")
        self.text.focus_set()
        self._on_cursor_moved()

    def position(self) -> tuple[int, int]:
        """Return the 1-based line and column of the caret."""
        line, column = self.text.index("insert").split(".")
        return int(line), int(column) + 1

    def selection_ranges(self) -> list[tuple[str, str]]:
        """Return every ``sel`` range (Ctrl+D can create several)."""
        values = self.text.tag_ranges("sel")
        return [
            (str(values[index]), str(values[index + 1]))
            for index in range(0, len(values), 2)
        ]

    def selection_size(self) -> int:
        """Return the number of selected characters across all ranges."""
        total = 0
        for start, end in self.selection_ranges():
            counts = self.text.count(start, end, "chars")
            total += int(counts[0]) if counts else 0
        return total

    def replace_selections(self, value: str) -> bool:
        """Replace every selected range with ``value`` (multi-cursor safe)."""
        ranges = self.selection_ranges()
        if not ranges:
            return False
        starts = [self._offset_of(start) for start, _ in ranges]
        self.text.edit_separator()
        self.text.tag_remove("sel", "1.0", "end")
        # Replace from the last range backwards: an edit at a later offset
        # never invalidates the offsets of earlier ranges.
        for position, ((start, end), offset) in reversed(
            list(enumerate(zip(ranges, starts, strict=True)))
        ):
            self.text.delete(start, end)
            self.text.insert(f"1.0+{offset}c", value)
            if position == len(ranges) - 1:
                self.text.mark_set(
                    "_ce_sel_end", f"1.0+{offset}c+{len(value)}c"
                )
        self.text.edit_separator()
        try:
            self.text.mark_set("insert", "_ce_sel_end")
            self.text.mark_unset("_ce_sel_end")
        except tk.TclError:
            pass
        return True

    def _current_lex(self) -> LexResult:
        """Return tokens for the current text, re-lexing when stale."""
        src = self.get_text()
        if self._lex is None or self._lex_source != src:
            self._lex = tokenize(src)
            self._lex_source = src
        return self._lex

    def document_mode(self) -> str:
        """Return the language mode at the caret."""
        return mode_at_offset(self._current_lex(), self._offset_of("insert"))

    def caret_prefix(self) -> str:
        """Return the identifier-like text before the caret."""
        from .completions import word_prefix

        text = self.get_text()
        return word_prefix(text, self._offset_of("insert"))

    def mode_prefix(self) -> tuple[str, str]:
        """Return the completion prefix and the active language mode."""
        return self.caret_prefix(), self.document_mode()

    # ------------------------------------------------------------- formatting
    def format_now(self) -> None:
        """Format the document in place, keeping the caret near its line."""
        src = self.get_text()
        if not src.strip():
            return
        line, column = self.position()
        formatted = format_document(src)
        if formatted == src:
            return
        self.text.edit_separator()
        self.text.replace("1.0", "end-1c", formatted)
        self.text.edit_separator()
        total = int(self.text.index("end-1c").split(".")[0])
        target = min(line, total)
        self.text.mark_set("insert", f"{target}.{max(0, column - 1)}")
        self.text.see("insert")
        self._on_cursor_moved()

    def toggle_comment(self) -> None:
        """Comment or uncomment the selected lines (or the current line)."""
        self._on_toggle_comment()

    # -------------------------------------------------------------- highlights
    def _schedule_highlight(self) -> None:
        self.cancel_pending()
        try:
            self._highlight_job = self.after(30, self._rehighlight)
        except tk.TclError:
            self._highlight_job = None

    def _clear_token_tags(self) -> None:
        for kind in self.theme.tokens:
            self.text.tag_remove(kind, "1.0", "end")

    def _rehighlight(self) -> None:
        self._highlight_job = None
        if not self.text.winfo_exists():
            return
        src = self.get_text()
        if self._lex is not None and self._lex_source == src:
            result = self._lex
        else:
            result = tokenize(src)
            self._lex = result
            self._lex_source = src
        self._clear_token_tags()
        for token in result.tokens:
            if token.kind is None:
                continue
            if token.kind not in self.theme.tokens:
                continue
            self.text.tag_add(
                token.kind,
                f"1.0+{token.start}c",
                f"1.0+{token.end}c",
            )
        self.text.tag_raise("find_cur")
        self.text.tag_raise("find")
        self._raise_selection()
        self._paint_current_line()
        self._paint_same_word()
        if self._on_status is not None:
            self._on_status(result)
        self._draw_gutter()

    def _paint_current_line(self) -> None:
        self.text.tag_remove("curline", "1.0", "end")
        line = self.text.index("insert").split(".")[0]
        self.text.tag_add("curline", f"{line}.0", f"{line}.end+1c")

    def _paint_brackets(self) -> None:
        self.text.tag_remove("matchbr", "1.0", "end")
        lex = self._current_lex()
        if not lex.pairs:
            return
        offset = self._offset_of("insert")
        for candidate in (offset - 1, offset):
            if candidate in lex.pairs:
                other = lex.pairs[candidate]
                for spot in (candidate, other):
                    self.text.tag_add(
                        "matchbr",
                        f"1.0+{spot}c",
                        f"1.0+{spot + 1}c",
                    )
                return

    def _paint_same_word(self) -> None:
        self.text.tag_remove("same", "1.0", "end")
        src = self.get_text()
        offset = self._offset_of("insert")
        if offset <= 0:
            return
        match = WORD_RE.search(src, max(0, offset - 60), offset)
        if match is None or match.end() != offset:
            return
        word = match.group(0)
        if len(word) < 2:
            return
        pattern = rf"\b{re.escape(word)}\b"
        start = "1.0"
        found = 0
        while found < 200:
            index = self.text.search(pattern, start, stopindex="end", regexp=True)
            if not index:
                break
            end = f"{index}+{len(word)}c"
            self.text.tag_add("same", index, end)
            start = end
            found += 1

    def clear_find_tags(self) -> None:
        """Remove find highlights."""
        for tag in FIND_TAGS:
            self.text.tag_remove(tag, "1.0", "end")

    def add_find_tag(self, start: str, end: str, current: bool = False) -> None:
        """Highlight one search match."""
        self.text.tag_add("find_cur" if current else "find", start, end)

    # ---------------------------------------------------------------- helpers
    def _draw_gutter(self) -> None:
        if not self.show_numbers or not self.gutter.winfo_exists():
            return
        self.gutter.delete("all")
        digits = max(3, len(str(self.text.index("end-1c").split(".")[0])))
        width = digits * self._gutter_font.measure("0") + px(self, 22)
        self.gutter.configure(width=width)
        current = int(self.text.index("insert").split(".")[0])
        first = self.text.index("@0,0")
        last = self.text.index(f"@0,{max(1, self.text.winfo_height())}")
        first_line = int(first.split(".")[0])
        last_line = int(last.split(".")[0]) + 1
        for number in range(first_line, last_line + 1):
            info = self.text.dlineinfo(f"{number}.0")
            if info is None:
                continue
            x, y, _width, height = info[:4]
            if number == current:
                self.gutter.create_rectangle(
                    0,
                    y,
                    width,
                    y + height,
                    fill=self.theme.current_line,
                    outline="",
                )
            self.gutter.create_text(
                width - px(self, 9),
                y + height // 2,
                anchor="e",
                text=str(number),
                fill=(
                    self.theme.gutter_current
                    if number == current
                    else self.theme.gutter_text
                ),
                font=self._gutter_font,
            )

    def _offset_of(self, index: str) -> int:
        try:
            count = self.text.count("1.0", index, "chars")
        except tk.TclError:
            return 0
        if not count:
            return 0
        return int(count[0] or 0)

    def _line_bounds(self) -> tuple[int, int]:
        if self.text.tag_ranges("sel"):
            start = self.text.index("sel.first")
            end = self.text.index("sel.last")
            first = int(start.split(".")[0])
            last = int(end.split(".")[0])
            if end.split(".")[1] == "0":
                last = max(first, last - 1)
            return first, last
        line = int(self.text.index("insert").split(".")[0])
        return line, line

    # -------------------------------------------------------------- edit keys
    def _on_keypress(self, event: tk.Event) -> str | None:
        vim_result = self.vim.handle(event)
        if vim_result is not None:
            return vim_result
        state = event.state if isinstance(event.state, int) else 0
        if state & 0x4:
            return None
        char = event.char
        if not char:
            return None
        if char in CLOSERS and self.text.get("insert") == char:
            self.text.mark_set("insert", "insert+1c")
            return "break"
        ranges = self.selection_ranges()
        if ranges:
            if char in "([{" and len(ranges) == 1:
                selected = self.text.get(ranges[0][0], ranges[0][1])
                self.text.delete(ranges[0][0], ranges[0][1])
                self.text.insert("insert", char + selected + PAIRS[char])
                return "break"
            if len(ranges) > 1:
                self.replace_selections(char)
                return "break"
            return None
        offset = self._offset_of("insert")
        if is_in_string_or_comment(self._current_lex().tokens, offset):
            return None
        following = self.text.get("insert")
        safe = following == "" or following.isspace() or following in ")]},;"
        if char in PAIRS and safe:
            self.text.insert("insert", char + PAIRS[char])
            self.text.mark_set("insert", "insert-1c")
            return "break"
        return None

    def _on_backspace(self, _event: tk.Event) -> str | None:
        if self.vim.handle_special("BackSpace"):
            return "break"
        if len(self.selection_ranges()) > 1:
            self.replace_selections("")
            return "break"
        if self.text.tag_ranges("sel"):
            self.text.delete("sel.first", "sel.last")
            return "break"
        before = self.text.get("insert-1c", "insert")
        after = self.text.get("insert", "insert+1c")
        if before in PAIRS and PAIRS[before] == after:
            self.text.delete("insert-1c", "insert+1c")
            return "break"
        return None

    def _on_return(self, _event: tk.Event) -> str:
        if self.vim.handle_special("Return"):
            return "break"
        if self.completion_hook is not None and self.completion_hook("accept"):
            return "break"
        line_start = self.text.index("insert linestart")
        line_end = self.text.index("insert lineend")
        content = self.text.get(line_start, line_end)
        indent = re.match(r"[ \t]*", content)
        base = indent.group(0) if indent else ""
        next_indent = auto_indent(content)
        before = self.text.get(line_start, "insert")
        after = self.text.get("insert", line_end)
        line = int(self.text.index("insert").split(".")[0])
        stripped = before.rstrip()
        if stripped.endswith(("(", "[", "{")):
            closer = {"(": ")", "[": "]", "{": "}"}[stripped[-1]]
            if after.lstrip().startswith(closer):
                self.text.insert("insert", "\n" + next_indent + "\n" + base)
                self.text.mark_set("insert", f"{line + 1}.{len(next_indent)}")
                self._notify_return()
                return "break"
        self.text.insert("insert", "\n" + next_indent)
        self._notify_return()
        return "break"

    def _notify_return(self) -> None:
        """Let the host react to a completed line (template auto-pairing)."""
        if self.after_return is None:
            return
        try:
            self.after(0, self.after_return)
        except tk.TclError:
            return

    def _on_tab(self, _event: tk.Event) -> str:
        if self.vim.handle_special("Tab"):
            return "break"
        if self.completion_hook is not None and self.completion_hook("accept"):
            return "break"
        if self.text.tag_ranges("sel"):
            self._indent_lines(1)
        else:
            self.text.insert("insert", INDENT)
        return "break"

    def _on_dedent(self, _event: tk.Event) -> str:
        if self.vim.handle_special("Tab"):
            return "break"
        self._indent_lines(-1)
        return "break"

    def _indent_lines(self, direction: int) -> None:
        first, last = self._line_bounds()
        for line in range(first, last + 1):
            if direction > 0:
                self.text.insert(f"{line}.0", INDENT)
            else:
                head = self.text.get(f"{line}.0", f"{line}.0+2c")
                strip = 0
                while strip < len(head) and head[strip] in " \t":
                    strip += 1
                if strip:
                    self.text.delete(f"{line}.0", f"{line}.0+{strip}c")
        self.text.tag_remove("sel", "1.0", "end")
        self.text.tag_add("sel", f"{first}.0", f"{last}.end")
        self._on_cursor_moved()

    def _on_toggle_comment(self, _event: tk.Event | None = None) -> str:
        first, last = self._line_bounds()
        modes = self._current_lex().line_modes
        start_mode = modes[first - 1] if first - 1 < len(modes) else "lua"
        lines = [
            self.text.get(f"{line}.0", f"{line}.end")
            for line in range(first, last + 1)
        ]
        changed = toggle_comment_lines(lines, start_mode)
        joined = "\n".join(changed)
        self.text.edit_separator()
        self.text.replace(f"{first}.0", f"{last}.end", joined)
        self.text.edit_separator()
        self.text.tag_remove("sel", "1.0", "end")
        self.text.tag_add("sel", f"{first}.0", f"{last}.end")
        self._on_cursor_moved()
        return "break"

    def _on_duplicate(self, _event: tk.Event) -> str:
        start = self.text.index("insert linestart")
        end = self.text.index("insert lineend")
        line = self.text.get(start, end)
        self.text.insert(end, "\n" + line)
        self.text.mark_set("insert", self.text.index(f"{end}+1c lineend"))
        return "break"

    def _on_autocomplete(self, _event: tk.Event) -> str:
        if self._on_complete is not None:
            self._on_complete(True)
        return "break"
