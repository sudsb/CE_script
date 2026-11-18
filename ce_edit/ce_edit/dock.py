"""Resizable bottom dock hosting a small terminal and a runtime log viewer.

The dock is a plain pack-managed frame with a draggable top sash; its two
panes live in a ``PanedWindow`` so their relative width can also be dragged.
Neither pane needs a third-party package, and all subprocess output is
marshalled back to the Tk thread through queues (never ``after`` from a
worker thread).
"""

from __future__ import annotations

import locale
import logging
import os
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
from collections.abc import Callable
from logging.handlers import QueueHandler
from pathlib import Path

from .editor import code_family
from .themes import Theme
from .ui import px, ui_font

LOGGER_NAME = "ce_edit"
COMMAND_TIMEOUT = 15
MAX_OUTPUT = 20000
MAX_LINES = 800

# Log records whose level maps to a colour tag in the log pane.
LOG_LEVEL_TAGS = {
    logging.ERROR: "log_error",
    logging.WARNING: "log_warning",
    logging.INFO: "log_info",
    logging.DEBUG: "log_debug",
}

# Substrings that mark a terminal line as an error/warning regardless of level.
_TERM_ERROR_HINTS_ASCII = ("error", "fail", "exception", "traceback", "warning")
_TERM_ERROR_HINTS_CJK = ("失败", "错误", "异常", "报错", "警告", "警告")


def _warn_color(theme: Theme) -> str:
    """Amber that reads on both bundled themes."""
    return "#9a6700" if theme.key == "light" else "#e3b341"


HELP = (
    "可用命令：\n"
    "  help              显示本帮助\n"
    "  cls / clear       清空终端\n"
    "  pwd               显示当前目录\n"
    "  cd <目录>         切换工作目录\n"
    "  open <文件>       在编辑器中打开文件\n"
    "  exit / quit       关闭底部面板\n"
    "  其它命令交给系统 shell 执行（超时 15 秒）。\n"
)


class BottomDock:
    """Terminal + log panels docked above the status bar."""

    def __init__(
        self,
        master: tk.Misc,
        theme: Theme,
        on_open_file: Callable[[str], None] | None = None,
        font_size: int = 12,
        on_font_change: Callable[[int], None] | None = None,
    ) -> None:
        self.master = master
        self.theme = theme
        self.on_open_file = on_open_file
        self.on_font_change = on_font_change
        self.font_size = max(8, min(30, int(font_size)))
        self.visible: set[str] = set()
        self.logger = logging.getLogger(LOGGER_NAME)
        self.logger.setLevel(logging.INFO)
        self.logger.propagate = False
        self._log_queue: queue.Queue[logging.LogRecord] = queue.Queue()
        self._results: queue.Queue[tuple[str, int]] = queue.Queue()
        self._handler = QueueHandler(self._log_queue)
        self._handler.setFormatter(logging.Formatter("%(message)s"))
        self.logger.addHandler(self._handler)
        self._poll_job: str | None = None
        self._busy = False
        self._cwd = os.getcwd()
        self._history: list[str] = []
        self._history_index = 0
        self._drag: tuple[int, int, int] | None = None
        self._height = px(master, 210)
        self._min_height = px(master, 90)
        self._max_height = px(master, 620)
        # Header labels + font-size controls, recoloured on theme change.
        self._header_labels: list[tk.Label] = []
        self._font_buttons: list[tk.Label] = []

        self.frame = tk.Frame(master, bd=0, height=self._height)
        self.frame.pack_propagate(False)
        self.handle = tk.Frame(
            self.frame, bd=0, height=px(master, 5), cursor="sb_v_double_arrow"
        )
        self.handle.pack(side="top", fill="x")
        self.paned = tk.PanedWindow(
            self.frame,
            orient="horizontal",
            bd=0,
            relief="flat",
            sashrelief="flat",
            sashwidth=px(master, 5),
            opaqueresize=True,
        )
        self.paned.pack(side="top", fill="both", expand=True)
        self._panes = {
            "terminal": self._build_terminal(),
            "log": self._build_log(),
        }
        self.handle.bind("<Button-1>", self._start_resize)
        self.handle.bind("<B1-Motion>", self._on_resize)
        self.handle.bind("<ButtonRelease-1>", self._end_resize)
        self.apply_theme(theme)
        self._poll_job = self.master.after(100, self._drain)

    # ------------------------------------------------------------------ build
    def _scrolled_text(self, parent: tk.Misc) -> tk.Text:
        holder = tk.Frame(parent, bd=0)
        holder.pack(side="top", fill="both", expand=True)
        text = tk.Text(
            holder,
            wrap="word",
            height=6,
            bd=0,
            highlightthickness=0,
            state="disabled",
            font=(code_family(), self.font_size),
            padx=px(self.master, 6),
            pady=px(self.master, 4),
        )
        scrollbar = tk.Scrollbar(holder, orient="vertical", command=text.yview)
        text.configure(yscrollcommand=scrollbar.set)
        text.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")
        return text

    def _make_header(self, parent: tk.Misc, title: str) -> tk.Label:
        """Title label + A-/A+ font-size controls in a horizontal bar."""
        bar = tk.Frame(parent, bd=0)
        bar.pack(side="top", fill="x")
        label = tk.Label(bar, text=f" {title} ", anchor="w")
        label.pack(side="left", fill="x", expand=True)
        self._header_labels.append(label)
        for caption, delta in (("A−", -1), ("A+", 1)):
            btn = tk.Label(bar, text=caption, anchor="e", cursor="hand2",
                           padx=px(self.master, 6))
            btn.pack(side="right")
            btn.bind("<Button-1>", lambda _event, d=delta: self._change_font(d))
            self._font_buttons.append(btn)
        return label

    def _build_terminal(self) -> tk.Frame:
        frame = tk.Frame(self.paned, bd=0)
        self.term_head = self._make_header(frame, "终端")
        self.term_text = self._scrolled_text(frame)
        self.term_row = tk.Frame(frame, bd=0)
        self.term_row.pack(side="bottom", fill="x")
        self.term_prompt = tk.Label(self.term_row, text=" > ", anchor="w")
        self.term_prompt.pack(side="left")
        self.term_entry = tk.Entry(self.term_row, bd=0, highlightthickness=0,
                                  font=(code_family(), self.font_size))
        self.term_entry.pack(side="left", fill="x", expand=True)
        self.term_entry.bind("<Return>", self._submit)
        self.term_entry.bind("<Up>", self._history_up)
        self.term_entry.bind("<Down>", self._history_down)
        return frame

    def _build_log(self) -> tk.Frame:
        frame = tk.Frame(self.paned, bd=0)
        self.log_head = self._make_header(frame, "日志")
        self.log_text = self._scrolled_text(frame)
        return frame

    # ----------------------------------------------------------------- theming
    def apply_theme(self, theme: Theme) -> None:
        """Recolour every dock widget for ``theme`` and refresh the log tags."""
        self.theme = theme
        label_size = max(8, self.font_size - 1)
        for frame in (
            self.frame,
            self.handle,
            self._panes["terminal"],
            self._panes["log"],
            self.term_row,
        ):
            frame.configure(bg=theme.panel)
        self.paned.configure(bg=theme.border, sashwidth=px(self.master, 5))
        for label in self._header_labels:
            label.configure(bg=theme.panel, fg=theme.accent,
                            font=ui_font(label_size, True))
        for btn in self._font_buttons:
            btn.configure(bg=theme.panel, fg=theme.muted,
                          font=ui_font(label_size))
        for text in (self.term_text, self.log_text):
            text.configure(
                bg=theme.bg,
                fg=theme.text,
                font=(code_family(), self.font_size),
                insertbackground=theme.caret,
                selectbackground=theme.sel_bg,
                selectforeground=theme.text,
            )
        self.term_entry.configure(
            bg=theme.bg,
            fg=theme.text,
            insertbackground=theme.caret,
            selectbackground=theme.sel_bg,
            selectforeground=theme.text,
            font=(code_family(), self.font_size),
        )
        self.term_prompt.configure(bg=theme.panel, fg=theme.accent,
                                   font=ui_font(label_size))
        # Per-level colour tags for the log pane.
        self.log_text.tag_configure("log_error", foreground=theme.status_error)
        self.log_text.tag_configure("log_warning", foreground=_warn_color(theme))
        self.log_text.tag_configure("log_info", foreground=theme.text)
        self.log_text.tag_configure("log_debug", foreground=theme.muted)
        # Error highlight for raw terminal output.
        self.term_text.tag_configure("term_error", foreground=theme.status_error)

    # ------------------------------------------------------------ font sizing
    def _change_font(self, delta: int) -> None:
        self.set_font_size(self.font_size + delta)

    def set_font_size(self, size: int) -> None:
        """Adjust the pane font size (8–30 pt) and persist it when possible."""
        size = max(8, min(30, int(size)))
        if size == self.font_size:
            return
        self.font_size = size
        self.apply_theme(self.theme)
        if self.on_font_change is not None:
            self.on_font_change(self.font_size)

    # ------------------------------------------------------------------ panes
    @property
    def terminal_visible(self) -> bool:
        return "terminal" in self.visible

    @property
    def log_visible(self) -> bool:
        return "log" in self.visible

    def show(self, name: str) -> None:
        """Show one pane, packing the dock if it was hidden."""
        if name in self.visible or name not in self._panes:
            return
        self.visible.add(name)
        self.paned.add(
            self._panes[name], minsize=px(self.master, 180), stretch="always"
        )
        self.frame.configure(height=self._height)
        # The editor body was packed earlier with expand=True. Insert the dock
        # *before* it in the pack order, otherwise the body keeps all leftover
        # space and the dock collapses to a couple of pixels.
        options: dict[str, object] = {"side": "bottom", "fill": "x"}
        anchor = getattr(self.master, "body", None)
        if anchor is not None and anchor.winfo_exists():
            options["before"] = anchor
        try:
            self.frame.pack(**options)
        except tk.TclError:
            self.frame.pack(side="bottom", fill="x")
        if name == "terminal":
            self.term_entry.focus_set()

    def hide(self, name: str) -> None:
        """Hide one pane, unpacking the dock when nothing is left."""
        if name not in self.visible:
            return
        self.visible.discard(name)
        self.paned.forget(self._panes[name])
        if not self.visible:
            self.frame.pack_forget()

    def toggle_terminal(self) -> bool:
        if self.terminal_visible:
            self.hide("terminal")
        else:
            self.show("terminal")
        return self.terminal_visible

    def toggle_log(self) -> bool:
        if self.log_visible:
            self.hide("log")
        else:
            self.show("log")
        return self.log_visible

    def hide_all(self) -> None:
        for name in tuple(self.visible):
            self.hide(name)

    # ------------------------------------------------------------------ resize
    def _start_resize(self, event: tk.Event) -> None:
        self._drag = (event.y_root, self.frame.winfo_height(), event.x_root)

    def _on_resize(self, event: tk.Event) -> None:
        if self._drag is None:
            return
        start_y, start_height, _start_x = self._drag
        height = start_height + (start_y - event.y_root)
        height = max(self._min_height, min(self._max_height, height))
        self._height = height
        self.frame.configure(height=height)

    def _end_resize(self, _event: tk.Event) -> None:
        self._drag = None

    # ------------------------------------------------------------------- output
    @staticmethod
    def _is_special_line(line: str) -> str | None:
        """Return a tag name when ``line`` looks like an error/warning."""
        low = line.lower()
        for hint in _TERM_ERROR_HINTS_ASCII:
            if hint in low:
                return "term_error"
        for hint in _TERM_ERROR_HINTS_CJK:
            if hint in line:
                return "term_error"
        return None

    def _append(self, widget: tk.Text, message: str) -> None:
        try:
            widget.configure(state="normal")
            widget.insert("end", message)
            lines = int(widget.index("end-1c").split(".")[0])
            if lines > MAX_LINES:
                widget.delete("1.0", f"{lines - MAX_LINES}.0")
            widget.configure(state="disabled")
            widget.see("end")
        except (tk.TclError, ValueError):
            return

    def _append_log(self, message: str, tag: str | None) -> None:
        try:
            widget = self.log_text
            widget.configure(state="normal")
            widget.insert("end", message, tag or "")
            lines = int(widget.index("end-1c").split(".")[0])
            if lines > MAX_LINES:
                widget.delete("1.0", f"{lines - MAX_LINES}.0")
            widget.configure(state="disabled")
            widget.see("end")
        except (tk.TclError, ValueError):
            return

    def _append_terminal(self, message: str) -> None:
        try:
            widget = self.term_text
            widget.configure(state="normal")
            for line in message.split("\n"):
                if line == "":
                    widget.insert("end", "\n")
                    continue
                tag = self._is_special_line(line)
                widget.insert("end", line + "\n", tag or "")
            lines = int(widget.index("end-1c").split(".")[0])
            if lines > MAX_LINES:
                widget.delete("1.0", f"{lines - MAX_LINES}.0")
            widget.configure(state="disabled")
            widget.see("end")
        except (tk.TclError, ValueError):
            return

    def clear_terminal(self) -> None:
        self._set_text(self.term_text, "")

    def clear_log(self) -> None:
        self._set_text(self.log_text, "")

    def _set_text(self, widget: tk.Text, value: str) -> None:
        try:
            widget.configure(state="normal")
            widget.delete("1.0", "end")
            if value:
                widget.insert("1.0", value)
            widget.configure(state="disabled")
        except tk.TclError:
            return

    def flush(self) -> None:
        """Drain queued log records and terminal results on the Tk thread."""
        while True:
            try:
                record = self._log_queue.get_nowait()
            except queue.Empty:
                break
            stamp = time.strftime("%H:%M:%S", time.localtime(record.created))
            tag = LOG_LEVEL_TAGS.get(record.levelno)
            self._append_log(
                f"[{stamp}] {record.levelname:<7} {record.getMessage()}\n",
                tag,
            )
        while True:
            try:
                output, code = self._results.get_nowait()
            except queue.Empty:
                break
            self._busy = False
            self._append_terminal(output)
            if code:
                self._append_terminal(f"\n[退出码 {code}]\n")

    def _drain(self) -> None:
        self.flush()
        try:
            self._poll_job = self.master.after(100, self._drain)
        except tk.TclError:
            self._poll_job = None

    def cancel_pending(self) -> None:
        """Stop polling and detach the logging handler before teardown."""
        if self._poll_job is not None:
            try:
                self.master.after_cancel(self._poll_job)
            except tk.TclError:
                pass
            self._poll_job = None
        self.logger.removeHandler(self._handler)
        self._handler.close()

    # ----------------------------------------------------------------- terminal
    def _submit(self, _event: tk.Event | None = None) -> str:
        command = self.term_entry.get().strip()
        self.term_entry.delete(0, "end")
        if not command:
            return "break"
        if not self._history or self._history[-1] != command:
            self._history.append(command)
        self._history_index = len(self._history)
        self._append(self.term_text, f"> {command}\n")
        self.logger.info("终端命令：%s", command)
        self._dispatch(command)
        return "break"

    def _history_up(self, _event: tk.Event) -> str:
        if not self._history:
            return "break"
        self._history_index = max(0, self._history_index - 1)
        self._history_show()
        return "break"

    def _history_down(self, _event: tk.Event) -> str:
        self._history_index = min(len(self._history), self._history_index + 1)
        self._history_show()
        return "break"

    def _history_show(self) -> None:
        self.term_entry.delete(0, "end")
        if self._history_index < len(self._history):
            self.term_entry.insert(0, self._history[self._history_index])

    def _dispatch(self, command: str) -> None:
        lowered = command.lower()
        if lowered in ("cls", "clear"):
            self.clear_terminal()
            return
        if lowered in ("exit", "quit"):
            self.hide_all()
            return
        if lowered in ("help", "?"):
            self._append(self.term_text, HELP)
            return
        if lowered == "pwd":
            self._append(self.term_text, f"{self._cwd}\n")
            return
        if lowered == "cd" or lowered.startswith("cd "):
            self._change_dir(command[2:].strip())
            return
        if lowered.startswith("open "):
            target = command[4:].strip().strip('"').strip("'")
            if self.on_open_file is not None:
                self.on_open_file(target)
            else:
                self._append(self.term_text, "当前环境不支持打开文件。\n")
            return
        self._run_async(command)

    def _change_dir(self, target: str) -> None:
        if not target:
            self._append(self.term_text, f"{self._cwd}\n")
            return
        candidate = Path(target.strip('"').strip("'"))
        if not candidate.is_absolute():
            candidate = Path(self._cwd) / candidate
        try:
            resolved = candidate.resolve()
        except OSError as error:
            self._append(self.term_text, f"路径无效：{error}\n")
            return
        if not resolved.is_dir():
            self._append(self.term_text, f"目录不存在：{resolved}\n")
            return
        self._cwd = str(resolved)
        self.logger.info("终端工作目录：%s", self._cwd)
        self._append(self.term_text, f"{self._cwd}\n")

    def _run_async(self, command: str) -> None:
        if self._busy:
            self._append(self.term_text, "上一条命令仍在执行，请稍候。\n")
            return
        self._busy = True
        cwd = self._cwd
        startupinfo = None
        creationflags = 0
        if sys.platform == "win32":
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

        def worker() -> None:
            try:
                completed = subprocess.run(
                    command,
                    shell=True,
                    cwd=cwd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    timeout=COMMAND_TIMEOUT,
                    startupinfo=startupinfo,
                    creationflags=creationflags,
                    check=False,
                )
                raw = completed.stdout or b""
                output = raw.decode(locale.getpreferredencoding(False), "replace")
                code = int(completed.returncode)
            except subprocess.TimeoutExpired:
                output, code = f"命令超时（{COMMAND_TIMEOUT}s）\n", -1
            except OSError as error:
                output, code = f"执行失败：{error}\n", -1
            if len(output) > MAX_OUTPUT:
                output = "…（输出已截断）\n" + output[-MAX_OUTPUT:]
            self._results.put((output, code))

        threading.Thread(target=worker, daemon=True).start()
