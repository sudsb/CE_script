"""In-editor find and replace bar."""

from __future__ import annotations

import re
import tkinter as tk

from .editor import CodeEditor
from .themes import Theme
from .ui import HoverButton, px, ui_font


class FindBar(tk.Frame):
    """A compact find/replace strip that highlights every match."""

    def __init__(
        self,
        master: tk.Misc,
        editor: CodeEditor,
        theme: Theme,
    ) -> None:
        super().__init__(master, bd=0)
        self.configure(padx=px(self, 10), pady=px(self, 6))
        self.editor = editor
        self.theme = theme
        self.case_sensitive = False
        self.matches: list[tuple[int, int]] = []
        self.index = 0
        self._job: str | None = None
        self._reset_index = False
        self._build()
        self.apply_theme(theme)

    # ----------------------------------------------------------------- build
    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        self.find_var = tk.StringVar()
        self.replace_var = tk.StringVar()
        self.find_entry = tk.Entry(self, textvariable=self.find_var, bd=0)
        self.replace_entry = tk.Entry(self, textvariable=self.replace_var, bd=0)
        self.case_button = HoverButton(self, "Aa", self._toggle_case)
        self.count_label = tk.Label(self, text="0/0", anchor="e")
        self.prev_button = HoverButton(self, "↑", self.find_previous)
        self.next_button = HoverButton(self, "↓", self.find_next)
        self.close_button = HoverButton(self, "×", self.hide)
        self.replace_button = HoverButton(
            self, "替换", self.replace_current, kind="primary"
        )
        self.replace_all_button = HoverButton(
            self, "全部替换", self.replace_all
        )
        self.find_label = tk.Label(self, text="查找")
        self.replace_label = tk.Label(self, text="替换")

        gap = px(self, 4)
        self.find_label.grid(row=0, column=0, sticky="w", padx=(0, px(self, 6)))
        self.find_entry.grid(row=0, column=1, sticky="ew", ipady=px(self, 3))
        self.case_button.grid(row=0, column=2, padx=px(self, 2))
        self.count_label.grid(
            row=0, column=3, padx=(px(self, 6), px(self, 4))
        )
        self.prev_button.grid(row=0, column=4)
        self.next_button.grid(row=0, column=5)
        self.close_button.grid(row=0, column=6, padx=(gap, 0))
        self.replace_label.grid(
            row=1, column=0, sticky="w", padx=(0, px(self, 6)), pady=(gap, 0)
        )
        self.replace_entry.grid(
            row=1, column=1, sticky="ew", ipady=px(self, 3), pady=(gap, 0)
        )
        self.replace_button.grid(
            row=1, column=2, columnspan=2, padx=px(self, 2), pady=(gap, 0)
        )
        self.replace_all_button.grid(
            row=1, column=4, columnspan=3, sticky="ew", pady=(gap, 0)
        )

        self.find_entry.bind("<Return>", lambda _event: self.find_next() or "break")
        self.find_entry.bind(
            "<Shift-Return>", lambda _event: self.find_previous() or "break"
        )
        self.find_entry.bind("<Escape>", lambda _event: self.hide() or "break")
        self.find_entry.bind("<KeyRelease>", self._on_query_changed)
        self.replace_entry.bind(
            "<Return>", lambda _event: self.replace_current() or "break"
        )
        self.replace_entry.bind("<Escape>", lambda _event: self.hide() or "break")

    # ---------------------------------------------------------------- theming
    def apply_theme(self, theme: Theme) -> None:
        """Recolour the find bar."""
        self.theme = theme
        self.configure(bg=theme.panel)
        for label in (self.find_label, self.replace_label):
            label.configure(
                bg=theme.panel, fg=theme.muted, font=ui_font(9)
            )
        self.count_label.configure(
            bg=theme.panel, fg=theme.muted, font=ui_font(9)
        )
        for entry in (self.find_entry, self.replace_entry):
            entry.configure(
                bg=theme.surface,
                fg=theme.text,
                insertbackground=theme.caret,
                selectbackground=theme.sel_bg,
                selectforeground=theme.text,
                font=ui_font(10),
                relief="flat",
                highlightthickness=1,
                highlightbackground=theme.border,
                highlightcolor=theme.accent,
            )
        for button in (
            self.case_button,
            self.prev_button,
            self.next_button,
            self.close_button,
            self.replace_button,
            self.replace_all_button,
        ):
            button.apply_theme(theme)
        self._paint_case()

    def _paint_case(self) -> None:
        self.case_button.configure(
            fg=self.theme.accent if self.case_sensitive else self.theme.muted
        )

    # -------------------------------------------------------------- visibility
    @property
    def visible(self) -> bool:
        """Return whether the bar is mapped."""
        return bool(self.winfo_manager())

    def show(self, prefill: bool = True) -> None:
        """Show the bar and focus its search field."""
        self.grid()
        if prefill and self.editor.text.tag_ranges("sel"):
            selected = self.editor.text.get("sel.first", "sel.last")
            if selected and "\n" not in selected:
                self.find_var.set(selected)
        self.find_entry.focus_set()
        self.find_entry.select_range(0, "end")
        self.refresh()

    def hide(self) -> None:
        """Hide the bar and remove highlights."""
        self.editor.clear_find_tags()
        self.grid_remove()
        self.editor.text.focus_set()

    def focus_find(self) -> None:
        """Focus the find field, showing the bar when needed."""
        if not self.visible:
            self.show()
        else:
            self.find_entry.focus_set()
            self.find_entry.select_range(0, "end")

    # ----------------------------------------------------------------- search
    def _pattern(self) -> re.Pattern[str] | None:
        query = self.find_var.get()
        if not query:
            return None
        flags = 0 if self.case_sensitive else re.IGNORECASE
        return re.compile(re.escape(query), flags)

    def _toggle_case(self) -> None:
        self.case_sensitive = not self.case_sensitive
        self._paint_case()
        self.index = 0
        self.refresh()

    def _schedule_refresh(self, delay: int = 120) -> None:
        self.cancel_pending()
        try:
            self._job = self.after(delay, self._refresh_from_typing)
        except tk.TclError:
            self._job = None

    def cancel_pending(self) -> None:
        """Cancel the debounced refresh, e.g. while the window is closing."""
        if self._job is not None:
            try:
                self.after_cancel(self._job)
            except tk.TclError:
                pass
            self._job = None

    def _on_query_changed(self, _event: tk.Event) -> None:
        self._schedule_refresh(120)

    def _refresh_from_typing(self) -> None:
        self._job = None
        self._reset_index = True
        self.refresh()
        self._reset_index = False

    def on_editor_change(self) -> None:
        """Re-collect matches shortly after the document changes."""
        if self.visible:
            self._schedule_refresh(60)

    def refresh(self) -> None:
        """Rebuild match highlights and the counter."""
        if not self.winfo_exists():
            return
        self.editor.clear_find_tags()
        if getattr(self, "_reset_index", False):
            self.index = 0
        self.matches = []
        pattern = self._pattern()
        if pattern is None:
            self.count_label.configure(text="0/0")
            return
        text = self.editor.get_text()
        self.matches = [(match.start(), match.end()) for match in pattern.finditer(text)]
        if not self.matches:
            self.count_label.configure(text="0/0")
            return
        self.index = max(0, min(self.index, len(self.matches) - 1))
        for number, (start, end) in enumerate(self.matches):
            self.editor.add_find_tag(
                f"1.0+{start}c",
                f"1.0+{end}c",
                current=number == self.index,
            )
        self.count_label.configure(
            text=f"{self.index + 1}/{len(self.matches)}"
        )

    def _caret_offset(self) -> int:
        return self.editor._offset_of("insert")

    def find_next(self) -> None:
        """Select the next match after the caret."""
        self.refresh()
        if not self.matches:
            self.bell()
            return
        caret = self._caret_offset()
        chosen = None
        for number, (start, _end) in enumerate(self.matches):
            if start >= caret:
                chosen = number
                break
        self.index = 0 if chosen is None else chosen
        self._select_current()

    def find_previous(self) -> None:
        """Select the previous match before the caret."""
        self.refresh()
        if not self.matches:
            self.bell()
            return
        caret = self._caret_offset()
        chosen = None
        for number in range(len(self.matches) - 1, -1, -1):
            if self.matches[number][1] < caret:
                chosen = number
                break
        self.index = len(self.matches) - 1 if chosen is None else chosen
        self._select_current()

    def _select_current(self) -> None:
        if not self.matches:
            return
        self.index = max(0, min(self.index, len(self.matches) - 1))
        start, end = self.matches[self.index]
        self.editor.clear_find_tags()
        for number, (other_start, other_end) in enumerate(self.matches):
            self.editor.add_find_tag(
                f"1.0+{other_start}c",
                f"1.0+{other_end}c",
                current=number == self.index,
            )
        first = f"1.0+{start}c"
        last = f"1.0+{end}c"
        self.editor.text.tag_remove("sel", "1.0", "end")
        self.editor.text.tag_add("sel", first, last)
        self.editor.text.mark_set("insert", last)
        self.editor.text.see(first)
        self.count_label.configure(
            text=f"{self.index + 1}/{len(self.matches)}"
        )

    # -------------------------------------------------------------- replacing
    def replace_current(self) -> None:
        """Replace the current match, then jump to the next one."""
        self.refresh()
        if not self.matches:
            self.bell()
            return
        self.index = max(0, min(self.index, len(self.matches) - 1))
        start, end = self.matches[self.index]
        replacement = self.replace_var.get()
        self.editor.text.delete(f"1.0+{start}c", f"1.0+{end}c")
        self.editor.text.insert(f"1.0+{start}c", replacement)
        caret = start + len(replacement)
        self.refresh()
        self.index = 0
        for number, (other_start, _other_end) in enumerate(self.matches):
            if other_start >= caret:
                self.index = number
                break
        self._select_current()

    def replace_all(self) -> None:
        """Replace every match in the document."""
        pattern = self._pattern()
        if pattern is None:
            return
        source = self.editor.get_text()
        replacement = self.replace_var.get()
        updated, count = pattern.subn(lambda _match: replacement, source)
        if count == 0:
            self.bell()
            return
        self.editor.text.edit_separator()
        self.editor.text.replace("1.0", "end-1c", updated)
        self.editor.text.edit_separator()
        self.count_label.configure(text=f"{count} 处已替换")
        self.refresh()
