"""Main window, menus, shortcuts and file handling."""

from __future__ import annotations

import logging
import sys
import tkinter as tk
import traceback
from collections.abc import Callable
from pathlib import Path
from tkinter import colorchooser, filedialog, messagebox, simpledialog
from typing import Any
from urllib.parse import unquote, urlparse

from . import __version__
from .analysis import active_checks, check_syntax, document_keywords
from .autocomplete import AutoComplete
from .ce_xml import build_ce_xml, extract_ce_code, has_script, is_cheat_table
from .completions import Completion, load_completions
from .config import DEFAULTS, load_config, save_config
from .dock import LOGGER_NAME, BottomDock
from .editor import CodeEditor
from .findbar import FindBar
from .lexer import LexResult
from .reference import ReferencePanel
from .templates import (
    CARET,
    TemplateRule,
    alloc_name,
    auto_pair_blocks,
    completion_argument,
    insert_into_section,
    insert_sections,
    parse_templates,
    render,
    resolve_variable,
)
from .themes import THEMES, Theme, get_theme
from .ui import (
    HoverButton,
    Separator,
    bind_context,
    center_window,
    clamp_rect,
    configure_scaling,
    enable_dpi_awareness,
    frameless,
    popup_menu,
    px,
    ui_font,
    work_area,
)

APP_TITLE = "CE 脚本编辑器"
MODE_NAMES = {"lua": "Lua", "aa": "Auto Assembler", "c": "C"}
FILE_TYPES = [
    ("CE 脚本站", "*.lua *.aa *.ce *.txt *.asm"),
    ("Lua 脚本", "*.lua"),
    ("Auto Assembler", "*.aa *.asm *.ce"),
    ("所有文件", "*.*"),
]
XML_TYPES = [("CE 表格", "*.CT *.xml"), ("所有文件", "*.*")]
MAX_RECENT = 10

WELCOME = """-- CE 脚本编辑器
-- Ctrl+Space 补全 · Ctrl+F 查找 · Ctrl+Shift+F 格式化 · F1 API 参考

[ENABLE]
aobscanmodule(INJECT, game.exe, 48 8B 05 ?? ?? ?? ??)
alloc(newmem, 0x1000)
label(code)
label(return)

newmem:
  mov [player_hp], eax
  jmp return

code:
  mov eax, [rax+0x10]
  jmp return

registersymbol(INJECT)
player_hp:
  dd 0

return:
  jmp code+4

[DISABLE]
unregistersymbol(INJECT)
dealloc(newmem)
"""


def _read_text(path: Path) -> str:
    for encoding in ("utf-8-sig", "utf-8", "gbk"):
        try:
            return path.read_text(encoding=encoding)
        except UnicodeDecodeError:
            continue
    return path.read_text(encoding="latin-1")


def _initial_file(argv: list[str]) -> str | None:
    """Find the first existing file in an external "open with" argument list.

    Both plain paths and ``file://`` URIs are accepted; relative paths are
    resolved against the process working directory, which is what the Windows
    shell, ``python -m ce_edit`` and the frozen exe all pass in.
    """
    for raw in argv:
        candidate = raw.strip()
        if not candidate or candidate.startswith("-"):
            continue
        if candidate.lower().startswith("file:"):
            parsed = urlparse(candidate)
            candidate = unquote(parsed.path)
            if parsed.netloc and parsed.netloc.lower() != "localhost":
                candidate = f"//{parsed.netloc}{candidate}"
            elif sys.platform == "win32":
                candidate = candidate.lstrip("/")
        path = Path(candidate)
        if not path.is_absolute():
            path = Path.cwd() / path
        try:
            resolved = path.resolve()
        except OSError:
            continue
        if resolved.is_file():
            return str(resolved)
    return None


class CeEditApp(tk.Tk):
    """The standalone CE script editor window."""

    def __init__(
        self,
        config_file: Path | None = None,
        initial_file: str | None = None,
    ) -> None:
        # Must run before Tk creates its root window, otherwise Windows
        # bitmap-scales the whole UI on high-DPI displays.
        enable_dpi_awareness()
        super().__init__()
        self.scale = configure_scaling(self)
        self.config_file = config_file
        self.initial_file = initial_file
        settings = load_config(config_file)
        self.settings = settings
        self.syntax_options = dict(settings.get("syntax_checks", {}))
        self.templates = parse_templates(settings.get("templates"))
        self._last_report_key = ""
        self.theme = get_theme(str(settings.get("theme", DEFAULTS["theme"])))
        self.recent = [
            str(item)
            for item in settings.get("recent_files", [])
            if isinstance(item, (str, Path))
        ][:MAX_RECENT]
        self.last_dir = str(settings.get("last_dir", ""))
        self.highlight_overrides: dict[str, dict[str, Any]] = dict(
            settings.get("highlight", {}) or {}
        )
        self.path: Path | None = None
        self.display_name: str | None = None
        self.dirty = False
        self._last_len = 0
        self._complete_job: str | None = None
        self._reposition_job: str | None = None
        self.buttons: list[HoverButton] = []
        self.separators: list[Separator] = []
        self.menus: list[tk.Menu] = []
        self.menu_buttons: list[tk.Menubutton] = []
        self.labels: list[tk.Label] = []
        self.rules: list[tk.Frame] = []
        self.context_target: tk.Misc | None = None
        self._menu_busy = False
        self._move_origin: tuple[int, int, int, int] | None = None
        self._resize_origin: tuple[int, int, int, int] | None = None
        self._saved_geometry: str | None = None
        self._maximized = False
        self.vim_var = tk.BooleanVar(value=bool(settings.get("vim_mode", False)))

        self.title(APP_TITLE)
        frameless(self)
        self._apply_initial_geometry(str(settings.get("geometry", "")))

        self._build_toolbar()
        self._build_status()
        self._build_dock()
        self._build_body()
        self._build_context_menus()
        self._bind_keys()
        self._bind_window_controls()
        self._configure_vim()
        self.apply_theme(self.theme)
        self.minsize(px(self, 680), px(self, 460))
        if self.highlight_overrides:
            self.editor.apply_highlight_overrides(self.highlight_overrides)
        self._new_document(WELCOME)
        try:
            font_size = int(settings.get("font_size", DEFAULTS["font_size"]))
        except (TypeError, ValueError):
            font_size = int(DEFAULTS["font_size"])
        self.editor.set_font_size(font_size)
        self.editor.set_show_numbers(
            bool(settings.get("show_line_numbers", DEFAULTS["show_line_numbers"]))
        )
        self.numbers_var.set(self.editor.show_numbers)
        if bool(settings.get("show_reference", DEFAULTS["show_reference"])):
            self._show_reference(True, animate=False)
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.bind("<Configure>", self._on_root_configure, add="+")
        self._log(f"启动 {APP_TITLE} {__version__}")
        if initial_file:
            self._log(f"外部调用传入文件：{initial_file}")
            self.open_file(initial_file)

    # ------------------------------------------------------------ runtime log
    def _log(self, message: str, level: int = logging.INFO) -> None:
        """Write a line to the dock's log pane (and every attached handler)."""
        logging.getLogger(LOGGER_NAME).log(level, message)

    # --------------------------------------------------------- window geometry
    def _apply_initial_geometry(self, geometry: str) -> None:
        """Restore a saved geometry, or open centered at a DPI-aware size."""
        if geometry:
            try:
                self.geometry(geometry)
            except tk.TclError:
                pass
            self.after_idle(self._clamp_to_screen)
        else:
            center_window(self, None, width=px(self, 1120), height=px(self, 720))

    def _clamp_to_screen(self) -> None:
        """Pull a restored window back onto a visible monitor."""
        try:
            self.update_idletasks()
            width, height = self.winfo_width(), self.winfo_height()
            x, y = self.winfo_rootx(), self.winfo_rooty()
            if width < 2 or height < 2:
                return
            left, top, right, bottom = work_area(self, x + width // 2, y + height // 2)
            if x >= left and y >= top and x + width <= right and y + height <= bottom:
                return
            width = min(width, right - left)
            height = min(height, bottom - top)
            x, y = clamp_rect(x, y, width, height, (left, top, right, bottom), 0)
            self.geometry(f"{width}x{height}+{x}+{y}")
        except (tk.TclError, ValueError, TypeError):
            return

    def _on_root_configure(self, event: tk.Event) -> None:
        """Keep a visible completion popup anchored to the moved window."""
        if event.widget is not self or not self.autocomplete.visible:
            return
        if self._reposition_job is not None:
            return
        self._reposition_job = self.after_idle(self._reposition_popup)

    def _reposition_popup(self) -> None:
        self._reposition_job = None
        self.autocomplete.reposition()

    # ------------------------------------------------------- frameless controls
    def _bind_window_controls(self) -> None:
        """Provide move / maximize / resize affordances without an OS frame."""
        for widget in (
            self.toolbar_wrapper,
            self.toolbar,
            self.brand_frame,
            self.brand_dot,
            self.brand,
            self.header_title,
            self.header_hint,
            self.status_wrapper,
            self.status,
            *self.labels,
        ):
            widget.bind("<Button-1>", self._start_move)
            widget.bind("<B1-Motion>", self._on_window_move)
            widget.bind("<ButtonRelease-1>", self._end_move)
            widget.bind("<Double-Button-1>", self._toggle_maximize)
        self.grip.bind("<Button-1>", self._start_resize)
        self.grip.bind("<B1-Motion>", self._on_window_resize)
        self.grip.bind("<ButtonRelease-1>", self._end_resize)

    def _start_move(self, event: tk.Event) -> None:
        self._move_origin = (
            event.x_root,
            event.y_root,
            self.winfo_x(),
            self.winfo_y(),
        )

    def _on_window_move(self, event: tk.Event) -> None:
        if self._move_origin is None or self._maximized:
            return
        px_root, py_root, x, y = self._move_origin
        self.geometry(f"+{x + event.x_root - px_root}+{y + event.y_root - py_root}")

    def _end_move(self, _event: tk.Event) -> None:
        self._move_origin = None

    def _start_resize(self, event: tk.Event) -> None:
        self._resize_origin = (
            event.x_root,
            event.y_root,
            self.winfo_width(),
            self.winfo_height(),
        )

    def _on_window_resize(self, event: tk.Event) -> None:
        if self._resize_origin is None:
            return
        px_root, py_root, width, height = self._resize_origin
        new_width = width + event.x_root - px_root
        new_height = height + event.y_root - py_root
        left, top, right, bottom = work_area(self, px_root, py_root)
        min_width, min_height = self.minsize()
        new_width = max(min_width, min(new_width, right - self.winfo_x()))
        new_height = max(min_height, min(new_height, bottom - self.winfo_y()))
        self.geometry(f"{new_width}x{new_height}+{self.winfo_x()}+{self.winfo_y()}")

    def _end_resize(self, _event: tk.Event) -> None:
        self._resize_origin = None

    def _toggle_maximize(self, _event: tk.Event) -> None:
        """Double-click the header to fill (or restore) the current monitor."""
        if self._maximized:
            if self._saved_geometry:
                self.geometry(self._saved_geometry)
            self._maximized = False
            return
        self._saved_geometry = self.geometry()
        left, top, right, bottom = work_area(
            self, self.winfo_rootx(), self.winfo_rooty()
        )
        self.geometry(
            f"{right - left}x{bottom - top}+{left}+{top}"
        )
        self._maximized = True

    # ------------------------------------------------------------------ build
    def _build_menu(self) -> None:
        self.file_menu = tk.Menu(self, tearoff=False)
        self.recent_menu = tk.Menu(self.file_menu, tearoff=False)
        self.edit_menu = tk.Menu(self, tearoff=False)
        self.view_menu = tk.Menu(self, tearoff=False)
        self.help_menu = tk.Menu(self, tearoff=False)
        self.menus = [
            self.file_menu,
            self.recent_menu,
            self.edit_menu,
            self.view_menu,
            self.help_menu,
        ]
        self.file_menu.add_command(label="新建", accelerator="Ctrl+N", command=self.new_file)
        self.file_menu.add_command(label="打开…", accelerator="Ctrl+O", command=self.open_file)
        self.file_menu.add_cascade(label="最近打开", menu=self.recent_menu)
        self.file_menu.add_separator()
        self.file_menu.add_command(label="保存", accelerator="Ctrl+S", command=self.save_file)
        self.file_menu.add_command(
            label="另存为…", accelerator="Ctrl+Shift+S", command=self.save_file_as
        )
        self.file_menu.add_separator()
        self.file_menu.add_command(label="导入 CE 表格 XML…", command=self.import_xml)
        self.file_menu.add_command(label="导出 CE 脚本 XML…", command=self.export_xml)
        self.file_menu.add_separator()
        self.file_menu.add_command(label="退出", command=self.on_close)

        self.edit_menu.add_command(label="撤销", accelerator="Ctrl+Z", command=self._undo)
        self.edit_menu.add_command(label="重做", accelerator="Ctrl+Y", command=self._redo)
        self.edit_menu.add_separator()
        self.edit_menu.add_command(label="剪切", accelerator="Ctrl+X", command=lambda: self._event("<<Cut>>"))
        self.edit_menu.add_command(label="复制", accelerator="Ctrl+C", command=lambda: self._event("<<Copy>>"))
        self.edit_menu.add_command(label="粘贴", accelerator="Ctrl+V", command=lambda: self._event("<<Paste>>"))
        self.edit_menu.add_command(label="全选", accelerator="Ctrl+A", command=self._select_all)
        self.edit_menu.add_separator()
        self.edit_menu.add_command(label="查找", accelerator="Ctrl+F", command=self.show_find)
        self.edit_menu.add_command(label="替换", accelerator="Ctrl+H", command=self.show_replace)
        self.edit_menu.add_command(label="转到行…", accelerator="Ctrl+G", command=self.goto_line_dialog)
        self.edit_menu.add_separator()
        self.edit_menu.add_command(
            label="格式化文档", accelerator="Ctrl+Shift+F", command=self.format_document
        )
        self.edit_menu.add_command(
            label="切换注释", accelerator="Ctrl+/", command=self.editor_toggle_comment
        )

        self.numbers_var = tk.BooleanVar(value=True)
        self.theme_var = tk.StringVar(value=self.theme.key)
        self.view_menu.add_radiobutton(
            label="浅色主题", value="light", variable=self.theme_var,
            command=lambda: self.set_theme("light")
        )
        self.view_menu.add_radiobutton(
            label="深色主题", value="dark", variable=self.theme_var,
            command=lambda: self.set_theme("dark")
        )
        self.view_menu.add_separator()
        self.view_menu.add_command(label="增大字号", accelerator="Ctrl++", command=lambda: self.zoom(1))
        self.view_menu.add_command(label="减小字号", accelerator="Ctrl+-", command=lambda: self.zoom(-1))
        self.view_menu.add_command(label="重置字号", accelerator="Ctrl+Shift+0", command=self.zoom_reset)
        self.view_menu.add_separator()
        self.view_menu.add_checkbutton(
            label="显示行号", variable=self.numbers_var, command=self.toggle_numbers
        )
        self.view_menu.add_command(
            label="API 参考面板", accelerator="F1", command=self.toggle_reference
        )
        self.view_menu.add_separator()
        self.view_menu.add_command(
            label="终端窗口", accelerator="Ctrl+\\", command=self.toggle_terminal
        )
        self.view_menu.add_command(
            label="日志窗口", accelerator="Ctrl+0", command=self.toggle_log
        )
        self.view_menu.add_separator()
        self.view_menu.add_command(
            label="代码高亮设置…", command=self.show_highlight_settings
        )

        self.help_menu.add_command(label="快捷键", command=self.show_shortcuts)
        self.help_menu.add_command(label="关于", command=self.show_about)

        cascades = (
            ("文件", self.file_menu),
            ("编辑", self.edit_menu),
            ("视图", self.view_menu),
            ("帮助", self.help_menu),
        )
        self.menubar: tk.Menu | None = None
        # No buttons anywhere in the window: the menus are reachable through
        # the right-click menu, Alt+F/E/V/H and keyboard shortcuts. macOS keeps
        # its native application menu because it is not part of the window.
        if str(self.tk.call("tk", "windowingsystem")) == "aqua":
            bar = tk.Menu(self, tearoff=False)
            for label, menu in cascades:
                bar.add_cascade(label=label, menu=menu)
            self.configure(menu=bar)
            self.menubar = bar
        self._rebuild_recent()

    def _build_toolbar(self) -> None:
        """Slim frameless drag strip: document title only, no buttons."""
        self.toolbar_wrapper = tk.Frame(self, bd=0)
        self.toolbar_wrapper.pack(side="top", fill="x")
        self.toolbar = tk.Frame(self.toolbar_wrapper, bd=0)
        self.toolbar.pack(side="top", fill="x")
        self.toolbar_rule = tk.Frame(
            self.toolbar_wrapper, height=px(self, 1), bd=0
        )
        self.toolbar_rule.pack(side="top", fill="x")
        self.rules.append(self.toolbar_rule)

        self.brand_frame = tk.Frame(self.toolbar, bd=0)
        self.brand_frame.pack(side="left", padx=(px(self, 12), px(self, 8)))
        self.brand_dot = tk.Frame(
            self.brand_frame,
            width=px(self, 8),
            height=px(self, 8),
            bd=0,
        )
        self.brand_dot.pack(side="left", padx=(0, px(self, 7)))
        self.brand = tk.Label(self.brand_frame, text="CE EDIT", anchor="w")
        self.brand.pack(side="left")
        self.header_title = tk.Label(self.toolbar, text="", anchor="w")
        self.header_title.pack(side="left", padx=(px(self, 10), 0))
        self.header_hint = tk.Label(
            self.toolbar, text="右键打开菜单  ·  Ctrl+Q 退出", anchor="e"
        )
        self.header_hint.pack(side="right", padx=px(self, 12))
        self._build_menu()

    def _build_status(self) -> None:
        self.status_wrapper = tk.Frame(self, bd=0)
        self.status_wrapper.pack(side="bottom", fill="x")
        self.status_rule = tk.Frame(
            self.status_wrapper, height=px(self, 1), bd=0
        )
        self.status_rule.pack(side="top", fill="x")
        self.rules.append(self.status_rule)
        self.status = tk.Frame(self.status_wrapper, bd=0)
        self.status.pack(side="top", fill="x")
        self.mode_label = tk.Label(self.status, text="Lua", anchor="w")
        self.mode_label.pack(
            side="left",
            padx=(px(self, 12), px(self, 10)),
            pady=px(self, 4),
        )
        self.pos_label = tk.Label(self.status, text="行 1，列 1", anchor="w")
        self.pos_label.pack(side="left", padx=px(self, 6))
        self.sel_label = tk.Label(self.status, text="", anchor="w")
        self.sel_label.pack(side="left", padx=px(self, 6))
        self.lint_label = tk.Label(self.status, text="", anchor="w")
        self.lint_label.pack(side="left", padx=px(self, 10))
        self.font_label = tk.Label(self.status, text="", anchor="e")
        self.font_label.pack(side="right", padx=(px(self, 12), px(self, 4)))
        self.theme_label = tk.Label(self.status, text="UTF-8", anchor="e")
        self.theme_label.pack(side="right", padx=px(self, 12))
        self.vim_label = tk.Label(self.status, text="", anchor="e")
        self.vim_label.pack(side="right", padx=px(self, 10))
        self.labels.extend(
            [
                self.mode_label,
                self.pos_label,
                self.sel_label,
                self.lint_label,
                self.font_label,
                self.theme_label,
                self.vim_label,
            ]
        )
        # Frameless windows have no OS resize border; this grip replaces it.
        self.grip = tk.Label(
            self.status, text="◢", anchor="se", cursor="size_nw_se", bd=0
        )
        self.grip.place(relx=1.0, rely=1.0, anchor="se", x=-2, y=-2)

    def _build_dock(self) -> None:
        """Create the hidden bottom terminal / log dock."""
        self.dock = BottomDock(
            self,
            self.theme,
            on_open_file=self.open_file,
            font_size=int(self.settings.get("output_font_size", 12)),
            on_font_change=self._persist_output_font_size,
        )

    def _build_body(self) -> None:
        self.body = tk.Frame(self, bd=0)
        self.body.pack(side="top", fill="both", expand=True)
        self.edit_area = tk.Frame(self.body, bd=0)
        self.edit_area.pack(side="left", fill="both", expand=True)
        self.edit_area.rowconfigure(1, weight=1)
        self.edit_area.columnconfigure(0, weight=1)
        self.editor = CodeEditor(
            self.edit_area,
            self.theme,
            on_change=self._on_editor_change,
            on_cursor=self._on_cursor,
            on_status=self._on_status,
            on_complete=self._force_complete,
        )
        self.autocomplete = AutoComplete(
            self.editor,
            keywords_provider=self._document_keywords,
            argument_provider=self._completion_argument,
            expansion_provider=self._completion_expansion,
        )
        self.editor.after_return = self._after_return
        self.findbar = FindBar(self.edit_area, self.editor, self.theme)
        self.findbar.grid(row=0, column=0, sticky="ew")
        self.findbar.grid_remove()
        self.editor.grid(row=1, column=0, sticky="nsew")
        self.editor.text.focus_set()

        self.ref_separator = Separator(self.body, "vertical")
        self.reference = ReferencePanel(self.body, self.theme, self.insert_api)
        from .animation import Animator

        self._ref_animator = Animator(self.reference)
        self._reference_visible = False

    # ---------------------------------------------------------- context menus
    def _act(
        self, action: Callable[[], object], focus: bool = True
    ) -> Callable[[], None]:
        """Wrap a menu command so a failure is logged instead of looking inert."""
        def invoke() -> None:
            try:
                if focus:
                    self.editor.text.focus_set()
                action()
            except Exception as error:  # keep the popup action alive
                self._log(f"菜单操作失败：{error}", logging.ERROR)
                if sys.stderr is not None:
                    traceback.print_exc()
        return invoke

    def _build_context_menus(self) -> None:
        """Build themed right-click menus for every surface of the window."""
        act = self._act
        self.template_menu = tk.Menu(self, tearoff=False)
        menu = self.editor_menu = tk.Menu(self, tearoff=False)
        menu.add_command(label="新建", accelerator="Ctrl+N", command=act(self.new_file))
        menu.add_command(label="打开…", accelerator="Ctrl+O", command=act(self.open_file))
        menu.add_command(label="保存", accelerator="Ctrl+S", command=act(self.save_file))
        menu.add_command(
            label="另存为…", accelerator="Ctrl+Shift+S", command=act(self.save_file_as)
        )
        menu.add_separator()
        menu.add_command(label="撤销", accelerator="Ctrl+Z", command=act(self._undo))
        menu.add_command(label="重做", accelerator="Ctrl+Y", command=act(self._redo))
        for label, virtual in (
            ("剪切", "<<Cut>>"),
            ("复制", "<<Copy>>"),
            ("粘贴", "<<Paste>>"),
        ):
            menu.add_command(
                label=label,
                command=act(lambda value=virtual: self._context_event(value), focus=False),
            )
        menu.add_command(label="删除", command=act(self._context_delete, focus=False))
        menu.add_command(label="全选", accelerator="Ctrl+A", command=act(self._select_all))
        menu.add_separator()
        menu.add_command(label="查找", accelerator="Ctrl+F", command=act(self.show_find))
        menu.add_command(label="替换…", accelerator="Ctrl+H", command=act(self.show_replace))
        menu.add_command(label="转到行…", accelerator="Ctrl+G", command=act(self.goto_line_dialog))
        menu.add_command(
            label="代码补全", accelerator="Ctrl+Space", command=act(self._force_complete)
        )
        menu.add_command(
            label="格式化文档", accelerator="Ctrl+Shift+F", command=act(self.format_document)
        )
        menu.add_command(
            label="切换注释", accelerator="Ctrl+/", command=act(self.editor_toggle_comment)
        )
        menu.add_command(
            label="选择下一个相同词",
            accelerator="Ctrl+D",
            command=act(lambda: self.editor._on_select_next()),
        )
        menu.add_command(
            label="复制当前行",
            accelerator="Ctrl+Shift+D",
            command=act(lambda: self.editor._on_duplicate(None)),
        )
        menu.add_command(
            label="语法检查报告…", command=act(self.show_syntax_report)
        )
        menu.add_separator()
        menu.add_checkbutton(
            label="Vim / nvim 模式",
            variable=self.vim_var,
            command=act(self.toggle_vim_mode),
        )
        menu.add_separator()
        menu.add_command(label="浅色主题", command=act(lambda: self.set_theme("light")))
        menu.add_command(label="深色主题", command=act(lambda: self.set_theme("dark")))
        menu.add_command(label="增大字号", command=act(lambda: self.zoom(1)))
        menu.add_command(label="减小字号", command=act(lambda: self.zoom(-1)))
        menu.add_command(label="重置字号", command=act(self.zoom_reset))
        menu.add_command(label="显示行号", command=act(self.toggle_line_numbers))
        menu.add_command(
            label="API 参考面板", accelerator="F1", command=act(self.toggle_reference)
        )
        menu.add_command(label="代码高亮设置…", command=act(self.show_highlight_settings))
        menu.add_command(label="设置…", accelerator="Ctrl+,", command=act(self.show_settings))
        menu.add_command(label="生成配套代码", command=act(self.generate_pair_code))
        menu.add_cascade(label="代码模板", menu=self.template_menu)
        menu.add_separator()
        menu.add_command(
            label="终端窗口", accelerator="Ctrl+\\", command=act(self.toggle_terminal)
        )
        menu.add_command(
            label="日志窗口", accelerator="Ctrl+0", command=act(self.toggle_log)
        )
        menu.add_command(label="快捷键", command=act(self.show_shortcuts))
        menu.add_command(label="关于", command=act(self.show_about))
        menu.add_separator()
        menu.add_command(label="退出", accelerator="Ctrl+Q", command=act(self.on_close))
        menu.configure(postcommand=self._sync_editor_menu)

        self.entry_menu = tk.Menu(self, tearoff=False)
        for label, virtual in (
            ("剪切", "<<Cut>>"),
            ("复制", "<<Copy>>"),
            ("粘贴", "<<Paste>>"),
        ):
            self.entry_menu.add_command(
                label=label,
                command=act(lambda value=virtual: self._context_event(value), focus=False),
            )
        self.entry_menu.add_separator()
        self.entry_menu.add_command(
            label="删除", command=act(self._context_delete, focus=False)
        )
        self.entry_menu.add_command(
            label="全选", command=act(self._context_select_all, focus=False)
        )

        self.readonly_menu = tk.Menu(self, tearoff=False)
        self.readonly_menu.add_command(
            label="复制", command=act(lambda: self._context_event("<<Copy>>"), focus=False)
        )
        self.readonly_menu.add_command(
            label="全选", command=act(self._context_select_all, focus=False)
        )
        self.readonly_menu.add_separator()
        self.readonly_menu.add_command(
            label="清空终端", command=act(self.dock.clear_terminal)
        )
        self.readonly_menu.add_command(label="清空日志", command=act(self.dock.clear_log))

        self.tree_menu = tk.Menu(self, tearoff=False)
        self.tree_menu.add_command(
            label="插入到光标处", command=act(self._context_insert_api)
        )
        self.tree_menu.add_command(
            label="复制名称", command=act(self._context_copy_name)
        )
        self.tree_menu.add_separator()
        self.tree_menu.add_command(
            label="展开全部分类", command=act(lambda: self._context_expand(True))
        )
        self.tree_menu.add_command(
            label="折叠全部分类", command=act(lambda: self._context_expand(False))
        )
        self.tree_menu.add_separator()
        self.tree_menu.add_command(
            label="清除筛选", command=act(self.reference.clear_filters)
        )

        self.menus.extend(
            (
                self.editor_menu,
                self.entry_menu,
                self.readonly_menu,
                self.tree_menu,
                self.template_menu,
            )
        )
        self._rebuild_template_menu()
        bind_context(self.editor.text, self._show_editor_menu)
        for widget in (self.editor.gutter, self.editor.vsb, self.editor.hsb):
            bind_context(widget, self._show_window_menu)
        for widget in (
            self.findbar.find_entry,
            self.findbar.replace_entry,
            self.reference.search_entry,
            self.dock.term_entry,
        ):
            bind_context(widget, self._show_entry_menu)
        bind_context(self.reference.tree, self._show_tree_menu)
        for widget in (self.dock.term_text, self.dock.log_text):
            bind_context(widget, self._show_readonly_menu)
        # Frameless windows have no title bar to click; make the whole chrome
        # (and the root fallback binding) open the same context menu.
        for widget in (
            self,
            self.body,
            self.edit_area,
            self.toolbar_wrapper,
            self.toolbar,
            self.brand_frame,
            self.brand_dot,
            self.brand,
            self.header_title,
            self.header_hint,
            self.status_wrapper,
            self.status,
            self.grip,
            *self.labels,
            self.dock.frame,
            self.dock.handle,
        ):
            bind_context(widget, self._show_window_menu)

    def _sync_editor_menu(self) -> None:
        """Disable actions that cannot apply to the current selection.

        A ``postcommand`` runs on every post; one bad lookup must never abort
        the menu, otherwise the right-click looks completely unresponsive.
        """
        try:
            has_selection = bool(self.editor.text.tag_ranges("sel"))
        except tk.TclError:
            has_selection = False
        try:
            has_clipboard = bool(self.clipboard_get())
        except (tk.TclError, OSError):
            has_clipboard = False
        for label, enabled in (
            ("剪切", has_selection),
            ("复制", has_selection),
            ("删除", has_selection),
            ("粘贴", has_clipboard),
        ):
            try:
                self.editor_menu.entryconfigure(
                    label, state="normal" if enabled else "disabled"
                )
            except tk.TclError:
                continue

    def _post_menu(
        self,
        menu: tk.Menu,
        widget: tk.Misc | None,
        event: tk.Event,
        target: tk.Misc,
    ) -> str:
        """Post exactly one menu and remember which widget the action targets."""
        if self._menu_busy:
            return "break"
        self._menu_busy = True
        self.context_target = target
        try:
            popup_menu(menu, widget or self, event)
        finally:
            self._menu_busy = False
        return "break"

    def _show_editor_menu(self, event: tk.Event) -> str:
        text = self.editor.text
        number = getattr(event, "num", 0)
        if (
            number in (2, 3)
            and isinstance(getattr(event, "x", None), int)
            and not text.tag_ranges("sel")
        ):
            try:
                text.mark_set("insert", f"@{event.x},{event.y}")
            except tk.TclError:
                pass
            self._on_cursor()
        text.focus_set()
        return self._post_menu(
            self.editor_menu, getattr(event, "widget", None), event, text
        )

    def _show_window_menu(self, event: tk.Event) -> str:
        return self._post_menu(
            self.editor_menu,
            getattr(event, "widget", None),
            event,
            self.editor.text,
        )

    def _show_entry_menu(self, event: tk.Event) -> str:
        widget = event.widget
        if isinstance(widget, tk.Entry):
            widget.focus_set()
        return self._post_menu(self.entry_menu, widget, event, widget)

    def _show_readonly_menu(self, event: tk.Event) -> str:
        widget = event.widget
        return self._post_menu(self.readonly_menu, widget, event, widget)

    def _show_tree_menu(self, event: tk.Event) -> str:
        tree = self.reference.tree
        number = getattr(event, "num", 0)
        if number in (2, 3) and isinstance(getattr(event, "y", None), int):
            row = tree.identify_row(event.y)
            if row:
                tree.selection_set(row)
                tree.focus(row)
        return self._post_menu(self.tree_menu, tree, event, tree)

    def _context_event(self, virtual: str) -> None:
        widget = self.context_target or self.editor.text
        try:
            widget.focus_set()
            widget.event_generate(virtual)
        except tk.TclError as error:
            self._log(f"剪贴板操作失败：{error}", logging.WARNING)

    def _context_delete(self) -> None:
        widget = self.context_target
        if widget is None:
            return
        try:
            if isinstance(widget, tk.Entry):
                if widget.selection_present():
                    widget.delete("sel.first", "sel.last")
            elif isinstance(widget, tk.Text) and widget.tag_ranges("sel"):
                widget.delete("sel.first", "sel.last")
        except tk.TclError:
            return

    def _context_select_all(self) -> None:
        widget = self.context_target
        if isinstance(widget, tk.Entry):
            widget.select_range(0, "end")
            widget.icursor("end")
        elif isinstance(widget, tk.Text):
            widget.tag_add("sel", "1.0", "end-1c")
            widget.focus_set()
        else:
            self._select_all()

    def _selected_reference_item(self) -> Completion | None:
        tree = self.reference.tree
        selection = tree.selection()
        if not selection:
            return None
        return self.reference._by_iid.get(str(selection[0]))

    def _context_insert_api(self) -> None:
        item = self._selected_reference_item()
        if item is not None:
            self.insert_api(item)

    def _context_copy_name(self) -> None:
        item = self._selected_reference_item()
        if item is None:
            return
        self.clipboard_clear()
        self.clipboard_append(item.name)

    def _context_expand(self, open_it: bool) -> None:
        tree = self.reference.tree
        for parent in tree.get_children():
            tree.item(parent, open=open_it)

    def _bind_keys(self) -> None:
        self.bind("<Control-n>", lambda _event: self.new_file())
        self.bind("<Control-o>", lambda _event: self.open_file())
        self.bind("<Control-s>", lambda _event: self.save_file() or "break")
        self.bind("<Control-Shift-S>", lambda _event: self.save_file_as())
        self.bind("<Control-f>", lambda _event: self.show_find() or "break")
        self.bind("<Control-h>", lambda _event: self.show_replace() or "break")
        self.bind("<Control-g>", lambda _event: self.goto_line_dialog() or "break")
        self.bind("<Control-Shift-F>", lambda _event: self.format_document() or "break")
        self.bind("<Control-plus>", lambda _event: self.zoom(1))
        self.bind("<Control-equal>", lambda _event: self.zoom(1))
        self.bind("<Control-minus>", lambda _event: self.zoom(-1))
        # Ctrl+Q quits (with the unsaved-changes prompt); Ctrl+0 opens the
        # log dock, so zoom reset moves to Ctrl+Shift+0.
        for sequence in ("<Control-q>", "<Control-Q>"):
            self.bind(sequence, lambda _event: self.on_close() or "break")
        self.bind("<Control-0>", lambda _event: self.toggle_log() or "break")
        self.bind("<Control-Shift-0>", lambda _event: self.zoom_reset() or "break")
        self.bind("<Control-parenright>", lambda _event: self.zoom_reset() or "break")
        for sequence in ("<Control-backslash>", "<Control-Key-backslash>"):
            self.bind(sequence, lambda _event: self.toggle_terminal() or "break")
        self.bind("<Control-comma>", lambda _event: self.show_settings() or "break")
        for sequence in ("<Control-Alt-v>", "<Control-Alt-V>"):
            self.bind(sequence, lambda _event: self._toggle_vim_key() or "break")
        self.bind("<F1>", lambda _event: self.toggle_reference(True))
        self.bind("<Escape>", self._on_escape)
        self.bind("<Control-MouseWheel>", self._on_wheel)
        # X11 reports wheel motion as buttons 4/5 rather than event.delta.
        self.bind(
            "<Control-Button-4>", lambda _event: self.zoom(1) or "break"
        )
        self.bind(
            "<Control-Button-5>", lambda _event: self.zoom(-1) or "break"
        )
        # With no visible menubar, Alt+F/E/V/H still post the dropdown menus.
        for key, menu in (
            ("f", self.file_menu),
            ("e", self.edit_menu),
            ("v", self.view_menu),
            ("h", self.help_menu),
        ):
            self.bind(
                f"<Alt-{key}>",
                lambda event, target=menu: self._post_menu(
                    target, getattr(event, "widget", None), event, self.editor.text
                ),
            )

    # --------------------------------------------------------------- document
    def _new_document(self, text: str = "") -> None:
        self.editor.set_text(text)
        self.path = None
        self.display_name = None
        self.dirty = False
        self._last_len = len(text)
        self.findbar.hide()
        self._refresh_title()
        self._refresh_status()

    def new_file(self) -> bool:
        """Create a blank document, asking about unsaved work."""
        if not self._maybe_save():
            return False
        self._new_document("")
        self.editor.text.focus_set()
        self._log("新建文档")
        return True

    def open_file(self, path: str | None = None) -> bool:
        """Open a script or a CE XML export."""
        if not self._maybe_save():
            return False
        if path is None:
            chosen = filedialog.askopenfilename(
                parent=self,
                title="打开脚本",
                initialdir=self.last_dir or None,
                filetypes=FILE_TYPES + [("CE 表格", "*.CT *.xml")],
            )
            if not chosen:
                return False
            path = chosen
        target = Path(path)
        try:
            text = _read_text(target)
        except OSError as error:
            self._log(f"打开失败：{target} — {error}", logging.ERROR)
            messagebox.showerror("打开失败", f"无法读取文件：\n{error}", parent=self)
            return False
        if is_cheat_table(text) and has_script(text):
            extracted = extract_ce_code(text)
            if extracted is not None:
                self._new_document(extracted)
                self.dirty = True
                self.display_name = f"XML 导入 · {target.name}"
                self._refresh_title()
                self._remember(target)
                self._log(f"导入 CE 表格脚本：{target}")
                return True
        self._new_document(text)
        self.path = target
        self.last_dir = str(target.parent)
        self._remember(target)
        self._refresh_title()
        self._log(f"打开文件：{target}")
        return True

    def save_file(self) -> bool:
        """Save to the current path, prompting when untitled."""
        if self.path is None:
            return self.save_file_as()
        return self._write(self.path)

    def save_file_as(self) -> bool:
        """Ask for a path and save the document."""
        chosen = filedialog.asksaveasfilename(
            parent=self,
            title="另存为",
            initialdir=self.last_dir or None,
            initialfile=self.path.name if self.path else "script.lua",
            defaultextension=".lua",
            filetypes=FILE_TYPES,
        )
        if not chosen:
            return False
        return self._write(Path(chosen))

    def _write(self, target: Path) -> bool:
        try:
            target.write_text(self.editor.get_text(), encoding="utf-8", newline="\n")
        except OSError as error:
            self._log(f"保存失败：{target} — {error}", logging.ERROR)
            messagebox.showerror("保存失败", f"无法写入文件：\n{error}", parent=self)
            return False
        self._log(f"保存文件：{target}")
        self.path = target
        self.last_dir = str(target.parent)
        self.dirty = False
        self._remember(target)
        self._refresh_title()
        return True

    def _maybe_save(self) -> bool:
        if not self.dirty:
            return True
        answer = messagebox.askyesnocancel(
            "未保存的更改",
            "当前文档已修改，是否保存？",
            parent=self,
        )
        if answer is None:
            self._log("取消退出：未保存的更改")
            return False
        if answer:
            return self.save_file()
        self._log("放弃未保存的更改")
        return True

    def import_xml(self) -> None:
        """Load the script embedded in a Cheat Engine table."""
        chosen = filedialog.askopenfilename(
            parent=self, title="导入 CE 表格", filetypes=XML_TYPES
        )
        if not chosen:
            return
        target = Path(chosen)
        try:
            text = _read_text(target)
        except OSError as error:
            messagebox.showerror("导入失败", str(error), parent=self)
            return
        extracted = extract_ce_code(text) if is_cheat_table(text) else None
        if extracted is None:
            messagebox.showwarning(
                "没有脚本",
                "该文件中没有找到 AssemblerScript 或 LuaScript。",
                parent=self,
            )
            return
        if not self._maybe_save():
            return
        self._new_document(extracted)
        self.dirty = True
        self.display_name = f"XML 导入 · {target.name}"
        self._refresh_title()

    def export_xml(self) -> None:
        """Export the current script as a one-entry CE table."""
        xml = build_ce_xml(self.editor.get_text())
        if xml is None:
            messagebox.showinfo("无内容", "当前文档为空，无法导出。", parent=self)
            return
        chosen = filedialog.asksaveasfilename(
            parent=self,
            title="导出 CE 表格",
            initialdir=self.last_dir or None,
            initialfile=(self.path.stem if self.path else "script") + ".CT",
            defaultextension=".CT",
            filetypes=XML_TYPES,
        )
        if not chosen:
            return
        try:
            Path(chosen).write_text(xml + "\n", encoding="utf-8", newline="\n")
        except OSError as error:
            messagebox.showerror("导出失败", str(error), parent=self)
            return
        messagebox.showinfo("导出完成", f"已写入：\n{chosen}", parent=self)

    # ------------------------------------------------------------------ state
    def _on_editor_change(self) -> None:
        self.dirty = True
        length = self.editor.char_count()
        delta = length - self._last_len
        self._last_len = length
        self._refresh_title()
        self._on_cursor()
        self.findbar.on_editor_change()
        if 0 < abs(delta) <= 2:
            if self._complete_job is not None:
                try:
                    self.after_cancel(self._complete_job)
                except tk.TclError:
                    pass
            self._complete_job = self.after(20, self._auto_complete)

    def _auto_complete(self) -> None:
        self._complete_job = None
        if not self.winfo_exists():
            return
        try:
            self.autocomplete.request(force=False)
        except tk.TclError:
            return

    def _force_complete(self, force: bool = True) -> None:
        self.autocomplete.request(force=force)

    def _on_escape(self, event: tk.Event) -> str | None:
        if self.autocomplete.visible:
            self.autocomplete.hide()
            return "break"
        if self.findbar.visible:
            self.findbar.hide()
            return "break"
        return None

    def _on_cursor(self) -> None:
        line, column = self.editor.position()
        self.pos_label.configure(text=f"行 {line}，列 {column}")
        selected = self.editor.selection_size()
        self.sel_label.configure(text=f"已选 {selected}" if selected else "")
        mode = self.editor.document_mode()
        self.mode_label.configure(text=MODE_NAMES.get(mode) or mode)

    def _on_status(self, result: LexResult) -> None:
        """Run the strengthened linter and surface every finding."""
        report = check_syntax(self.editor.get_text(), result, self.syntax_options)
        checks = active_checks(self.syntax_options)
        if report.errors:
            text, colour = f"⚠ {report.message}", self.theme.status_error
        elif report.warnings:
            text, colour = f"ⓘ {report.message}", self.theme.accent
        else:
            text, colour = f"✓ 语法正常 · {checks} 项检查", self.theme.status_ok
        self.lint_label.configure(text=text, fg=colour)
        key = "\n".join(report.lines()) or f"OK:{checks}"
        if key != self._last_report_key:
            if report.diagnostics:
                self._log(
                    "语法检查：" + " | ".join(report.lines()[:3]),
                    logging.ERROR if report.errors else logging.WARNING,
                )
            else:
                self._log(f"语法检查通过（{checks} 项）")
        self._last_report_key = key
        self._on_cursor()

    def _refresh_title(self, suffix: str | None = None) -> None:
        name = (
            self.path.name
            if self.path is not None
            else suffix or self.display_name or "未命名"
        )
        mark = " *" if self.dirty else ""
        self.title(f"{name}{mark} — {APP_TITLE}")
        # The frameless window has no caption bar, so mirror the title here.
        self.header_title.configure(text=f"{name}{mark}")

    def _refresh_status(self) -> None:
        self._on_cursor()
        text = str(self.lint_label.cget("text"))
        if text.startswith("⚠"):
            self.lint_label.configure(fg=self.theme.status_error)
        elif text.startswith("ⓘ"):
            self.lint_label.configure(fg=self.theme.accent)
        elif text:
            self.lint_label.configure(fg=self.theme.status_ok)
        else:
            self.lint_label.configure(fg=self.theme.muted)

    def _remember(self, target: Path) -> None:
        text = str(target)
        self.last_dir = str(target.parent)
        values = [item for item in self.recent if item != text]
        values.insert(0, text)
        self.recent = values[:MAX_RECENT]
        self._rebuild_recent()

    def _rebuild_recent(self) -> None:
        self.recent_menu.delete(0, "end")
        if not self.recent:
            self.recent_menu.add_command(label="（暂无记录）", state="disabled")
            return
        for item in self.recent:
            self.recent_menu.add_command(
                label=Path(item).name,
                command=lambda value=item: self.open_file(value),
            )
        self.recent_menu.add_separator()
        self.recent_menu.add_command(label="清除记录", command=self.clear_recent)

    def clear_recent(self) -> None:
        """Forget the recent file list."""
        self.recent = []
        self._rebuild_recent()

    # --------------------------------------------------- keywords & templates
    def _document_keywords(self) -> list[str]:
        """Keywords extracted from the current AA section for the popup."""
        if not bool(self.settings.get("dynamic_completions", True)):
            return []
        source = self.editor.get_text()
        offset = self.editor._offset_of("insert")
        return document_keywords(source, offset, self.editor.document_mode())

    def _clipboard_text(self) -> str:
        if not bool(self.settings.get("clipboard_variable", True)):
            return ""
        try:
            return str(self.clipboard_get())
        except (tk.TclError, OSError):
            return ""

    def _selected_text(self) -> str:
        ranges = self.editor.text.tag_ranges("sel")
        if len(ranges) != 2:
            return ""
        return str(self.editor.text.get(ranges[0], ranges[1]))

    def _completion_argument(self, function_name: str) -> str | None:
        """Resolve ``$ver$`` for a template-bound function completion."""
        if not bool(self.settings.get("auto_arguments", True)):
            return None
        if self._bound_template(function_name) is not None:
            return resolve_variable(
                self.editor.get_text(),
                self._clipboard_text(),
                self._selected_text(),
                allow_clipboard=bool(self.settings.get("clipboard_variable", True)),
            )
        return completion_argument(
            function_name,
            self.editor.get_text(),
            self._clipboard_text(),
            self._selected_text(),
            self.settings.get("auto_arg_functions", []),
            enabled=True,
            allow_clipboard=bool(self.settings.get("clipboard_variable", True)),
        )

    def _bound_template(self, function_name: str) -> TemplateRule | None:
        folded = function_name.casefold()
        for rule in self.templates:
            if rule.trigger and rule.trigger.casefold() == folded:
                return rule
        return None

    def _completion_expansion(self, function_name: str) -> str | None:
        """Expand a single-line template bound to a function completion."""
        if not bool(self.settings.get("auto_arguments", True)):
            return None
        rule = self._bound_template(function_name)
        if rule is None:
            return None
        variable = resolve_variable(
            self.editor.get_text(),
            self._clipboard_text(),
            self._selected_text(),
            allow_clipboard=bool(self.settings.get("clipboard_variable", True)),
        )
        if variable is None:
            return None
        rendered = render(rule, variable)
        return rendered if len(rendered.splitlines()) == 1 else None

    def _rebuild_template_menu(self) -> None:
        """Refresh the right-click template cascade after settings change."""
        self.template_menu.delete(0, "end")
        for rule in self.templates:
            self.template_menu.add_command(
                label=rule.name,
                command=self._act(
                    lambda item=rule: self.insert_template(item), focus=False
                ),
            )
        self.template_menu.add_separator()
        self.template_menu.add_command(
            label="管理模板 / 设置…", command=self._act(self.show_settings)
        )

    def show_settings(self) -> None:
        """Open the independent settings window."""
        from .settings import SettingsDialog

        dialog = SettingsDialog(self, self.settings, self.apply_settings)
        dialog.show()

    def apply_settings(self, new_settings: dict[str, Any]) -> None:
        """Apply and persist every preference from the settings dialog."""
        self.settings = dict(new_settings)
        self.syntax_options = dict(self.settings.get("syntax_checks", {}))
        self.templates = parse_templates(self.settings.get("templates"))
        theme = THEMES.get(str(self.settings.get("theme", self.theme.key)))
        if theme is not None and theme.key != self.theme.key:
            self.set_theme(theme.key)
        try:
            size = max(7, min(36, int(self.settings.get("font_size", 12))))
        except (TypeError, ValueError):
            size = 12
        self.editor.set_font_size(size)
        self.font_label.configure(text="")
        show_numbers = bool(self.settings.get("show_line_numbers", True))
        self.numbers_var.set(show_numbers)
        self.editor.set_show_numbers(show_numbers)
        self.editor.double_click_word = bool(
            self.settings.get("double_click_word", True)
        )
        self._configure_vim()
        self._rebuild_template_menu()
        self.editor._schedule_highlight()
        self._save_settings()
        self._log("已应用设置")

    def _save_settings(self) -> None:
        current = load_config(self.config_file)
        current.update(self.settings)
        save_config(current, self.config_file)

    def _persist_output_font_size(self, size: int) -> None:
        """Remember the output dock font size across sessions."""
        self.settings["output_font_size"] = max(8, min(30, int(size)))
        self._save_settings()

    def insert_template(self, rule: TemplateRule) -> None:
        """Expand a user template with ``$ver$`` and insert it.

        A template whose section is ``CARET`` (or empty) is pasted at the caret.
        Any other section name targets that labelled ``[SECTION]`` block (a
        missing section is created at the end of the document); this lets users
        drop snippets into custom blocks, not just ``[ENABLE]``/``[DISABLE]``.
        """
        source = self.editor.get_text()
        variable = resolve_variable(
            source,
            self._clipboard_text(),
            self._selected_text(),
            allow_clipboard=bool(self.settings.get("clipboard_variable", True)),
        )
        if variable is None:
            self._log("模板变量未解析：文档中没有已定义变量", logging.WARNING)
            messagebox.showwarning(
                "模板",
                "未找到已定义变量。请先输入 alloc(name, 8) 或选中/复制变量名。",
                parent=self,
            )
            return
        text = render(rule, variable)
        section = rule.section.upper()
        if section == CARET or not section:
            self.editor.insert_text(text)
            self.editor.text.focus_set()
            self._log(f"插入模板「{rule.name}」到光标位置：{variable}")
            return
        new_source = insert_into_section(source, section, text)
        if new_source != source:
            self.editor.set_text(new_source)
            self.editor.text.focus_set()
            self._log(f"插入模板「{rule.name}」到 [{section}] 区块：{variable}")
        else:
            self.editor.insert_text(text)
            self.editor.text.focus_set()
            self._log(f"插入模板「{rule.name}」：{variable}")

    def generate_pair_code(self, variable: str | None = None) -> bool:
        """Generate missing registersymbol / unregister / dealloc lines."""
        source = self.editor.get_text()
        name = variable or resolve_variable(
            source,
            self._clipboard_text(),
            self._selected_text(),
            allow_clipboard=bool(self.settings.get("clipboard_variable", True)),
        )
        if not name:
            self._log("配套代码未生成：没有已定义变量", logging.WARNING)
            return False
        blocks = auto_pair_blocks(source, name, self.templates)
        if not blocks:
            return False
        updated = insert_sections(source, blocks)
        if updated == source:
            return False
        line, column = self.editor.position()
        text = self.editor.text
        text.edit_separator()
        text.replace("1.0", "end-1c", updated)
        text.edit_separator()
        total = int(text.index("end-1c").split(".")[0])
        text.mark_set("insert", f"{min(line, total)}.{max(0, column - 1)}")
        text.see("insert")
        self._log(
            f"生成配套代码：{name} → "
            + ", ".join(f"[{section}]" for section in blocks)
        )
        return True

    def _after_return(self) -> None:
        """After an ``alloc(name, size)`` line, auto-generate its counterpart."""
        if not bool(self.settings.get("auto_pair_code", True)):
            return
        text = self.editor.text
        try:
            line = int(text.index("insert").split(".")[0])
        except (tk.TclError, ValueError):
            return
        if line <= 1:
            return
        finished = text.get(f"{line - 1}.0", f"{line - 1}.end")
        name = alloc_name(finished)
        if name:
            self.generate_pair_code(name)

    # ------------------------------------------------------------- vim / nvim
    def _configure_vim(self) -> None:
        """Wire the modal editor to the status bar and host commands."""
        vim = self.editor.vim
        vim.on_state = self._on_vim_state
        vim.on_command = self._vim_command
        vim.on_find = self.show_find
        vim.set_enabled(bool(self.settings.get("vim_mode", False)))
        self.vim_var.set(vim.enabled)

    def _on_vim_state(self, display: str) -> None:
        try:
            self.vim_label.configure(text=display)
        except tk.TclError:
            return

    def toggle_vim_mode(self) -> None:
        """Turn nvim-like modal editing on or off."""
        enabled = bool(self.vim_var.get())
        self.settings["vim_mode"] = enabled
        self.editor.vim.set_enabled(enabled)
        self._save_settings()
        self._log(f"Vim 模式：{'开启' if enabled else '关闭'}")

    def _toggle_vim_key(self) -> None:
        self.vim_var.set(not bool(self.vim_var.get()))
        self.toggle_vim_mode()

    def _vim_command(self, command: str) -> bool:
        """Handle ``:w`` / ``:q`` / ``:42`` style commands from normal mode."""
        lowered = command.strip().casefold()
        if lowered in ("w", "write"):
            self.save_file()
        elif lowered in ("q", "q!", "quit"):
            self.on_close()
        elif lowered in ("wq", "x"):
            self.save_file()
            self.on_close()
        elif lowered.isdigit():
            self.editor.goto_line(int(lowered))
        elif "nonu" in lowered or "nonumber" in lowered:
            self.numbers_var.set(False)
            self.toggle_numbers()
        elif lowered.startswith("set ") and ("nu" in lowered or "number" in lowered):
            self.numbers_var.set(True)
            self.toggle_numbers()
        else:
            self._log(f"未知 Vim 命令：{command}", logging.WARNING)
            return False
        return True

    # ------------------------------------------------------------------- view
    def set_theme(self, key: str) -> None:
        """Switch between the bundled light and dark themes."""
        theme = THEMES.get(key, self.theme)
        if theme.key == self.theme.key:
            return
        self.apply_theme(theme)
        self.theme_var.set(theme.key)
        self._log(f"切换主题：{theme.name}")

    def toggle_theme(self) -> None:
        """Flip to the other bundled theme."""
        self.set_theme("dark" if self.theme.key == "light" else "light")

    def apply_theme(self, theme: Theme) -> None:
        """Apply a theme to every widget in the window."""
        self.theme = theme
        self.highlight_overrides = self.highlight_overrides or {}
        self.configure(bg=theme.panel)
        for frame in (
            self.toolbar_wrapper,
            self.toolbar,
            self.brand_frame,
            self.status_wrapper,
            self.status,
        ):
            frame.configure(bg=theme.panel)
        self.body.configure(bg=theme.bg)
        self.edit_area.configure(bg=theme.bg)
        for rule in self.rules:
            rule.configure(bg=theme.border)
        self.brand_dot.configure(bg=theme.accent)
        self.brand.configure(
            bg=theme.panel, fg=theme.accent, font=ui_font(11, True)
        )
        for label in self.labels:
            label.configure(bg=theme.panel, fg=theme.muted, font=ui_font(9))
        # The brand is intentionally not in ``self.labels``: it keeps its own
        # accent colour instead of being overwritten by the muted loop above.
        self.mode_label.configure(
            bg=theme.accent_soft,
            fg=theme.accent,
            font=ui_font(9, True),
            padx=px(self, 8),
        )
        for label in (self.header_title, self.header_hint, self.grip):
            label.configure(bg=theme.panel, fg=theme.muted, font=ui_font(9))
        self.header_title.configure(fg=theme.text, font=ui_font(9, True))
        self.vim_label.configure(fg=theme.accent, font=ui_font(9, True))
        self.grip.configure(fg=theme.accent)
        if self.autocomplete.visible:
            self.autocomplete.hide()
        for button in self.buttons:
            button.apply_theme(theme)
        for separator in self.separators:
            separator.apply_theme(theme)
        self._style_menus(theme)
        self.editor.apply_theme(theme)
        if self.highlight_overrides:
            self.editor.apply_highlight_overrides(self.highlight_overrides)
        self.findbar.apply_theme(theme)
        self.reference.apply_theme(theme)
        self.dock.apply_theme(theme)
        self._refresh_status()

    def _style_menus(self, theme: Theme) -> None:
        options = dict(
            bg=theme.surface,
            fg=theme.text,
            activebackground=theme.accent_soft,
            activeforeground=theme.text,
            disabledforeground=theme.muted,
            font=ui_font(9),
            bd=0,
        )
        for menu in self.menus:
            try:
                menu.configure(**options)
            except tk.TclError:
                # Native (aqua) menus reject some colour options; keep working.
                continue
        if self.menubar is not None:
            try:
                self.menubar.configure(**{**options, "bg": theme.panel})
            except tk.TclError:
                pass
        for button in self.menu_buttons:
            button.configure(
                bg=theme.panel,
                fg=theme.text,
                activebackground=theme.accent_soft,
                activeforeground=theme.accent,
                font=ui_font(9),
                relief="flat",
                bd=0,
                highlightthickness=0,
                cursor="hand2",
                padx=px(button, 8),
                pady=px(button, 3),
            )

    def toggle_numbers(self) -> None:
        """Show or hide the gutter line numbers."""
        self.editor.set_show_numbers(bool(self.numbers_var.get()))

    def toggle_line_numbers(self) -> None:
        """Flip the line-number gutter from the context menu."""
        self.numbers_var.set(not bool(self.numbers_var.get()))
        self.toggle_numbers()

    def toggle_terminal(self) -> bool:
        """Show or hide the bottom terminal pane."""
        shown = self.dock.toggle_terminal()
        self._log("打开终端窗口" if shown else "关闭终端窗口")
        return shown

    def toggle_log(self) -> bool:
        """Show or hide the bottom runtime log pane."""
        shown = self.dock.toggle_log()
        self._log("打开日志窗口" if shown else "关闭日志窗口")
        return shown

    def zoom(self, direction: int) -> None:
        """Increase or decrease the editor font size."""
        self.editor.set_font_size(self.editor.font_size + direction)
        self.font_label.configure(text=f"{self.editor.font_size} pt")

    def zoom_reset(self) -> None:
        """Restore the default font size."""
        self.editor.set_font_size(int(DEFAULTS["font_size"]))
        self.font_label.configure(text="")

    def _on_wheel(self, event: tk.Event) -> str:
        direction = 1 if event.delta > 0 else -1
        self.zoom(direction)
        return "break"

    def toggle_reference(self, show: bool | None = None) -> None:
        """Show or hide the API reference panel."""
        target = (not self._reference_visible) if show is None else show
        self._show_reference(target, animate=True)
        if target and self._reference_visible:
            self.reference.focus_search()

    def _show_reference(self, show: bool, animate: bool) -> None:
        if show == self._reference_visible:
            if show:
                self.reference.focus_search()
            return
        self._reference_visible = show
        if show:
            self.ref_separator.pack(side="right", fill="y")
            collapsed = px(self.reference, 40)
            expanded = self.reference.width
            self.reference.configure(width=collapsed)
            self.reference.pack(side="right", fill="y")
            if animate:
                self._animate_reference(collapsed, expanded)
            else:
                self.reference.configure(width=expanded)
        elif animate:
            self._animate_reference(
                self.reference.width, px(self.reference, 40), then_hide=True
            )
        else:
            self.ref_separator.pack_forget()
            self.reference.pack_forget()

    def _animate_reference(
        self, start: int, end: int, then_hide: bool = False
    ) -> None:
        def frame(t: float) -> None:
            width = int(start + (end - start) * t)
            self.reference.configure(width=width)

        def done() -> None:
            if then_hide:
                self.ref_separator.pack_forget()
                self.reference.pack_forget()

        self._ref_animator.animate(
            "width", 150, frame, on_done=done, max_frames=12
        )

    # ------------------------------------------------------------------ actions
    def _undo(self) -> None:
        try:
            self.editor.text.edit_undo()
        except tk.TclError:
            pass

    def _redo(self) -> None:
        try:
            self.editor.text.edit_redo()
        except tk.TclError:
            pass

    def _event(self, virtual: str) -> None:
        self.editor.text.event_generate(virtual)
        self.editor.text.focus_set()

    def _select_all(self) -> None:
        self.editor.text.tag_add("sel", "1.0", "end-1c")
        self.editor.text.mark_set("insert", "1.0")
        self.editor.text.focus_set()
        self._on_cursor()

    def insert_api(self, item: Completion) -> None:
        """Insert a reference entry, filling template-bound arguments."""
        if item.is_call:
            argument = self._completion_argument(item.name)
            self.editor.insert_text(item.name + f"({argument or ''})")
            if not argument:
                self.editor.text.mark_set("insert", "insert-1c")
        else:
            self.editor.insert_text(item.name)
        self.editor.text.focus_set()

    def editor_toggle_comment(self) -> None:
        """Toggle comments through the editor helper."""
        self.editor.toggle_comment()
        self.editor.text.focus_set()

    def format_document(self) -> None:
        """Format the whole document."""
        self.editor.format_now()
        self.editor.text.focus_set()

    def show_find(self) -> None:
        """Open the find bar."""
        self.findbar.show(prefill=True)

    def show_replace(self) -> None:
        """Open the find bar focused on the replace field."""
        self.findbar.show(prefill=True)
        self.findbar.replace_entry.focus_set()

    def goto_line_dialog(self) -> None:
        """Ask for a line number and jump there."""
        line, _column = self.editor.position()
        answer = simpledialog.askinteger(
            "转到行",
            "行号：",
            parent=self,
            initialvalue=line,
            minvalue=1,
        )
        if answer:
            self.editor.goto_line(answer)

    def show_syntax_report(self) -> None:
        """Force a re-check and show every current diagnostic."""
        self.editor._rehighlight()
        report = check_syntax(self.editor.get_text(), None, self.syntax_options)
        checks = active_checks(self.syntax_options)
        if report.diagnostics:
            body = "\n".join(report.lines())
            title = f"语法检查报告（{len(report.diagnostics)} 项）"
        else:
            body = f"全部通过：{checks} 项检查均未发现问题。"
            title = "语法检查报告"
        self._log(f"手动语法检查：{len(report.diagnostics)} 项诊断")
        messagebox.showinfo(title, body, parent=self)

    def show_shortcuts(self) -> None:
        """Show the key binding cheat sheet."""
        messagebox.showinfo(
            "快捷键",
            "Ctrl+N 新建        Ctrl+O 打开        Ctrl+S 保存\n"
            "Ctrl+F 查找        Ctrl+H 替换        Ctrl+G 转到行\n"
            "Ctrl+Z 撤销        Ctrl+Y 重做        Ctrl+A 全选\n"
            "Ctrl+/ 切换注释    Ctrl+Shift+F 格式化\n"
            "Ctrl+Space 代码补全 F1 API 参考       Esc 关闭浮层\n"
            "Ctrl+= / Ctrl+- 调整字号   Ctrl+Shift+0 重置字号\n"
            "Ctrl+\\ 终端窗口   Ctrl+0 日志窗口    Ctrl+Q 退出\n"
            "Ctrl+, 设置      双击 选中单词    Ctrl+D 选择下一个相同词\n"
            "Ctrl+Shift+D 复制当前行   Ctrl+Alt+V 开关 Vim 模式\n\n"
            "Vim 模式：Esc 普通模式 · i/a/o/O 插入 · v/V 可视\n"
            "h j k l w b 0 $ G gg 移动 · 3j / 2dd 数字前缀\n"
            "x dd yy p/P u Ctrl+R >> << · / 查找 · :w :q :42\n\n"
            "无边框窗口：拖动顶部标题栏移动，双击最大化，右下角拖动缩放；\n"
            "所有功能也可在任意位置右键菜单中使用。\n\n"
            "模板：输入 alloc(name, 8) 后回车自动生成注册/释放代码；\n"
            "选中或复制已定义变量后，$ver$ 模板与 registersymbol() 补全自动填参。\n\n"
            "编辑辅助：输入 ( [ { \" ' 自动配对，回车自动缩进，\n"
            "选中多行后 Tab / Shift+Tab 增减缩进。",
            parent=self,
        )

    def show_about(self) -> None:
        """Show version information."""
        messagebox.showinfo(
            "关于",
            f"{APP_TITLE} {__version__}\n\n"
            "面向 Cheat Engine Lua / Auto Assembler 的轻量编辑器。\n"
            f"内置 {len(load_completions())} 条补全与参考条目。",
            parent=self,
        )

    # ----------------------------------------------------------------- highlight
    def show_highlight_settings(self) -> None:
        """Open a dialog to customise per-token-kind colours and styles.

        Changes apply immediately to the editor and are saved with the rest of
        the preferences when the window is closed.
        """
        from .themes import TOKEN_KINDS, get_theme

        base = get_theme(self.theme.key)
        theme = self.theme
        dialog = tk.Toplevel(self)
        dialog.title("代码高亮设置")
        dialog.configure(bg=theme.panel)
        dialog.transient(self)
        dialog.grab_set()
        dialog.resizable(False, False)

        container = tk.Frame(dialog, bg=theme.panel)
        container.pack(
            fill="both", expand=True, padx=px(dialog, 12), pady=px(dialog, 10)
        )

        canvas = tk.Canvas(
            container,
            bg=theme.panel,
            borderwidth=0,
            highlightthickness=0,
        )
        scrollbar = tk.Scrollbar(
            container,
            orient="vertical",
            command=canvas.yview,
            bg=theme.panel,
            activebackground=theme.border,
            troughcolor=theme.panel,
            bd=0,
            highlightthickness=0,
            elementborderwidth=0,
        )
        inner = tk.Frame(canvas, bg=theme.panel)
        inner.bind(
            "<Configure>",
            lambda _event: canvas.configure(scrollregion=canvas.bbox("all")),
        )
        canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=scrollbar.set)
        canvas.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        def wheel(_event: tk.Event) -> None:
            delta = getattr(_event, "delta", 0)
            if delta:
                canvas.yview_scroll(-1 if delta > 0 else 1, "units")
            elif getattr(_event, "num", 0) == 4:
                canvas.yview_scroll(-3, "units")
            elif getattr(_event, "num", 0) == 5:
                canvas.yview_scroll(3, "units")

        for target in (canvas, inner):
            for sequence in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
                target.bind(sequence, wheel, add="+")

        entries: dict[str, dict[str, tk.Variable]] = {}
        swatches: dict[str, tk.Label] = {}

        def kind_label(kind: str) -> str:
            pretty = {
                # Lua
                "com": "注释",
                "comdoc": "文档注释",
                "kw": "关键字",
                "kwd": "声明关键字",
                "kwop": "运算符关键字",
                "const": "常量",
                "self": "self",
                "num": "数字",
                "str": "字符串",
                "strd": "长字符串",
                "esc": "转义",
                "fn": "函数定义",
                "call": "函数调用",
                "method": "方法调用",
                "builtin": "内置函数",
                "lib": "库成员",
                "prop": "属性",
                "op": "运算符",
                "punct": "标点",
                "lbl": "标签",
                "vararg": "变长参数",
                "br0": "圆括号",
                "br1": "方括号",
                "br2": "花括号",
                "brerr": "括号错误",
                # Auto Assembler / C directives
                "aamark": "[ENABLE]/[DISABLE]",
                "aacmd": "AA 指令",
                "aadir": "{$...} 伪指令",
                "mne": "汇编指令",
                "jmp": "跳转指令",
                "asmdir": "汇编伪指令",
                "reg": "寄存器",
                "size": "操作数大小",
                "mod": "模块名",
                "wild": "通配符",
                "asmlab": "汇编标签",
                "aasym": "AA 符号",
                "aabr": "AA 标点",
            }.get(kind, kind)
            return f"{pretty} ({kind})"

        for index, kind in enumerate(TOKEN_KINDS):
            current = dict(self.highlight_overrides.get(kind, {}))
            style = base.tokens.get(kind)
            if style is None:
                continue
            fg_var = tk.StringVar(value=current.get("fg", style.fg))
            bold_var = tk.BooleanVar(value=current.get("bold", style.bold))
            italic_var = tk.BooleanVar(value=current.get("italic", style.italic))
            underline_var = tk.BooleanVar(
                value=current.get("underline", style.underline)
            )

            frame = tk.Frame(inner, bg=theme.panel)
            frame.grid(row=index, column=0, sticky="ew", pady=px(dialog, 2))
            inner.columnconfigure(0, weight=1)

            tk.Label(
                frame,
                text=kind_label(kind),
                anchor="w",
                width=26,
                bg=theme.panel,
                fg=theme.text,
                font=ui_font(9),
            ).pack(side="left", padx=(0, px(dialog, 6)))

            swatch = tk.Label(
                frame,
                text="   ",
                bg=fg_var.get(),
                relief="flat",
                bd=0,
                highlightthickness=px(dialog, 1),
                highlightbackground=theme.border,
            )
            swatch.pack(side="left", padx=(0, px(dialog, 6)))
            swatches[kind] = swatch

            def pick(colour_var=fg_var, swatch_label=swatch) -> None:
                chosen = colorchooser.askcolor(
                    color=colour_var.get(), parent=dialog, title="选择前景色"
                )
                if chosen and chosen[1]:
                    colour_var.set(chosen[1])
                    swatch_label.configure(bg=chosen[1])

            tk.Button(
                frame,
                text="颜色…",
                command=pick,
                bg=theme.surface,
                fg=theme.text,
                activebackground=theme.accent_soft,
                activeforeground=theme.text,
                relief="flat",
                bd=0,
                cursor="hand2",
                font=ui_font(9),
            ).pack(side="left", padx=(0, px(dialog, 8)))
            for label, variable in (
                ("粗体", bold_var),
                ("斜体", italic_var),
                ("下划线", underline_var),
            ):
                tk.Checkbutton(
                    frame,
                    text=label,
                    variable=variable,
                    bg=theme.panel,
                    fg=theme.text,
                    selectcolor=theme.surface,
                    activebackground=theme.panel,
                    activeforeground=theme.text,
                    highlightthickness=0,
                    bd=0,
                    font=ui_font(9),
                ).pack(side="left", padx=(0, px(dialog, 6)))

            entries[kind] = {
                "fg": fg_var,
                "bold": bold_var,
                "italic": italic_var,
                "underline": underline_var,
            }

        def apply_changes() -> None:
            overrides: dict[str, dict[str, Any]] = {}
            for kind, vars_ in entries.items():
                payload: dict[str, Any] = {"fg": vars_["fg"].get()}
                if vars_["bold"].get():
                    payload["bold"] = True
                if vars_["italic"].get():
                    payload["italic"] = True
                if vars_["underline"].get():
                    payload["underline"] = True
                overrides[kind] = payload
            self.highlight_overrides = overrides
            self.editor.apply_highlight_overrides(overrides)

        def reset_changes() -> None:
            self.highlight_overrides = {}
            self.editor.apply_theme(get_theme(self.theme.key))
            for kind, vars_ in entries.items():
                default = base.tokens.get(kind)
                if default is None:
                    continue
                vars_["fg"].set(default.fg)
                vars_["bold"].set(default.bold)
                vars_["italic"].set(default.italic)
                vars_["underline"].set(default.underline)
                swatches[kind].configure(bg=default.fg)

        button_bar = tk.Frame(dialog, bg=theme.panel)
        button_bar.pack(
            fill="x", padx=px(dialog, 12), pady=px(dialog, 8)
        )
        for text, action, kind in (
            ("应用并关闭", lambda: (apply_changes(), dialog.destroy()), "primary"),
            ("重置", reset_changes, "ghost"),
        ):
            button = HoverButton(button_bar, text, action, kind)
            button.pack(side="right", padx=(px(button, 6), 0))
            button.apply_theme(theme)

        center_window(dialog, self, width=px(dialog, 620), height=px(dialog, 680))
        dialog.wait_window(dialog)

    # ------------------------------------------------------------------- close
    def _persist(self) -> None:
        settings = load_config(self.config_file)
        settings.update(self.settings)
        settings.update(
            {
                "theme": self.theme.key,
                "font_size": self.editor.font_size,
                "show_line_numbers": self.editor.show_numbers,
                "show_reference": self._reference_visible,
                "geometry": self.geometry(),
                "last_dir": self.last_dir,
                "recent_files": self.recent,
                "highlight": self.highlight_overrides,
            }
        )
        self.settings = settings
        save_config(settings, self.config_file)

    def _cancel_jobs(self) -> None:
        """Cancel every pending ``after`` callback owned by this window."""
        for attribute in ("_complete_job", "_reposition_job"):
            job = getattr(self, attribute, None)
            if job is None:
                continue
            try:
                self.after_cancel(job)
            except tk.TclError:
                pass
            setattr(self, attribute, None)
        for name in (
            "editor",
            "findbar",
            "reference",
            "autocomplete",
            "dock",
            "_ref_animator",
        ):
            owner = getattr(self, name, None)
            if owner is None:
                continue
            cancel = getattr(owner, "cancel_pending", None) or getattr(
                owner, "cancel_all", None
            )
            if callable(cancel):
                try:
                    cancel()
                except (tk.TclError, RuntimeError):
                    continue

    def report_callback_exception(
        self, exc: type[BaseException], value: BaseException, tb: object
    ) -> None:
        """Keep the UI alive when a widget callback raises, but report it."""
        self._log(f"回调异常：{value}", logging.WARNING if issubclass(exc, tk.TclError) else logging.ERROR)
        if issubclass(exc, tk.TclError):
            # Usually a callback that outlived its widget; no need for a trace.
            return
        if sys.stderr is not None:
            traceback.print_exception(exc, value, tb, file=sys.stderr)

    def destroy(self) -> None:
        """Tear down pending timers before Tk destroys the widget tree."""
        self._cancel_jobs()
        super().destroy()

    def on_close(self) -> None:
        """Save preferences and close when safe (Ctrl+Q or WM_DELETE_WINDOW)."""
        if not self._maybe_save():
            return
        self._log("退出程序")
        self.dock.flush()
        self._persist()
        self.destroy()


def main(argv: list[str] | None = None) -> None:
    """Run the application, opening a file when one is passed from outside."""
    arguments = list(sys.argv[1:] if argv is None else argv)
    app = CeEditApp(initial_file=_initial_file(arguments))
    app.mainloop()


if __name__ == "__main__":
    main()
