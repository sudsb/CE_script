"""A compact nvim-like modal editing layer for the Tk text editor.

Only the editor's own ``tk.Text`` widget is used, so the feature stays
portable. The controller owns the mode state; ``CodeEditor`` merely forwards
key events to it and supplies a few host callbacks (status, ``:`` commands).
"""

from __future__ import annotations

import tkinter as tk
from collections.abc import Callable
from typing import TYPE_CHECKING

from .ui import px

if TYPE_CHECKING:
    from .editor import CodeEditor

WORD = r"\m[A-Za-z0-9_$]+\M"

MODES = {
    "normal": "普通模式",
    "insert": "插入模式",
    "visual": "可视模式",
    "visual_line": "可视行模式",
}


class VimController:
    """Normal / insert / visual modes plus the most common nvim keys."""

    def __init__(self, editor: CodeEditor) -> None:
        self.editor = editor
        self.enabled = False
        self.state = "insert"
        self.count = ""
        self.pending = ""
        self.command = ""
        self.anchor = "1.0"
        self.register = ""
        self.linewise = False
        self.on_state: Callable[[str], None] | None = None
        self.on_command: Callable[[str], bool] | None = None
        self.on_find: Callable[[], None] | None = None

    # ------------------------------------------------------------------ state
    @property
    def text(self) -> tk.Text:
        return self.editor.text

    def active(self) -> bool:
        """Whether key events must be intercepted (not plain insert mode)."""
        return self.enabled and self.state != "insert"

    def set_enabled(self, enabled: bool) -> None:
        self.enabled = bool(enabled)
        self._set_state("normal" if enabled else "insert", reset=True)

    def _set_state(self, state: str, reset: bool = True) -> None:
        previous = self.state
        self.state = state
        if reset:
            self.count = ""
            self.pending = ""
            self.command = ""
        if state == "insert":
            self.text.tag_remove("sel", "1.0", "end")
        elif (
            previous == "insert"
            and state == "normal"
            and self.text.compare("insert", ">", "insert linestart")
        ):
            self.text.mark_set("insert", "insert-1c")
        try:
            width = 8 if state == "normal" else 2
            self.text.configure(insertwidth=px(self.editor, width))
        except tk.TclError:
            pass
        self._notify()

    def _notify(self) -> None:
        if self.on_state is None:
            return
        label = MODES.get(self.state, self.state)
        if self.command:
            display = self.command
        elif self.count:
            display = f"{label} · {self.count}"
        elif self.pending:
            display = f"{label} · {self.pending}"
        else:
            display = label if self.enabled else ""
        self.on_state(display)

    def _count(self) -> int:
        value = int(self.count) if self.count.isdigit() else 1
        self.count = ""
        return max(1, min(value, 999))

    # ------------------------------------------------------------- entry points
    def handle(self, event: tk.Event) -> str | None:
        """Handle a ``<KeyPress>``; return ``break`` when the key is consumed."""
        if not self.enabled:
            return None
        if self.state == "insert":
            return self._handle_insert_key(event)
        return self._handle_key(event)

    def handle_escape(self) -> bool:
        if not self.enabled:
            return False
        if self.state == "normal" and not (self.command or self.count or self.pending):
            return False
        self.text.tag_remove("sel", "1.0", "end")
        self._set_state("normal", reset=True)
        return True

    def scroll(self, lines: int) -> None:
        """Ctrl+D in normal mode: move down roughly half a screen."""
        self._move("j", max(1, lines))
        self._notify()

    def handle_special(self, keysym: str) -> bool:
        """Intercept Return / BackSpace / Tab outside insert mode."""
        if not self.active():
            return False
        if self.command:
            if keysym == "Return":
                self._run_command()
                return True
            if keysym in ("BackSpace", "Delete"):
                self.command = self.command[:-1] or ":"
                self._notify()
                return True
            return True
        if keysym == "Return":
            self._move("j", self._count())
            return True
        if keysym in ("BackSpace", "Delete"):
            self._move("h", self._count())
            return True
        return True

    def _handle_insert_key(self, event: tk.Event) -> str | None:
        if event.keysym == "Escape":
            self._set_state("normal", reset=True)
            return "break"
        return None

    # --------------------------------------------------------------- normal keys
    def _handle_key(self, event: tk.Event) -> str:
        state = event.state if isinstance(event.state, int) else 0
        char = event.char or ""
        keysym = event.keysym or ""

        if state & 0x4 and char.casefold() == "r":
            self._redo()
            self._notify()
            return "break"

        if self.command:
            if keysym == "Escape":
                self.command = ""
                self._set_state("normal", reset=True)
            elif keysym == "Return":
                self._run_command()
            elif keysym in ("BackSpace", "Delete"):
                self.command = self.command[:-1] or ":"
                self._notify()
            elif char and char.isprintable():
                self.command += char
                self._notify()
            return "break"

        if char in "123456789" or (char == "0" and self.count):
            self.count += char
            self._notify()
            return "break"

        if self.pending:
            pending, self.pending = self.pending, ""
            if pending == "g" and char == "g":
                self.text.mark_set("insert", "1.0")
                self._see()
            elif pending == "d" and char == "d":
                self._delete_lines(self._count())
            elif pending == "y" and char == "y":
                self._yank_lines(self._count())
            elif pending == ">" and char == ">":
                self._indent_line(1)
            elif pending == "<" and char == "<":
                self._indent_line(-1)
            else:
                self.pending = ""
            self.count = ""
            self._notify()
            return "break"

        if char in ("d", "y", "g", ">", "<"):
            self.pending = char
            self._notify()
            return "break"

        if keysym == "Escape":
            self.text.tag_remove("sel", "1.0", "end")
            self._set_state("normal", reset=True)
            return "break"

        if char == ":":
            self.command = ":"
            self._notify()
            return "break"
        if char == "/":
            if self.on_find is not None:
                self.on_find()
            return "break"

        if char in ("h", "j", "k", "l", "w", "b", "0", "$", "G"):
            self._move(char if char != "$" else "end", self._count())
            if self.state in ("visual", "visual_line"):
                self._update_visual()
            self._notify()
            return "break"

        if self.state in ("visual", "visual_line"):
            if char == "y":
                self._yank_selection()
                self._set_state("normal", reset=True)
            elif char in ("d", "x"):
                self._delete_selection()
                self._set_state("normal", reset=True)
            elif char == ">":
                self._indent_selection(1)
            elif char == "<":
                self._indent_selection(-1)
            self._notify()
            return "break"

        if char == "u":
            self._undo()
        elif char == "x":
            self.text.delete("insert")
        elif char == "p":
            self._paste(after=True)
        elif char == "P":
            self._paste(after=False)
        elif char == "v":
            self.anchor = self.text.index("insert")
            self._set_state("visual", reset=True)
            self._update_visual()
        elif char == "V":
            self.anchor = self.text.index("insert")
            self._set_state("visual_line", reset=True)
            self._update_visual()
        elif char == "i":
            self._set_state("insert", reset=True)
        elif char == "a":
            self.text.mark_set("insert", "insert+1c")
            self._set_state("insert", reset=True)
        elif char == "I":
            self.text.mark_set("insert", "insert linestart")
            self._set_state("insert", reset=True)
        elif char == "A":
            self.text.mark_set("insert", "insert lineend")
            self._set_state("insert", reset=True)
        elif char == "o":
            self._open_line(below=True)
        elif char == "O":
            self._open_line(below=False)
        self._notify()
        return "break"

    # ----------------------------------------------------------------- motions
    def _move(self, key: str, count: int) -> None:
        text = self.text
        try:
            if key == "h":
                text.mark_set("insert", f"insert-{count}c")
            elif key == "l":
                text.mark_set("insert", f"insert+{count}c")
            elif key == "j":
                text.mark_set("insert", f"insert + {count} lines")
            elif key == "k":
                text.mark_set("insert", f"insert - {count} lines")
            elif key == "0":
                text.mark_set("insert", "insert linestart")
            elif key == "end":
                text.mark_set("insert", "insert lineend")
            elif key == "G":
                text.mark_set("insert", "end-1c linestart")
            elif key == "w":
                for _ in range(count):
                    found = text.search(WORD, "insert+1c", "end", regexp=True)
                    text.mark_set("insert", found or "insert lineend")
            elif key == "b":
                for _ in range(count):
                    found = text.search(
                        WORD, "insert-1c", "1.0", regexp=True, backwards=True
                    )
                    text.mark_set("insert", found or "insert linestart")
        except tk.TclError:
            return
        self._see()

    def _see(self) -> None:
        try:
            self.text.see("insert")
        except tk.TclError:
            return
        self.editor._on_cursor_moved()

    # ------------------------------------------------------------------ editing
    def _undo(self) -> None:
        try:
            self.text.edit_undo()
        except tk.TclError:
            return

    def _redo(self) -> None:
        try:
            self.text.edit_redo()
        except tk.TclError:
            return

    def _open_line(self, below: bool) -> None:
        text = self.text
        line = int(text.index("insert").split(".")[0])
        if below:
            text.insert(f"{line}.end", "\n")
            text.mark_set("insert", f"{line + 1}.0")
        else:
            text.insert(f"{line}.0", "\n")
            text.mark_set("insert", f"{line}.0")
        text.edit_separator()
        self._set_state("insert", reset=True)

    def _line_range(self, count: int) -> tuple[str, str]:
        text = self.text
        line = int(text.index("insert").split(".")[0])
        last = int(text.index("end-1c").split(".")[0])
        start = f"{line}.0"
        end_line = min(last + 1, line + count)
        return start, f"{end_line}.0"

    def _yank_lines(self, count: int) -> None:
        start, end = self._line_range(count)
        self.register = str(self.text.get(start, end))
        self.linewise = True
        self._copy_register()

    def _delete_lines(self, count: int) -> None:
        start, end = self._line_range(count)
        self.register = str(self.text.get(start, end))
        self.linewise = True
        self.text.edit_separator()
        self.text.delete(start, end)
        self.text.edit_separator()
        self._copy_register()

    def _yank_selection(self) -> None:
        self.register = self._selection_text()
        self.linewise = self.state == "visual_line"
        self._copy_register()

    def _delete_selection(self) -> None:
        self.register = self._selection_text()
        self.linewise = self.state == "visual_line"
        self.text.edit_separator()
        self.text.delete("sel.first", "sel.last")
        self.text.edit_separator()

    def _selection_text(self) -> str:
        ranges = self.text.tag_ranges("sel")
        if len(ranges) < 2:
            return ""
        return str(self.text.get(ranges[0], ranges[-1]))

    def _copy_register(self) -> None:
        try:
            self.text.clipboard_clear()
            self.text.clipboard_append(self.register)
        except tk.TclError:
            return

    def _paste(self, after: bool) -> None:
        payload = self.register
        if not payload:
            try:
                payload = str(self.text.clipboard_get())
            except tk.TclError:
                return
        text = self.text
        text.edit_separator()
        if self.linewise:
            line = int(text.index("insert").split(".")[0])
            target = f"{line + 1}.0" if after else f"{line}.0"
            text.insert(target, payload)
        else:
            target = "insert+1c" if after else "insert"
            text.insert(target, payload)
        text.edit_separator()

    def _indent_line(self, direction: int) -> None:
        self.editor._indent_lines(direction)

    def _indent_selection(self, direction: int) -> None:
        self.editor._indent_lines(direction)
        self._set_state("normal", reset=True)

    # ------------------------------------------------------------------ visual
    def _update_visual(self) -> None:
        text = self.text
        anchor = self.anchor
        caret = text.index("insert")
        try:
            if self.state == "visual_line":
                first, last = sorted(
                    (int(anchor.split(".")[0]), int(caret.split(".")[0]))
                )
                start, end = f"{first}.0", f"{last}.end+1c"
            elif text.compare(anchor, ">", caret):
                start, end = caret, f"{anchor}+1c"
            else:
                start, end = anchor, f"{caret}+1c"
            text.tag_remove("sel", "1.0", "end")
            text.tag_add("sel", start, end)
        except (tk.TclError, ValueError):
            return
        text.tag_raise("sel")

    # ----------------------------------------------------------------- commands
    def _run_command(self) -> None:
        command = self.command.lstrip(":").strip()
        self.command = ""
        self._set_state("normal", reset=True)
        if self.on_command is not None and command:
            self.on_command(command)
