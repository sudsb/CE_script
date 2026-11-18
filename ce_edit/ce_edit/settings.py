"""Independent settings window for editor behaviour, linter and templates."""

from __future__ import annotations

import copy
import tkinter as tk
from collections.abc import Callable
from tkinter import ttk
from typing import Any

from .analysis import DEFAULT_CHECK_OPTIONS
from .templates import DEFAULT_ARG_FUNCTIONS, TemplateRule, default_template_dicts
from .themes import THEMES
from .ui import center_window, px, ui_font

CATEGORIES = ("常规", "代码补全", "语法检查", "代码模板", "快捷键")
CHECK_LABELS = {
    "brackets": "括号配对与未闭合括号",
    "strings": "未闭合字符串与长注释",
    "sections": "[ENABLE] / [DISABLE] 段配对",
    "pairs": "alloc / dealloc、注册 / 取消注册配对",
    "labels": "AA 标签与跳转目标",
    "lua_blocks": "Lua function / if / for 与 end 平衡",
    "aa_blocks": "{$lua} / {$ccode} 与 {$asm} 块配对",
    "duplicates": "alloc 符号重复定义",
    "aa_statements": "无法识别的 AA 语句（半截词等）",
}
SHORTCUT_HELP = (
    "Ctrl+N / Ctrl+O / Ctrl+S     新建 / 打开 / 保存\n"
    "Ctrl+F / Ctrl+H / Ctrl+G     查找 / 替换 / 转到行\n"
    "Ctrl+Z / Ctrl+Y / Ctrl+A     撤销 / 重做 / 全选\n"
    "Ctrl+Space                   代码补全\n"
    "Ctrl+Shift+F / Ctrl+/        格式化 / 切换注释\n"
    "Ctrl+\\ / Ctrl+0              终端 / 日志窗口\n"
    "Ctrl+Shift+0                 重置字号\n"
    "Ctrl+D                       选择下一个相同词（可重复）\n"
    "Ctrl+Shift+D                 复制当前行\n"
    "Ctrl+Alt+V                   临时开关 Vim / nvim 模式\n"
    "Ctrl+Q                       退出（未保存时确认）\n"
    "\nVim 模式（nvim 风格）：\n"
    "  Esc 普通模式 · i/a/I/A/o/O 插入 · v/V 可视/可视行\n"
    "  h j k l w b 0 $ G gg 移动 · 数字前缀如 3j / 2dd\n"
    "  x 删除字符 · dd 删除行 · yy 复制行 · p/P 粘贴\n"
    "  u 撤销 · Ctrl+R 重做 · >> / << 缩进\n"
    "  / 查找 · :w 保存 · :q 退出 · :42 跳到第 42 行\n"
    "双击                         选中当前单词范围\n"
    "右键 / Shift+F10             全局菜单（含「设置」与「代码模板」）\n\n"
    "内置配对规则：alloc ↔ dealloc、registersymbol ↔ unregistersymbol。\n"
    "注册代码写入 [ENABLE]，取消注册与释放写入 [DISABLE]。\n"
    "输入 alloc(axcc, 8) 后回车即自动生成；模板可附加自定义行。\n"
    "选中单词或复制已定义变量后，$ver$ 会优先使用该值。"
)


class SettingsDialog:
    """A themed, modal preferences window independent of the main toolbar."""

    def __init__(
        self,
        app: tk.Misc,
        settings: dict[str, Any],
        on_apply: Callable[[dict[str, Any]], None],
    ) -> None:
        self.app = app
        self.theme = app.theme
        self.on_apply = on_apply
        self.data = copy.deepcopy(settings)
        self.vars: dict[str, tk.Variable] = {}
        self.check_vars: dict[str, tk.BooleanVar] = {}
        self.pages: dict[str, tk.Frame] = {}
        self.rule_index: int | None = None

        self.dialog = tk.Toplevel(app)
        self.dialog.title("设置")
        self.dialog.configure(bg=self.theme.panel)
        self.dialog.transient(app)
        self.dialog.resizable(True, True)
        self.dialog.grab_set()

        self._build()
        center_window(self.dialog, app, width=px(self.dialog, 720), height=px(self.dialog, 560))
        # Keep every keystroke (including Delete / BackSpace) inside this dialog:
        # the modal grab plus an explicit focus grab make sure nothing leaks to
        # the editor behind it.
        self.dialog.focus_force()

    # ------------------------------------------------------------------ build
    def _build(self) -> None:
        outer = tk.Frame(self.dialog, bg=self.theme.panel)
        outer.pack(fill="both", expand=True, padx=px(self.dialog, 10), pady=px(self.dialog, 10))
        self.nav = tk.Listbox(
            outer,
            width=14,
            bd=0,
            highlightthickness=0,
            exportselection=False,
            font=ui_font(10),
            bg=self.theme.surface,
            fg=self.theme.text,
            selectbackground=self.theme.accent_soft,
            selectforeground=self.theme.accent,
        )
        for name in CATEGORIES:
            self.nav.insert("end", f"  {name}")
        self.nav.pack(side="left", fill="y", padx=(0, px(self.dialog, 10)))
        self.nav.bind("<<ListboxSelect>>", self._on_navigate)

        self.page_host = tk.Frame(outer, bg=self.theme.panel)
        self.page_host.pack(side="left", fill="both", expand=True)
        for name in CATEGORIES:
            page = tk.Frame(self.page_host, bg=self.theme.panel)
            self.pages[name] = page
        self._build_general(self.pages["常规"])
        self._build_completion(self.pages["代码补全"])
        self._build_checks(self.pages["语法检查"])
        self._build_templates(self.pages["代码模板"])
        self._build_shortcuts(self.pages["快捷键"])

        bar = tk.Frame(self.dialog, bg=self.theme.panel)
        bar.pack(fill="x", padx=px(self.dialog, 10), pady=(0, px(self.dialog, 10)))
        for text, action in (
            ("保存并关闭", self._save),
            ("应用", self._apply),
            ("取消", self._cancel),
            ("恢复默认", self._restore_defaults),
        ):
            button = tk.Button(
                bar,
                text=text,
                command=action,
                bg=self.theme.surface,
                fg=self.theme.text,
                activebackground=self.theme.accent_soft,
                activeforeground=self.theme.accent,
                relief="flat",
                bd=0,
                cursor="hand2",
                font=ui_font(9),
                padx=px(bar, 12),
                pady=px(bar, 4),
            )
            button.pack(side="right", padx=px(bar, 3))
        self._select_page(0)

    def _heading(self, parent: tk.Misc, text: str) -> None:
        tk.Label(
            parent,
            text=text,
            anchor="w",
            bg=self.theme.panel,
            fg=self.theme.accent,
            font=ui_font(11, True),
        ).pack(fill="x", pady=(0, px(parent, 8)))

    def _check(self, parent: tk.Misc, key: str, label: str) -> None:
        variable = tk.BooleanVar(value=bool(self.data.get(key, False)))
        self.vars[key] = variable
        button = tk.Checkbutton(
            parent,
            text=label,
            variable=variable,
            anchor="w",
            bg=self.theme.panel,
            fg=self.theme.text,
            selectcolor=self.theme.surface,
            activebackground=self.theme.panel,
            activeforeground=self.theme.text,
            highlightthickness=0,
            bd=0,
            font=ui_font(9),
        )
        button.pack(fill="x", pady=px(parent, 2))

    def _build_general(self, page: tk.Frame) -> None:
        self._heading(page, "常规")
        theme_var = tk.StringVar(value=str(self.data.get("theme", "light")))
        self.vars["theme"] = theme_var
        for key, theme in THEMES.items():
            tk.Radiobutton(
                page,
                text=theme.name,
                value=key,
                variable=theme_var,
                anchor="w",
                bg=self.theme.panel,
                fg=self.theme.text,
                selectcolor=self.theme.surface,
                activebackground=self.theme.panel,
                highlightthickness=0,
                bd=0,
                font=ui_font(9),
            ).pack(fill="x")
        size_var = tk.StringVar(value=str(self.data.get("font_size", 12)))
        self.vars["font_size"] = size_var
        row = tk.Frame(page, bg=self.theme.panel)
        row.pack(fill="x", pady=px(page, 8))
        tk.Label(
            row, text="字号 (7-36)", bg=self.theme.panel, fg=self.theme.text, font=ui_font(9)
        ).pack(side="left")
        tk.Spinbox(
            row,
            from_=7,
            to=36,
            textvariable=size_var,
            width=5,
            bd=0,
            highlightthickness=px(row, 1),
            highlightbackground=self.theme.border,
            bg=self.theme.bg,
            fg=self.theme.text,
            font=ui_font(9),
        ).pack(side="left", padx=px(row, 8))
        self._check(page, "show_line_numbers", "显示行号")
        self._check(page, "double_click_word", "双击选中当前单词范围")
        self._check(page, "auto_pair_code", "输入 alloc(...) 后自动生成注册 / 释放代码")
        self._check(
            page,
            "vim_mode",
            "启用 Vim / nvim 模态编辑（Ctrl+Alt+V 可临时切换）",
        )

    def _build_completion(self, page: tk.Frame) -> None:
        self._heading(page, "代码补全")
        self._check(page, "dynamic_completions", "提取当前输入范围关键词并加入补全窗口")
        self._check(page, "clipboard_variable", "允许从剪贴板读取已定义变量")
        self._check(page, "auto_arguments", "函数补全时自动填入变量（registersymbol(axcc)）")
        tk.Label(
            page,
            text="自动填参函数（逗号分隔）",
            anchor="w",
            bg=self.theme.panel,
            fg=self.theme.text,
            font=ui_font(9),
        ).pack(fill="x", pady=(px(page, 10), 0))
        functions = tk.StringVar(
            value=", ".join(self.data.get("auto_arg_functions", DEFAULT_ARG_FUNCTIONS))
        )
        self.vars["auto_arg_functions"] = functions
        tk.Entry(
            page,
            textvariable=functions,
            bd=0,
            highlightthickness=px(page, 1),
            highlightbackground=self.theme.border,
            bg=self.theme.bg,
            fg=self.theme.text,
            insertbackground=self.theme.caret,
            font=ui_font(9),
        ).pack(fill="x", pady=px(page, 4))

    def _build_checks(self, page: tk.Frame) -> None:
        self._heading(page, "语法检查（可逐项关闭）")
        checks = self.data.get("syntax_checks", {})
        for key, default in DEFAULT_CHECK_OPTIONS.items():
            variable = tk.BooleanVar(value=bool(checks.get(key, default)))
            self.check_vars[key] = variable
            tk.Checkbutton(
                page,
                text=CHECK_LABELS.get(key, key),
                variable=variable,
                anchor="w",
                bg=self.theme.panel,
                fg=self.theme.text,
                selectcolor=self.theme.surface,
                activebackground=self.theme.panel,
                highlightthickness=0,
                bd=0,
                font=ui_font(9),
            ).pack(fill="x", pady=px(page, 3))

    def _build_templates(self, page: tk.Frame) -> None:
        self._heading(page, "代码模板（$ver$ 为变量占位符）")
        tk.Label(
            page,
            text=(
                "内置配对：alloc → dealloc，registersymbol → unregistersymbol；"
                "注册写入 [ENABLE]，取消注册与释放写入 [DISABLE]。"
            ),
            anchor="w",
            justify="left",
            wraplength=px(page, 520),
            bg=self.theme.panel,
            fg=self.theme.muted,
            font=ui_font(9),
        ).pack(fill="x", pady=(0, px(page, 6)))
        body = tk.Frame(page, bg=self.theme.panel)
        body.pack(fill="both", expand=True)
        left = tk.Frame(body, bg=self.theme.panel)
        left.pack(side="left", fill="y")
        self.rule_list = tk.Listbox(
            left,
            width=22,
            bd=0,
            highlightthickness=0,
            exportselection=False,
            font=ui_font(9),
            bg=self.theme.surface,
            fg=self.theme.text,
            selectbackground=self.theme.accent_soft,
            selectforeground=self.theme.accent,
        )
        self.rule_list.pack(fill="y", expand=True)
        self.rule_list.bind("<<ListboxSelect>>", self._on_rule_select)

        form = tk.Frame(body, bg=self.theme.panel)
        form.pack(side="left", fill="both", expand=True, padx=(px(body, 10), 0))
        self.rule_name = tk.StringVar()
        self.rule_trigger = tk.StringVar()
        self.rule_section_raw = tk.StringVar(value="DISABLE")
        self.rule_auto = tk.BooleanVar()
        for label, variable in (
            ("名称", self.rule_name),
            ("绑定函数（可选）", self.rule_trigger),
        ):
            tk.Label(
                form, text=label, anchor="w", bg=self.theme.panel,
                fg=self.theme.text, font=ui_font(9),
            ).pack(fill="x")
            tk.Entry(
                form,
                textvariable=variable,
                bd=0,
                highlightthickness=px(form, 1),
                highlightbackground=self.theme.border,
                bg=self.theme.bg,
                fg=self.theme.text,
                insertbackground=self.theme.caret,
                font=ui_font(9),
            ).pack(fill="x", pady=(0, px(form, 4)))
        tk.Label(
            form, text="目标区块（光标位置 / ENABLE / DISABLE / 自定义）",
            anchor="w", bg=self.theme.panel, fg=self.theme.text, font=ui_font(9),
        ).pack(fill="x")
        # Combobox: pick a built-in target or type a custom [SECTION] name. The
        # "光标位置" choice stores the CARET sentinel so the template is pasted
        # at the caret instead of a labelled block.
        self.rule_section = ttk.Combobox(
            form,
            values=("光标位置", "ENABLE", "DISABLE"),
            state="normal",
            font=ui_font(9),
            textvariable=self.rule_section_raw,
        )
        self.rule_section.pack(fill="x", pady=(0, px(form, 4)))
        tk.Label(
            form, text="模板内容", anchor="w", bg=self.theme.panel,
            fg=self.theme.text, font=ui_font(9),
        ).pack(fill="x")
        self.rule_body = tk.Text(
            form,
            height=6,
            bd=0,
            highlightthickness=px(form, 1),
            highlightbackground=self.theme.border,
            bg=self.theme.bg,
            fg=self.theme.text,
            insertbackground=self.theme.caret,
            font=ui_font(9),
            wrap="none",
        )
        self.rule_body.pack(fill="both", expand=True)
        tk.Checkbutton(
            form,
            text="自动配套生成时附加此模板（内置配对规则始终生效）",
            variable=self.rule_auto,
            anchor="w",
            bg=self.theme.panel,
            fg=self.theme.text,
            selectcolor=self.theme.surface,
            activebackground=self.theme.panel,
            highlightthickness=0,
            bd=0,
            font=ui_font(9),
        ).pack(fill="x")
        buttons = tk.Frame(form, bg=self.theme.panel)
        buttons.pack(fill="x", pady=px(form, 4))
        for text, action in (
            ("新增", self._new_rule),
            ("保存修改", self._save_rule),
            ("删除", self._delete_rule),
        ):
            tk.Button(
                buttons,
                text=text,
                command=action,
                bg=self.theme.surface,
                fg=self.theme.text,
                activebackground=self.theme.accent_soft,
                relief="flat",
                bd=0,
                cursor="hand2",
                font=ui_font(9),
            ).pack(side="left", padx=(0, px(buttons, 4)))
        self._refresh_rule_list()

    def _build_shortcuts(self, page: tk.Frame) -> None:
        self._heading(page, "快捷键与行为说明")
        text = tk.Text(
            page,
            bd=0,
            highlightthickness=0,
            bg=self.theme.bg,
            fg=self.theme.text,
            font=ui_font(9),
            wrap="word",
            padx=px(page, 8),
            pady=px(page, 8),
        )
        text.pack(fill="both", expand=True)
        text.insert("1.0", SHORTCUT_HELP)
        text.configure(state="disabled")

    # ----------------------------------------------------------------- pages
    def _select_page(self, index: int) -> None:
        for page in self.pages.values():
            page.pack_forget()
        name = CATEGORIES[max(0, min(index, len(CATEGORIES) - 1))]
        self.pages[name].pack(fill="both", expand=True)

    def _on_navigate(self, _event: tk.Event) -> None:
        selection = self.nav.curselection()
        if selection:
            self._select_page(selection[0])

    # -------------------------------------------------------------- templates
    def _rules(self) -> list[dict[str, Any]]:
        rules = self.data.get("templates")
        return rules if isinstance(rules, list) else []

    def _refresh_rule_list(self) -> None:
        self.rule_list.delete(0, "end")
        for rule in self._rules():
            self.rule_list.insert("end", str(rule.get("name", "未命名")))
        if self._rules():
            self.rule_list.selection_set(0)
            self._on_rule_select()

    def _on_rule_select(self, _event: tk.Event | None = None) -> None:
        selection = self.rule_list.curselection()
        if not selection:
            return
        self.rule_index = selection[0]
        rule = self._rules()[self.rule_index]
        self.rule_name.set(str(rule.get("name", "")))
        self.rule_trigger.set(str(rule.get("trigger", "")))
        self.rule_section_raw.set(self._section_display(rule.get("section", "DISABLE")))
        self.rule_auto.set(bool(rule.get("auto", False)))
        self.rule_body.delete("1.0", "end")
        self.rule_body.insert("1.0", str(rule.get("body", "")))

    @staticmethod
    def _section_display(raw: object) -> str:
        """Map the stored section value to the combobox display string."""
        upper = str(raw).strip().upper()
        if upper in ("CARET", "CURSOR", "光标", "光标位置"):
            return "光标位置"
        return upper or "DISABLE"

    @staticmethod
    def _section_raw(display: object) -> str:
        """Map the combobox display string back to a stored section value."""
        text = str(display).strip()
        if text in ("光标位置", "CARET", "CURSOR", "光标"):
            return "CARET"
        return text.upper() or "DISABLE"

    def _current_rule(self) -> TemplateRule | None:
        return TemplateRule.from_dict(
            {
                "name": self.rule_name.get(),
                "trigger": self.rule_trigger.get(),
                "section": self._section_raw(self.rule_section_raw.get()),
                "body": self.rule_body.get("1.0", "end-1c"),
                "auto": self.rule_auto.get(),
            }
        )

    def _new_rule(self) -> None:
        rule = self._current_rule()
        if rule is None:
            return
        rules = self._rules()
        rules.append(rule.to_dict())
        self.rule_index = len(rules) - 1
        self._refresh_rule_list()
        self.rule_list.selection_clear(0, "end")
        self.rule_list.selection_set(self.rule_index)
        self._on_rule_select()

    def _save_rule(self) -> None:
        if self.rule_index is None:
            self._new_rule()
            return
        rule = self._current_rule()
        if rule is None:
            return
        rules = self._rules()
        if self.rule_index < len(rules):
            rules[self.rule_index] = rule.to_dict()
            self._refresh_rule_list()
            self.rule_list.selection_clear(0, "end")
            self.rule_list.selection_set(self.rule_index)
            self._on_rule_select()

    def _delete_rule(self) -> None:
        if self.rule_index is None:
            return
        rules = self._rules()
        if self.rule_index < len(rules):
            rules.pop(self.rule_index)
        self.rule_index = None
        self._refresh_rule_list()

    # ----------------------------------------------------------------- actions
    def _collect(self) -> None:
        for key, variable in self.vars.items():
            if key == "font_size":
                try:
                    self.data[key] = max(7, min(36, int(variable.get())))
                except (tk.TclError, TypeError, ValueError):
                    self.data[key] = 12
            elif key == "auto_arg_functions":
                self.data[key] = [
                    part.strip()
                    for part in str(variable.get()).split(",")
                    if part.strip()
                ] or list(DEFAULT_ARG_FUNCTIONS)
            else:
                self.data[key] = variable.get()
        self.data["syntax_checks"] = {
            key: bool(variable.get())
            for key, variable in self.check_vars.items()
        }
        if self.rule_index is not None:
            rule = self._current_rule()
            if rule is not None and self.rule_index < len(self._rules()):
                self._rules()[self.rule_index] = rule.to_dict()
        self.data["templates"] = self._rules() or default_template_dicts()

    def _apply(self) -> None:
        self._collect()
        self.on_apply(copy.deepcopy(self.data))

    def _save(self) -> None:
        self._apply()
        self.dialog.grab_release()
        self.dialog.destroy()

    def _cancel(self) -> None:
        self.dialog.grab_release()
        self.dialog.destroy()

    def _restore_defaults(self) -> None:
        from .config import DEFAULTS

        defaults = copy.deepcopy(DEFAULTS)
        for key, variable in self.vars.items():
            value = defaults.get(key)
            if key == "auto_arg_functions" and isinstance(value, list):
                variable.set(", ".join(value))
            elif value is not None:
                variable.set(value)
        for key, variable in self.check_vars.items():
            variable.set(bool(DEFAULT_CHECK_OPTIONS.get(key, True)))
        self.data["templates"] = default_template_dicts()
        self.rule_index = None
        self._refresh_rule_list()

    def show(self) -> None:
        """Block until the dialog closes (Tk's wait_window loop)."""
        self.dialog.wait_window(self.dialog)
