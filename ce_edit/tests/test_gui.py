"""Headless GUI tests, skipped when no Tk display is available."""

from __future__ import annotations

import ctypes
import re
import sys
import tkinter as tk
from tkinter import messagebox
from types import SimpleNamespace

import pytest

from ce_edit.app import WELCOME, CeEditApp, _initial_file
from ce_edit.completions import Completion
from ce_edit.config import DEFAULTS
from ce_edit.templates import DEFAULT_TEMPLATES, TemplateRule, parse_templates
from ce_edit.ui import center_window, work_area

pytestmark = pytest.mark.gui


@pytest.fixture(scope="module")
def app(tmp_path_factory: pytest.TempPathFactory):
    # Reuse one Tk root: repeatedly creating/destroying interpreters in a
    # single process makes some Tk builds lose their init.tcl path.
    try:
        instance = CeEditApp(tmp_path_factory.mktemp("ce_edit") / "config.json")
    except tk.TclError as exc:
        pytest.skip(f"no Tk display available: {exc}")
    instance.update()
    yield instance
    try:
        instance.destroy()
    except tk.TclError:
        pass


@pytest.fixture(autouse=True)
def _reset_state(app: CeEditApp) -> None:
    app.set_theme("light")
    app.autocomplete.hide()
    app.findbar.hide()
    app.reference.clear_filters()
    if app._reference_visible:
        app._show_reference(False, animate=False)
    app.numbers_var.set(True)
    app.editor.set_show_numbers(True)
    app.zoom_reset()
    app.dock.hide_all()
    app.settings = dict(DEFAULTS)
    app.vim_var.set(False)
    app.editor.vim.set_enabled(False)
    app.editor.text.tag_remove("sel", "1.0", "end")
    app.syntax_options = dict(DEFAULTS["syntax_checks"])
    app.templates = parse_templates(DEFAULTS["templates"])
    app.editor.double_click_word = True
    app._rebuild_template_menu()
    app.editor.set_text(WELCOME)
    app.dirty = False
    app.path = None
    app.update()


def test_startup_and_themes(app: CeEditApp) -> None:
    assert app.editor.get_text().strip()
    app.set_theme("dark")
    app.update()
    app.set_theme("light")
    app.update()
    assert app.config is not None


def test_format_action(app: CeEditApp) -> None:
    app.editor.set_text("local x=1\nif x>0 then\nprint( 'hi' )\nend\n")
    app.format_document()
    app.update()
    assert app.editor.get_text() == (
        "local x = 1\nif x > 0 then\n  print('hi')\nend\n"
    )


def test_comment_toggle_from_app(app: CeEditApp) -> None:
    app.editor.set_text("local a = 1\nprint(a)\n")
    app.editor.text.tag_add("sel", "1.0", "end-1c")
    app.editor.toggle_comment()
    app.update()
    assert app.editor.get_text() == "-- local a = 1\n-- print(a)\n"
    app.editor.toggle_comment()
    app.update()
    assert app.editor.get_text() == "local a = 1\nprint(a)\n"


def test_find_and_replace(app: CeEditApp) -> None:
    app.editor.set_text("jmp return\nmov eax, 1\njmp done\n")
    app.show_find()
    app.findbar.find_var.set("jmp")
    app.findbar.refresh()
    app.findbar.find_next()
    app.update()
    assert app.findbar.count_label.cget("text") != "0/0"
    app.findbar.replace_var.set("jump")
    app.findbar.replace_current()
    app.update()
    assert "jump" in app.editor.get_text()
    app.findbar.hide()


def test_reference_panel_filters(app: CeEditApp) -> None:
    app.toggle_reference(True)
    app.reference.query = "readInteger"
    app.reference.rebuild()
    app.update()
    app.reference.clear_filters()
    app.reference.query = "definitely_missing_entry"
    app.reference.rebuild()
    app.update()
    app.toggle_reference(False)


def test_autocomplete_popup(app: CeEditApp) -> None:
    app.editor.set_text("readIn")
    app.editor.text.mark_set("insert", "end-1c")
    app.editor.text.focus_set()
    app.autocomplete.request(force=True)
    app.update()
    assert app.autocomplete.items
    app.autocomplete._move(1)
    app.autocomplete.hide()


def test_zoom_and_line_numbers(app: CeEditApp) -> None:
    app.zoom(2)
    app.zoom(2)
    app.zoom(-1)
    app.update()
    app.zoom_reset()
    app.toggle_numbers()
    app.toggle_numbers()
    app.update()
    assert app.editor.show_numbers is True


def _parse_geometry(value: str) -> tuple[int, int, int, int]:
    match = re.fullmatch(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", value)
    assert match is not None, value
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def test_frameless_window_has_no_buttons(app: CeEditApp) -> None:
    assert bool(app.overrideredirect()) is True
    assert app.buttons == []
    assert app.menu_buttons == []
    assert not any(isinstance(child, tk.Button) for child in app.toolbar.winfo_children())
    assert app.brand_frame.master is app.toolbar
    assert app.brand.master is app.brand_frame
    assert str(app.brand.cget("fg")) == str(app.theme.accent)


def test_context_menus_are_bound_everywhere(app: CeEditApp) -> None:
    for menu in (app.editor_menu, app.entry_menu, app.readonly_menu, app.tree_menu):
        assert isinstance(menu, tk.Menu)
        assert int(menu.index("end") or 0) > 0
        assert menu in app.menus
    for widget in (
        app.editor.text,
        app.editor.gutter,
        app.status,
        app.toolbar,
        app.dock.term_text,
        app.dock.term_entry,
    ):
        assert widget.bind("<Button-3>")


def test_context_menu_commands_execute(app: CeEditApp) -> None:
    labels = []
    for index in range(int(app.editor_menu.index("end") or 0) + 1):
        try:
            labels.append(str(app.editor_menu.entrycget(index, "label")))
        except tk.TclError:
            labels.append("---")
    assert {"终端窗口", "日志窗口", "退出", "全选"} <= set(labels)
    app.editor.set_text("hello world")
    app.context_target = app.editor.text
    app.editor_menu.invoke(labels.index("全选"))
    assert len(app.editor.text.tag_ranges("sel")) == 2
    app.editor.text.tag_remove("sel", "1.0", "end")


def test_selection_tag_has_highest_priority(app: CeEditApp) -> None:
    # Otherwise curline / find / same-word backgrounds hide the drag selection.
    assert app.editor.text.tag_names()[-1] == "sel"
    app.editor._rehighlight()
    assert app.editor.text.tag_names()[-1] == "sel"
    app.editor.text.tag_add("sel", "1.0", "1.4")
    assert app.editor.selection_size() == 4
    app.editor.text.tag_remove("sel", "1.0", "end")


def test_mouse_drag_selects_text(app: CeEditApp) -> None:
    app.editor.set_text("abcdefghij\nsecond line\n")
    app.update()
    info = app.editor.text.dlineinfo("1.0")
    assert info is not None
    y = info[1] + info[3] // 2
    text = app.editor.text
    text.event_generate("<Button-1>", x=20, y=y)
    text.event_generate("<B1-Motion>", x=160, y=y)
    text.event_generate("<ButtonRelease-1>", x=160, y=y)
    app.update()
    assert len(text.tag_ranges("sel")) == 2
    start, end = (str(index) for index in text.tag_ranges("sel"))
    assert start.startswith("1.") and end != start
    assert text.index("insert") == end
    assert text.get(start, end)
    text.tag_remove("sel", "1.0", "end")


def test_ctrl_q_prompts_for_unsaved_changes(app: CeEditApp, monkeypatch) -> None:
    answers = []
    calls = []

    def fake_prompt(*_args, **_kwargs):
        calls.append(True)
        return answers.pop(0)

    monkeypatch.setattr(app, "_persist", lambda: None)
    monkeypatch.setattr(app, "destroy", lambda: calls.append("closed"))
    monkeypatch.setattr(messagebox, "askyesnocancel", fake_prompt)
    answers.append(None)
    app.dirty = True
    app.on_close()
    assert calls == [True]
    answers.append(False)
    app.on_close()
    assert calls == [True, True, "closed"]


def test_main_window_opens_inside_work_area(app: CeEditApp) -> None:
    width, height, x, y = _parse_geometry(app.geometry())
    left, top, right, bottom = work_area(app, x + width // 2, y + height // 2)
    assert left <= x and top <= y
    assert x + width <= right and y + height <= bottom


def test_completion_popup_stays_inside_work_area(app: CeEditApp) -> None:
    app.editor.set_text("readIn")
    app.editor.text.mark_set("insert", "end-1c")
    app.autocomplete.request(force=True)
    app.update()
    popup = app.autocomplete.popup
    assert popup is not None and app.autocomplete.visible
    width, height, x, y = _parse_geometry(popup.geometry())
    left, top, right, bottom = work_area(popup, x, y)
    assert left <= x and top <= y
    assert x + width <= right and y + height <= bottom
    app.autocomplete.reposition()
    app.autocomplete.hide()


def test_center_window(app: CeEditApp) -> None:
    dialog = tk.Toplevel(app)
    x, y = center_window(dialog, app, width=320, height=240)
    app.update()
    width, height, parsed_x, parsed_y = _parse_geometry(dialog.geometry())
    assert (parsed_x, parsed_y) == (x, y)
    assert width == 320 and height == 240
    dialog.destroy()


def test_ctrl_shortcuts_open_docks(app: CeEditApp, monkeypatch) -> None:
    hits: list[str] = []
    monkeypatch.setattr(app, "toggle_terminal", lambda: hits.append("terminal") or True)
    monkeypatch.setattr(app, "toggle_log", lambda: hits.append("log") or True)
    monkeypatch.setattr(app, "zoom_reset", lambda: hits.append("zoom"))
    app.event_generate("<Control-backslash>")
    app.event_generate("<Control-0>")
    app.event_generate("<Control-Shift-0>")
    assert hits[:2] == ["terminal", "log"]
    assert "zoom" in hits


def test_dock_terminal_log_and_resize(app: CeEditApp) -> None:
    body_before = app.body.winfo_height()
    assert app.toggle_terminal() is True
    app.toggle_log()
    app.update()
    assert app.dock.visible == {"terminal", "log"}
    assert app.dock.frame.winfo_height() > 100
    assert app.body.winfo_height() < body_before
    app.dock.term_entry.insert(0, "help")
    app.dock._submit()
    assert "可用命令" in app.dock.term_text.get("1.0", "end")
    app.dock._log_queue.put(
        _record("打开 /tmp/example.lua")
    )
    app.dock.flush()
    assert "/tmp/example.lua" in app.dock.log_text.get("1.0", "end")
    app.dock.hide_all()
    app.update()
    assert not app.dock.visible
    assert app.body.winfo_height() == body_before


def _record(message: str):
    import logging

    return logging.getLogger("ce_edit").makeRecord(
        "ce_edit", logging.INFO, __file__, 0, message, (), None
    )


def test_external_file_argument_and_open(app: CeEditApp, tmp_path) -> None:
    target = tmp_path / "external.lua"
    target.write_text("print('external')\n", encoding="utf-8")
    assert _initial_file(["--flag", str(target)]) == str(target.resolve())
    assert _initial_file([target.as_uri()]) == str(target.resolve())
    assert _initial_file([str(tmp_path / "missing.lua")]) is None
    assert app.open_file(str(target)) is True
    assert app.editor.get_text().strip() == "print('external')"
    app.dock.flush()
    assert str(target) in app.dock.log_text.get("1.0", "end")


def test_popup_menu_keyboard_anchor(app: CeEditApp, monkeypatch) -> None:
    from ce_edit.ui import popup_menu

    positions: list[tuple[int, int]] = []
    monkeypatch.setattr(tk.Menu, "tk_popup", lambda self, x, y: positions.append((x, y)))
    popup_menu(
        app.editor_menu,
        app.editor.text,
        SimpleNamespace(type="2", num="??", x_root=0, y_root=0),
    )
    popup_menu(
        app.editor_menu,
        app.editor.text,
        SimpleNamespace(type="4", num=3, x_root=600, y_root=500),
    )
    assert len(positions) == 2
    assert positions[1] != (0, 0)
    assert positions[0][0] > 0 and positions[0][1] > 0


def test_settings_and_template_menu(app: CeEditApp) -> None:
    assert app.settings["dynamic_completions"] is True
    assert app.settings["syntax_checks"]["brackets"] is True
    assert len(app.templates) == len(DEFAULT_TEMPLATES)
    assert int(app.template_menu.index("end") or 0) > 0
    labels = []
    for index in range(int(app.editor_menu.index("end") or 0) + 1):
        try:
            labels.append(str(app.editor_menu.entrycget(index, "label")))
        except tk.TclError:
            labels.append("---")
    assert {"设置…", "生成配套代码", "代码模板"} <= set(labels)
    assert app.bind("<Control-comma>")


def test_double_click_selects_word(app: CeEditApp) -> None:
    app.editor.set_text("alloc(axcc, 8)\n")
    app.update()
    info = app.editor.text.dlineinfo("1.0")
    assert info is not None
    event = SimpleNamespace(x=45, y=info[1] + info[3] // 2)
    assert app.editor._on_double_click(event) == "break"
    ranges = app.editor.text.tag_ranges("sel")
    assert len(ranges) == 2
    assert str(app.editor.text.get(ranges[0], ranges[1])) == "alloc"
    app.editor.double_click_word = False
    app.editor.text.tag_remove("sel", "1.0", "end")
    assert app.editor._on_double_click(event) is None
    app.editor.double_click_word = True


def test_dynamic_keywords_lead_the_completion_popup(app: CeEditApp) -> None:
    app.editor.set_text("[ENABLE]\nalloc(axcc，8)\nax")
    app.editor.text.mark_set("insert", "3.2")
    app.autocomplete.request(force=True)
    app.update()
    assert app.autocomplete.items
    assert app.autocomplete.items[0].name == "axcc"
    assert app.autocomplete.items[0].category == "文档符号"
    app.autocomplete.hide()


def test_template_bound_function_completion(app: CeEditApp) -> None:
    app.editor.set_text("[ENABLE]\nalloc(axcc，8)\nreg")
    app.editor.text.mark_set("insert", "3.3")
    app.clipboard_clear()
    app.clipboard_append("axcc")
    app.autocomplete.items = [
        Completion("registersymbol", "registersymbol(name)", "自动汇编")
    ]
    app.autocomplete._accept(0)
    assert app.editor.get_text().splitlines()[-1] == "registersymbol(axcc)"


def test_auto_pair_code_after_alloc(app: CeEditApp) -> None:
    app.editor.set_text("[ENABLE]\nalloc(axcc，8)\n[DISABLE]\n")
    app.editor.text.mark_set("insert", "3.0")
    assert app._after_return() is None
    app.update()
    source = app.editor.get_text()
    assert "registersymbol(axcc)" in source
    assert "unregistersymbol(axcc)" in source
    assert "dealloc(axcc)" in source
    assert "，" in source
    app.dock.flush()
    assert "生成配套代码" in app.dock.log_text.get("1.0", "end")
    # A second run must not duplicate anything.
    snapshot = app.editor.get_text()
    app._after_return()
    app.update()
    assert app.editor.get_text() == snapshot


def test_manual_template_and_settings_dialog(app: CeEditApp) -> None:
    from ce_edit.settings import SettingsDialog

    app.editor.set_text("[ENABLE]\nalloc(axcc, 8)\n")
    rule = TemplateRule("模板函数1", "dealloc", "dealloc($ver$)", "DISABLE")
    app.insert_template(rule)
    assert "dealloc(axcc)" in app.editor.get_text()
    dialog = SettingsDialog(app, app.settings, lambda _data: None)
    assert set(dialog.pages) == set(
        ("常规", "代码补全", "语法检查", "代码模板", "快捷键")
    )
    dialog.dialog.destroy()


def test_apply_settings_updates_editor(app: CeEditApp, monkeypatch) -> None:
    monkeypatch.setattr(app, "_save_settings", lambda: None)
    updated = dict(app.settings)
    updated["double_click_word"] = False
    checks = dict(updated["syntax_checks"])
    checks["pairs"] = False
    updated["syntax_checks"] = checks
    app.apply_settings(updated)
    assert app.editor.double_click_word is False
    assert app.syntax_options["pairs"] is False
    app.apply_settings(dict(DEFAULTS))
    assert app.editor.double_click_word is True


def test_syntax_status_label_reports_problems(app: CeEditApp) -> None:
    app.editor.set_text("if x > 0 then\n  print(x)\n")
    app.editor._rehighlight()
    app.update()
    assert str(app.lint_label.cget("text")).startswith(("ⓘ", "⚠"))
    app.editor.set_text("local x = 1\n")
    app.editor._rehighlight()
    app.update()
    assert str(app.lint_label.cget("text")).startswith("✓")


def test_custom_bound_template_expands_completion(app: CeEditApp) -> None:
    bound = TemplateRule(
        "自定义绑定", "myRegister", "myRegister($ver$)", "ENABLE"
    )
    app.templates = (*app.templates, bound)
    app.settings = dict(app.settings)
    app.settings["auto_arg_functions"] = ["registersymbol"]
    app.editor.set_text("[ENABLE]\nalloc(axcc, 8)\nmy")
    app.editor.text.mark_set("insert", "3.2")
    app.clipboard_clear()
    app.clipboard_append("axcc")
    app.autocomplete.items = [Completion("myRegister", "myRegister(name)", "测试")]
    app.autocomplete._accept(0)
    assert app.editor.get_text().splitlines()[-1] == "myRegister(axcc)"


def test_settings_dialog_edits_templates_and_applies(app: CeEditApp) -> None:
    from ce_edit.settings import SettingsDialog

    applied: list[dict] = []
    dialog = SettingsDialog(app, app.settings, applied.append)
    dialog.vars["double_click_word"].set(False)
    dialog.rule_name.set("新模板")
    dialog.rule_trigger.set("myFunc")
    dialog.rule_section.set("ENABLE")
    dialog.rule_auto.set(True)
    dialog.rule_body.delete("1.0", "end")
    dialog.rule_body.insert("1.0", "myFunc($ver$)")
    dialog._new_rule()
    assert len(dialog.data["templates"]) == len(DEFAULT_TEMPLATES) + 1
    dialog._save()
    assert applied
    assert applied[0]["double_click_word"] is False
    assert applied[0]["templates"][-1]["trigger"] == "myFunc"


def test_show_settings_can_open_and_close(app: CeEditApp) -> None:
    def close_settings() -> None:
        for child in app.winfo_children():
            if isinstance(child, tk.Toplevel) and child.title() == "设置":
                child.destroy()

    app.after(60, close_settings)
    app.show_settings()
    app.update()
    assert not any(
        isinstance(child, tk.Toplevel) and child.title() == "设置"
        for child in app.winfo_children()
    )


def test_ctrl_d_selects_each_next_occurrence(app: CeEditApp) -> None:
    app.editor.set_text("alpha beta alpha gamma alpha\n")
    app.editor.text.mark_set("insert", "1.2")
    app.editor._on_select_next()
    assert len(app.editor.text.tag_ranges("sel")) == 2
    app.editor._on_select_next()
    assert len(app.editor.text.tag_ranges("sel")) == 4
    assert app.editor.selection_size() == 10
    app.editor._on_select_next()
    app.editor.replace_selections("X")
    assert app.editor.get_text() == "X beta X gamma X\n"
    # Line duplication moved off Ctrl+D.
    assert app.editor.text.bind("<Control-Shift-D>")
    assert app.editor.text.bind("<Control-d>")


def test_vim_modes_and_keys(app: CeEditApp) -> None:
    vim = app.editor.vim
    app.vim_var.set(True)
    app.toggle_vim_mode()
    assert vim.enabled and vim.state == "normal"
    assert app.vim_label.cget("text") == "普通模式"

    app.editor.set_text("one\ntwo\nthree\n")
    app.editor.text.mark_set("insert", "1.0")

    def key(char: str, keysym: str | None = None, state: int = 0) -> None:
        vim.handle(SimpleNamespace(char=char, keysym=keysym or char, state=state))

    key("j")
    assert app.editor.text.index("insert") == "2.0"
    key("d")
    key("d")
    assert app.editor.get_text() == "one\nthree\n"
    key("i")
    assert vim.state == "insert"
    vim.handle(SimpleNamespace(char="", keysym="Escape", state=0))
    assert vim.state == "normal"
    key("v")
    assert vim.state == "visual"
    assert len(app.editor.text.tag_ranges("sel")) == 2
    assert vim.handle_escape() is True
    assert vim.state == "normal"
    assert app._vim_command("42") is True
    assert app.editor.position()[0] == 3


def test_syntax_label_shows_check_count(app: CeEditApp) -> None:
    app.editor.set_text("[ENABLE]\nne\n[DISABLE]\n")
    app.editor._rehighlight()
    app.update()
    assert "无法识别" in str(app.lint_label.cget("text"))
    app.editor.set_text("local x = 1\n")
    app.editor._rehighlight()
    app.update()
    assert "项检查" in str(app.lint_label.cget("text"))


@pytest.mark.skipif(sys.platform != "win32", reason="Windows-only taskbar style")
def test_frameless_window_has_taskbar_entry(app: CeEditApp) -> None:
    from ce_edit.ui import _windows_taskbar_entry

    app.update()
    _windows_taskbar_entry(app)
    app.update()
    user32 = ctypes.windll.user32
    wrapper = user32.GetParent(app.winfo_id())
    style = user32.GetWindowLongW(wrapper, -20) & 0xFFFFFFFF
    assert style & 0x00040000  # WS_EX_APPWINDOW
    assert not style & 0x00000080  # WS_EX_TOOLWINDOW
    assert style & 0x00080000  # WS_EX_LAYERED (enables the drop shadow)
    assert bool(user32.IsWindowVisible(wrapper))


def test_pending_jobs_cleanup(app: CeEditApp) -> None:
    app._cancel_jobs()
    app._cancel_jobs()
