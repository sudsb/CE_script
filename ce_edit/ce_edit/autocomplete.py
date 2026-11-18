"""Completion popup for the code editor."""

from __future__ import annotations

import re
import tkinter as tk
from collections.abc import Callable

from .completions import Completion, filter_completions
from .editor import CodeEditor
from .lexer import is_in_string_or_comment
from .themes import Theme
from .ui import clamp_rect, glass_palette, px, ui_font, work_area

TRIGGER_RE = re.compile(r"[\[{]?\$?[A-Za-z_]")

# A small amount of real translucency reads as frosted glass without making
# the completion text hard to read on a high-DPI display.
GLASS_ALPHA = 0.96
FADE_ALPHA = 0.80


class AutoComplete:
    """A lightweight, glass-styled completion popup driven by the caret prefix."""

    def __init__(
        self,
        editor: CodeEditor,
        on_accept: Callable[[Completion], None] | None = None,
        keywords_provider: Callable[[], list[str]] | None = None,
        argument_provider: Callable[[str], str | None] | None = None,
        expansion_provider: Callable[[str], str | None] | None = None,
    ) -> None:
        self.editor = editor
        self.on_accept = on_accept
        self.keywords_provider = keywords_provider
        self.argument_provider = argument_provider
        self.expansion_provider = expansion_provider
        self.popup: tk.Toplevel | None = None
        self.listbox: tk.Listbox | None = None
        self.detail: tk.Label | None = None
        self.items: list[Completion] = []
        self._fade = 0
        self._fade_job: str | None = None
        self._alpha_supported = True
        self._theme: Theme = editor.theme
        editor.completion_hook = self._hook
        text = editor.text
        text.bind("<Up>", self._on_up, add="+")
        text.bind("<Down>", self._on_down, add="+")
        text.bind("<Escape>", self._on_escape, add="+")
        for sequence in ("<Left>", "<Right>", "<Home>", "<End>", "<Prior>", "<Next>"):
            text.bind(sequence, self._on_move, add="+")
        # A stale popup that stays anchored to an old caret is worse than no
        # popup at all: dismiss it on caret moves and scrolling. Never destroy
        # a Toplevel on <Button-1> -- that can abort an in-progress mouse drag
        # and leave ``sel`` empty while the caret appears to move.
        text.bind("<ButtonRelease-1>", self._on_move, add="+")
        text.bind("<Button-3>", self._on_move, add="+")
        text.bind("<MouseWheel>", self._on_move, add="+")
        for sequence in ("<Button-4>", "<Button-5>"):
            text.bind(sequence, self._on_move, add="+")

    # ------------------------------------------------------------------ state
    @property
    def visible(self) -> bool:
        """Return whether the popup is currently shown."""
        return self.popup is not None and bool(self.popup.winfo_exists())

    def on_text_changed(self) -> None:
        """Refresh or dismiss the popup after the document changed."""
        if self.visible:
            self.request(force=True)
        else:
            self.request(force=False)

    def request(self, force: bool = False) -> None:
        """Open or refresh the popup for the current caret prefix."""
        if not self.editor.winfo_exists() or not self.editor.text.winfo_exists():
            self.hide()
            return
        if self.editor.text.tag_ranges("sel"):
            self.hide()
            return
        prefix = self.editor.caret_prefix()
        if not force and not TRIGGER_RE.match(prefix):
            self.hide()
            return
        mode = self.editor.document_mode()
        if not force:
            lex = self.editor._lex
            offset = self.editor._offset_of("insert")
            if lex is not None and is_in_string_or_comment(lex.tokens, offset):
                self.hide()
                return
        keywords: list[str] = []
        if self.keywords_provider is not None:
            try:
                keywords = self.keywords_provider()
            except (tk.TclError, ValueError):
                keywords = []
        items = filter_completions(prefix, mode, limit=60, extra=keywords)
        if not items:
            self.hide()
            return
        self._show(items)

    def reposition(self) -> None:
        """Re-anchor the popup after the main window moved or was resized."""
        if self.visible:
            try:
                self._place()
            except tk.TclError:
                self.hide()

    def cancel_pending(self) -> None:
        """Close the popup and cancel its fade animation before teardown."""
        self.hide()

    # ------------------------------------------------------------------ popup
    def _show(self, items: list[Completion]) -> None:
        theme = self.editor.theme
        rebuilding = not self.visible or self._theme is not theme
        if rebuilding:
            self._destroy()
            self._build(theme)
        assert self.listbox is not None
        assert self.detail is not None
        self.items = items
        self._theme = theme
        self.listbox.delete(0, "end")
        for item in items:
            self.listbox.insert("end", f"{item.name}   ·   {item.category}")
        self.listbox.configure(height=min(12, len(items)))
        self.listbox.selection_clear(0, "end")
        self.listbox.selection_set(0)
        self.listbox.activate(0)
        self.detail.configure(text=items[0].signature)
        self._place()
        if rebuilding:
            self._start_fade()

    def _build(self, theme: Theme) -> None:
        popup = tk.Toplevel(self.editor)
        popup.wm_overrideredirect(True)
        try:
            popup.attributes("-topmost", True)
        except tk.TclError:
            pass
        glass = glass_palette(theme)
        # Three progressively lighter rings approximate a soft shadow; the
        # tinted body plus the whole-window alpha give the frosted look.
        outer = tk.Frame(popup, bg=glass.shadow)
        outer.pack(fill="both", expand=True, padx=px(popup, 3), pady=px(popup, 3))
        edge = tk.Frame(outer, bg=glass.edge)
        edge.pack(fill="both", expand=True, padx=px(popup, 1), pady=px(popup, 1))
        shell = tk.Frame(edge, bg=glass.border)
        shell.pack(fill="both", expand=True, padx=px(popup, 1), pady=px(popup, 1))
        body = tk.Frame(shell, bg=glass.body)
        body.pack(fill="both", expand=True)
        tk.Frame(body, height=px(popup, 1), bg=glass.highlight).pack(fill="x")
        self.listbox = tk.Listbox(
            body,
            bd=0,
            highlightthickness=0,
            activestyle="none",
            exportselection=False,
            takefocus=0,
            font=(self.editor._family, self.editor.font_size),
            bg=glass.list_bg,
            fg=theme.text,
            selectbackground=glass.select,
            selectforeground=theme.text,
        )
        self.listbox.pack(fill="both", expand=True, padx=px(popup, 2), pady=(px(popup, 2), 0))
        self.detail = tk.Label(
            body,
            text="",
            anchor="w",
            justify="left",
            font=ui_font(9),
            bg=glass.detail,
            fg=theme.muted,
            padx=px(popup, 8),
            pady=px(popup, 4),
        )
        self.detail.pack(fill="x")
        self.listbox.bind("<<ListboxSelect>>", self._on_select)
        self.listbox.bind("<ButtonRelease-1>", self._on_click)
        self.popup = popup

    def _place(self) -> None:
        """Place the popup under the caret, flipping and shrinking as needed."""
        assert self.popup is not None
        assert self.listbox is not None
        popup = self.popup
        text = self.editor.text
        try:
            bbox = text.bbox("insert")
            if bbox and len(bbox) >= 4:
                caret_x = int(text.winfo_rootx()) + int(bbox[0])
                caret_y = int(text.winfo_rooty()) + int(bbox[1])
                caret_height = int(bbox[3])
            else:
                # The caret can be scrolled out of view; never leave the
                # popup at the window manager's default (0, 0) position.
                caret_x = int(text.winfo_rootx()) + px(popup, 12)
                caret_y = int(text.winfo_rooty()) + px(popup, 12)
                caret_height = px(popup, 16)
        except (tk.TclError, ValueError, TypeError):
            self.hide()
            return

        gap = px(popup, 4)
        margin = px(popup, 8)
        area = work_area(popup, caret_x, caret_y)
        left, top, right, bottom = area
        popup.update_idletasks()
        width = max(px(popup, 360), self.listbox.winfo_reqwidth() + px(popup, 16))
        width = min(width, max(px(popup, 220), right - left - 2 * margin))
        height = popup.winfo_reqheight()

        below = bottom - (caret_y + caret_height + gap) - margin
        above = caret_y - gap - top - margin
        place_above = below < height and above > below
        available = int(above if place_above else below)
        if 0 < available < height:
            rows = max(1, int(self.listbox.cget("height") or 1))
            row_height = max(1, self.listbox.winfo_reqheight() // rows)
            chrome = max(0, height - self.listbox.winfo_reqheight())
            fit_rows = max(
                1, min(len(self.items), (max(1, available) - chrome) // row_height)
            )
            if fit_rows < rows:
                self.listbox.configure(height=fit_rows)
                popup.update_idletasks()
                height = popup.winfo_reqheight()

        y = caret_y - height - gap if place_above else caret_y + caret_height + gap
        x, y = clamp_rect(caret_x, y, width, height, area, margin)
        try:
            popup.geometry(f"{width}x{height}+{x}+{y}")
        except tk.TclError:
            self.hide()

    def _start_fade(self) -> None:
        popup = self.popup
        # ``_place`` may have dismissed the popup when the caret was unusable.
        if popup is None or not self._alpha_supported:
            return
        try:
            popup.attributes("-alpha", FADE_ALPHA)
        except tk.TclError:
            self._alpha_supported = False
            return
        self._fade = 0

        def step() -> None:
            if self.popup is not popup or not popup.winfo_exists():
                self._fade_job = None
                return
            self._fade += 1
            alpha = min(GLASS_ALPHA, FADE_ALPHA + self._fade * 0.08)
            try:
                popup.attributes("-alpha", alpha)
            except tk.TclError:
                self._alpha_supported = False
                self._fade_job = None
                return
            if alpha < GLASS_ALPHA:
                self._fade_job = popup.after(16, step)
            else:
                self._fade_job = None

        self._fade_job = popup.after(8, step)

    def hide(self) -> None:
        """Dismiss the popup, if visible."""
        self._destroy()

    def _destroy(self) -> None:
        if self.popup is not None:
            if self._fade_job is not None:
                try:
                    self.popup.after_cancel(self._fade_job)
                except tk.TclError:
                    pass
            try:
                self.popup.destroy()
            except tk.TclError:
                pass
        self._fade_job = None
        self.popup = None
        self.listbox = None
        self.detail = None
        self.items = []

    # ------------------------------------------------------------------ input
    def _hook(self, command: str) -> bool:
        if command == "accept" and self.visible:
            self._accept()
            return True
        return False

    def _on_up(self, _event: tk.Event) -> str | None:
        if not self.visible:
            return None
        self._move(-1)
        return "break"

    def _on_down(self, _event: tk.Event) -> str | None:
        if not self.visible:
            return None
        self._move(1)
        return "break"

    def _on_escape(self, _event: tk.Event) -> str | None:
        if not self.visible:
            return None
        self.hide()
        return "break"

    def _on_move(self, _event: tk.Event) -> None:
        if self.visible:
            self.hide()

    def _move(self, delta: int) -> None:
        if self.listbox is None or self.detail is None or not self.items:
            return
        current = self.listbox.curselection()
        index = current[0] if current else 0
        index = (index + delta) % len(self.items)
        self.listbox.selection_clear(0, "end")
        self.listbox.selection_set(index)
        self.listbox.activate(index)
        self.listbox.see(index)
        self.detail.configure(text=self.items[index].signature)

    def _on_select(self, _event: tk.Event) -> None:
        if self.listbox is None or self.detail is None:
            return
        selection = self.listbox.curselection()
        if selection:
            self.detail.configure(text=self.items[selection[0]].signature)

    def _on_click(self, event: tk.Event) -> str:
        if self.listbox is not None:
            index = self.listbox.nearest(event.y)
            self._accept(index)
        return "break"

    def _accept(self, index: int | None = None) -> None:
        if not self.items:
            self.hide()
            return
        if index is None and self.listbox is not None:
            selection = self.listbox.curselection()
            index = selection[0] if selection else 0
        index = max(0, min(int(index or 0), len(self.items) - 1))
        item = self.items[index]
        editor = self.editor
        prefix = editor.caret_prefix()
        start = f"insert-{len(prefix)}c" if prefix else "insert"
        editor.text.delete(start, "insert")
        expansion: str | None = None
        if item.is_call and self.expansion_provider is not None:
            try:
                expansion = self.expansion_provider(item.name)
            except (tk.TclError, ValueError):
                expansion = None
        if expansion:
            # A single-line template bound to this function replaces both the
            # name and its parentheses, e.g. registersymbol(axcc).
            editor.text.insert("insert", expansion)
            editor.text.focus_set()
            editor.text.see("insert")
            self.hide()
            if self.on_accept is not None:
                self.on_accept(item)
            return
        editor.text.insert("insert", item.name)
        argument: str | None = None
        if item.is_call and self.argument_provider is not None:
            try:
                argument = self.argument_provider(item.name)
            except (tk.TclError, ValueError):
                argument = None
        if item.is_call:
            if argument:
                # Template-bound functions receive the resolved $ver$ value.
                editor.text.insert("insert", f"({argument})")
            elif editor.text.get("insert") == ")":
                editor.text.insert("insert", "(")
                editor.text.mark_set("insert", "insert+1c")
            else:
                editor.text.insert("insert", "()")
                editor.text.mark_set("insert", "insert-1c")
        editor.text.focus_set()
        editor.text.see("insert")
        self.hide()
        if self.on_accept is not None:
            self.on_accept(item)
