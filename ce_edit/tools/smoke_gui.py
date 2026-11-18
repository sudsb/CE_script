"""Smoke-test the Tk application without user interaction."""

from __future__ import annotations

import tempfile
import tkinter as tk
from pathlib import Path
from types import SimpleNamespace

from ce_edit.app import CeEditApp
from ce_edit.completions import Completion
from ce_edit.settings import SettingsDialog
from ce_edit.templates import TemplateRule
from ce_edit.ui import center_window


def main() -> int:
    config = Path(tempfile.gettempdir()) / "ce_edit_smoke_config.json"
    if config.exists():
        config.unlink()
    app = CeEditApp(config)
    app.update()
    app.set_theme("dark")
    app.update()
    app.set_theme("light")
    app.update()

    app.show_find()
    app.findbar.find_var.set("jmp")
    app.findbar.refresh()
    app.findbar.find_next()
    app.update()
    app.findbar.replace_var.set("jump")
    app.findbar.replace_current()
    app.update()
    app.findbar.hide()

    app.toggle_reference(True)
    app.reference.query = "mov"
    app.reference.rebuild()
    app.update()
    app.reference.clear_filters()
    app.toggle_reference(False)
    app.update()

    app.editor.goto_line(5)
    app.editor.toggle_comment()
    app.update()
    app.editor.toggle_comment()
    app.update()

    document = app.editor.get_text()
    app.editor.set_text("readIn")
    app.editor.text.mark_set("insert", "end-1c")
    app.autocomplete.request(force=True)
    app.update()
    assert app.autocomplete.visible
    app.autocomplete.reposition()
    app.autocomplete._move(1)
    app.autocomplete.hide()
    app.editor.set_text(document)

    assert app.brand.master is app.brand_frame
    assert bool(app.overrideredirect())
    assert not app.buttons and not app.menu_buttons
    assert app.editor.text.tag_names()[-1] == "sel"
    for menu in (app.editor_menu, app.entry_menu, app.readonly_menu, app.tree_menu):
        assert menu.index("end")
    for widget in (app.editor.text, app.editor.gutter, app.status, app.toolbar):
        assert widget.bind("<Button-3>")
    app.editor.apply_highlight_overrides({"kw": {"fg": "#123456", "bold": False}})

    # Right-click menu commands must really execute.
    labels = []
    for index in range(int(app.editor_menu.index("end") or 0) + 1):
        try:
            labels.append(str(app.editor_menu.entrycget(index, "label")))
        except tk.TclError:
            labels.append("---")
    app.context_target = app.editor.text
    app.editor_menu.invoke(labels.index("全选"))
    assert len(app.editor.text.tag_ranges("sel")) == 2
    app.editor.text.tag_remove("sel", "1.0", "end")

    # Bottom dock: terminal, log, drag resize and runtime logging.
    app.editor.set_text(document)
    assert app.toggle_terminal() is True
    app.toggle_log()
    app.update()
    assert app.dock.frame.winfo_height() > 100
    app.dock.term_entry.insert(0, "help")
    app.dock._submit()
    assert "可用命令" in app.dock.term_text.get("1.0", "end")
    app.dock.flush()
    assert "启动" in app.dock.log_text.get("1.0", "end")
    app.dock.hide_all()
    app.update()

    # Opening a file from an external caller is logged.
    external = Path(tempfile.gettempdir()) / "ce_edit_smoke_external.lua"
    external.write_text("print('external')\n", encoding="utf-8")
    assert app.open_file(str(external))
    assert "external" in app.editor.get_text()
    app.dock.flush()
    assert str(external) in app.dock.log_text.get("1.0", "end")
    external.unlink(missing_ok=True)
    app.editor.set_text(document)

    dialog = tk.Toplevel(app)
    center_window(dialog, app, width=320, height=240)
    dialog.destroy()

    def close_dialogs() -> None:
        for child in app.winfo_children():
            if isinstance(child, tk.Toplevel):
                child.destroy()

    app.after(120, close_dialogs)
    app.show_highlight_settings()
    app.update()

    app.zoom(2)
    app.zoom_reset()
    app.toggle_numbers()
    app.toggle_numbers()
    app.update()

    text = app.editor.get_text()
    assert "jump" in text

    # Strengthened linter, word selection, dynamic keywords and templates.
    assert str(app.lint_label.cget("text")).startswith(("✓", "ⓘ", "⚠"))
    assert int(app.template_menu.index("end") or 0) > 0
    assert "syntax_checks" in app.settings

    app.editor.set_text("alloc(axcc, 8)\n")
    app.update()
    info = app.editor.text.dlineinfo("1.0")
    assert info is not None
    app.editor._on_double_click(SimpleNamespace(x=45, y=info[1] + info[3] // 2))
    ranges = app.editor.text.tag_ranges("sel")
    assert len(ranges) == 2
    app.editor.text.tag_remove("sel", "1.0", "end")

    app.editor.set_text("[ENABLE]\nalloc(axcc，8)\nax")
    app.editor.text.mark_set("insert", "3.2")
    app.autocomplete.request(force=True)
    app.update()
    assert app.autocomplete.items[0].name == "axcc"
    app.autocomplete.hide()

    app.editor.set_text("[ENABLE]\nalloc(axcc，8)\nreg")
    app.editor.text.mark_set("insert", "3.3")
    app.clipboard_clear()
    app.clipboard_append("axcc")
    app.autocomplete.items = [
        Completion("registersymbol", "registersymbol(name)", "自动汇编")
    ]
    app.autocomplete._accept(0)
    assert app.editor.get_text().splitlines()[-1] == "registersymbol(axcc)"

    app.editor.set_text("[ENABLE]\nalloc(axcc，8)\n[DISABLE]\n")
    app.editor.text.mark_set("insert", "3.0")
    app._after_return()
    app.update()
    assert all(
        item in app.editor.get_text()
        for item in ("registersymbol(axcc)", "unregistersymbol(axcc)", "dealloc(axcc)")
    )
    app.insert_template(
        TemplateRule("模板函数1", "dealloc", "dealloc($ver$)", "DISABLE")
    )
    settings_dialog = SettingsDialog(app, app.settings, lambda _data: None)
    settings_dialog.dialog.destroy()
    app.dock.flush()
    assert "生成配套代码" in app.dock.log_text.get("1.0", "end")

    # No CE keyword / half-typed word fragments in the completion list.
    from ce_edit.analysis import document_keywords

    sample = "[ENABLE]\nnewmem:\nmov [player_hp], eax\njmp return\nnewm"
    keywords = document_keywords(sample, len(sample) - 2, "aa")
    assert "newm" not in keywords and "mov" not in keywords
    assert "newmem" in keywords

    # Syntax check reports a bare AA statement and shows its check count.
    app.editor.set_text("[ENABLE]\nne\n[DISABLE]\n")
    app.editor._rehighlight()
    app.update()
    assert "无法识别" in str(app.lint_label.cget("text"))
    app.editor.set_text("local x = 1\n")
    app.editor._rehighlight()
    app.update()
    assert "项检查" in str(app.lint_label.cget("text"))

    # Ctrl+D multi-selection.
    app.editor.set_text("alpha beta alpha gamma alpha\n")
    app.editor.text.mark_set("insert", "1.2")
    app.editor._on_select_next()
    app.editor._on_select_next()
    assert len(app.editor.text.tag_ranges("sel")) == 4
    app.editor._on_select_next()
    app.editor.replace_selections("X")
    assert app.editor.get_text() == "X beta X gamma X\n"

    # Vim / nvim modal editing.
    app.vim_var.set(True)
    app.toggle_vim_mode()
    vim = app.editor.vim
    assert vim.state == "normal"
    app.editor.set_text("one\ntwo\nthree\n")
    app.editor.text.mark_set("insert", "1.0")
    vim.handle(SimpleNamespace(char="j", keysym="j", state=0))
    assert app.editor.text.index("insert") == "2.0"
    vim.handle(SimpleNamespace(char="d", keysym="d", state=0))
    vim.handle(SimpleNamespace(char="d", keysym="d", state=0))
    assert app.editor.get_text() == "one\nthree\n"
    app.vim_var.set(False)
    app.toggle_vim_mode()
    assert vim.state == "insert"

    app._cancel_jobs()
    app.destroy()
    config.unlink(missing_ok=True)
    print("smoke OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
