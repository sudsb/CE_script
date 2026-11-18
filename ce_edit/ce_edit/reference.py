"""The searchable Cheat Engine API reference panel."""

from __future__ import annotations

import tkinter as tk
from collections.abc import Callable
from tkinter import ttk

from .completions import (
    Completion,
    category_order,
    grouped_reference,
    load_completions,
)
from .themes import Theme
from .ui import px, ui_font

WIDTH = 340


class ReferencePanel(tk.Frame):
    """A grouped, filterable list of CE and Lua API entries."""

    def __init__(
        self,
        master: tk.Misc,
        theme: Theme,
        on_insert: Callable[[Completion], None] | None = None,
    ) -> None:
        super().__init__(master, bd=0)
        self.width = px(self, WIDTH)
        self.configure(width=self.width)
        self.theme = theme
        self.on_insert = on_insert
        self.query = ""
        self.category: str | None = None
        self._by_iid: dict[str, Completion] = {}
        self._groups = grouped_reference()
        self._search_job: str | None = None
        self._style = ttk.Style(self)
        try:
            self._style.theme_use("clam")
        except tk.TclError:
            pass
        self.pack_propagate(False)
        self._build()
        self.apply_theme(theme)
        self.rebuild()

    # ------------------------------------------------------------------ build
    def _build(self) -> None:
        header = tk.Frame(self)
        header.pack(fill="x", padx=12, pady=(10, 4))
        self.title = tk.Label(header, text="API 参考", anchor="w")
        self.title.pack(side="left")
        self.count = tk.Label(header, text="", anchor="e")
        self.count.pack(side="right")

        search_row = tk.Frame(self)
        search_row.pack(fill="x", padx=12, pady=(2, 4))
        self.search_entry = tk.Entry(search_row, bd=0)
        self.search_entry.pack(side="left", fill="x", expand=True, ipady=4)
        self.search_entry.bind("<KeyRelease>", self._on_search)
        self.search_entry.bind("<Return>", self._insert_first)
        self.search_entry.bind("<Down>", self._focus_tree)

        filter_row = tk.Frame(self)
        filter_row.pack(fill="x", padx=12, pady=(0, 6))
        self.category_button = tk.Menubutton(filter_row, text="全部分类", anchor="w")
        self.category_button.pack(side="left", fill="x", expand=True)
        self.category_menu = tk.Menu(self.category_button, tearoff=False)
        self.category_button.configure(menu=self.category_menu)
        self._fill_categories()

        tree_frame = tk.Frame(self)
        tree_frame.pack(fill="both", expand=True, padx=(6, 0), pady=(0, 4))
        self.tree = ttk.Treeview(
            tree_frame,
            show="tree",
            style="Ref.Treeview",
            selectmode="browse",
        )
        self.tree.column("#0", width=self.width - px(self, 40), stretch=True)
        self.tree.pack(side="left", fill="both", expand=True)
        self.scroll = tk.Scrollbar(
            tree_frame, orient="vertical", command=self.tree.yview
        )
        self.scroll.pack(side="right", fill="y")
        self.tree.configure(yscrollcommand=self.scroll.set)
        self.tree.bind("<<TreeviewSelect>>", self._on_select)
        self.tree.bind("<Double-Button-1>", self._on_double_click)
        self.tree.bind("<Return>", self._on_double_click)

        self.detail = tk.Label(
            self,
            text="双击名称插入到光标位置",
            anchor="w",
            justify="left",
            wraplength=self.width - px(self, 28),
        )
        self.detail.pack(fill="x", padx=px(self, 12), pady=(px(self, 2), px(self, 10)))

    def _fill_categories(self) -> None:
        self.category_menu.delete(0, "end")
        self.category_menu.add_command(
            label="全部分类", command=lambda: self.set_category(None)
        )
        for category in category_order():
            self.category_menu.add_command(
                label=category, command=lambda value=category: self.set_category(value)
            )

    # ---------------------------------------------------------------- theming
    def apply_theme(self, theme: Theme) -> None:
        """Recolour the panel and its treeview."""
        self.theme = theme
        self.configure(bg=theme.panel)
        for widget in (self.search_entry, self.category_button):
            widget.configure(background=theme.panel)
        self.title.configure(bg=theme.panel, fg=theme.text, font=ui_font(11, True))
        self.count.configure(bg=theme.panel, fg=theme.muted, font=ui_font(9))
        self.detail.configure(
            bg=theme.panel,
            fg=theme.muted,
            font=ui_font(9),
            wraplength=self.width - px(self, 28),
        )
        for frame in self._frames():
            frame.configure(bg=theme.panel)
        self.search_entry.configure(
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
        self.category_button.configure(
            bg=theme.surface,
            fg=theme.text,
            activebackground=theme.accent_soft,
            activeforeground=theme.text,
            font=ui_font(9),
            relief="flat",
            bd=0,
            highlightthickness=1,
            highlightbackground=theme.border,
            padx=6,
            pady=3,
        )
        self.category_menu.configure(
            bg=theme.surface,
            fg=theme.text,
            activebackground=theme.accent_soft,
            activeforeground=theme.text,
            font=ui_font(9),
            bd=0,
        )
        self._style.configure(
            "Ref.Treeview",
            background=theme.panel,
            fieldbackground=theme.panel,
            foreground=theme.text,
            borderwidth=0,
            relief="flat",
            rowheight=px(self, 23),
            font=ui_font(9),
        )
        self._style.map(
            "Ref.Treeview",
            background=[("selected", theme.accent_soft)],
            foreground=[("selected", theme.text)],
        )
        self._style.layout(
            "Ref.Treeview", [("Treeview.treearea", {"sticky": "nswe"})]
        )
        self.tree.tag_configure(
            "category", font=ui_font(9, True), foreground=theme.accent
        )
        self.tree.tag_configure("api", foreground=theme.text)
        self.scroll.configure(
            bg=theme.panel,
            activebackground=theme.border,
            troughcolor=theme.panel,
            bd=0,
            highlightthickness=0,
            elementborderwidth=0,
        )

    def _frames(self) -> list[tk.Frame]:
        found: list[tk.Frame] = []
        for child in self.winfo_children():
            if isinstance(child, tk.Frame):
                found.append(child)
        return found

    # --------------------------------------------------------------- filtering
    def _on_search(self, _event: tk.Event) -> None:
        self.query = self.search_entry.get().strip()
        self.cancel_pending()
        try:
            self._search_job = self.after(120, self._rebuild_from_typing)
        except tk.TclError:
            self._search_job = None

    def _rebuild_from_typing(self) -> None:
        self._search_job = None
        self.rebuild()

    def cancel_pending(self) -> None:
        """Cancel the debounced tree rebuild before teardown."""
        if self._search_job is not None:
            try:
                self.after_cancel(self._search_job)
            except tk.TclError:
                pass
            self._search_job = None

    def set_category(self, category: str | None) -> None:
        """Restrict the list to one category."""
        self.category = category
        self.category_button.configure(text=category or "全部分类")
        self.cancel_pending()
        self.rebuild()

    @staticmethod
    def _matches(item: Completion, needle: str) -> bool:
        if not needle:
            return True
        folded = needle.casefold()
        return (
            folded in item.name.casefold()
            or folded in item.signature.casefold()
            or folded in item.category.casefold()
        )

    def rebuild(self) -> None:
        """Repopulate the treeview from the current filters."""
        if not self.tree.winfo_exists():
            return
        self.tree.delete(*self.tree.get_children())
        self._by_iid.clear()
        needle = self.query.casefold()
        shown = 0
        for category, items in self._groups:
            if self.category is not None and category != self.category:
                continue
            visible = [item for item in items if self._matches(item, needle)]
            if not visible:
                continue
            parent = self.tree.insert(
                "",
                "end",
                text=f"{category}  ·  {len(visible)}",
                open=bool(needle) or len(visible) <= 8,
                tags=("category",),
            )
            for item in visible:
                iid = self.tree.insert(
                    parent,
                    "end",
                    text=item.name,
                    tags=("api",),
                )
                self._by_iid[iid] = item
                shown += 1
        total = len(load_completions())
        self.count.configure(text=f"{shown}/{total}")
        if needle:
            self.detail.configure(text=f'搜索“{self.query}” · {shown} 个结果')
        else:
            self.detail.configure(text="双击名称插入到光标位置")

    # ------------------------------------------------------------- interaction
    def _on_select(self, _event: tk.Event) -> None:
        selection = self.tree.selection()
        if not selection:
            return
        item = self._by_iid.get(selection[0])
        if item is None:
            return
        self.detail.configure(
            text=f"{item.signature}\n分类：{item.category}"
        )

    def _on_double_click(self, _event: tk.Event) -> None:
        selection = self.tree.selection()
        if not selection:
            return
        item = self._by_iid.get(selection[0])
        if item is not None and self.on_insert is not None:
            self.on_insert(item)

    def _first_item(self) -> str | None:
        for parent in self.tree.get_children():
            children = self.tree.get_children(parent)
            if children:
                return str(children[0])
        return None

    def _insert_first(self, _event: tk.Event) -> str:
        iid = self._first_item()
        if iid is not None:
            self.tree.selection_set(iid)
            self.tree.focus(iid)
            self.tree.see(iid)
            item = self._by_iid.get(iid)
            if item is not None and self.on_insert is not None:
                self.on_insert(item)
        return "break"

    def _focus_tree(self, _event: tk.Event) -> str:
        iid = self._first_item()
        if iid is not None:
            self.tree.selection_set(iid)
            self.tree.focus(iid)
            self.tree.see(iid)
        self.tree.focus_set()
        return "break"

    def focus_search(self) -> None:
        """Focus the filter entry and select its contents."""
        self.search_entry.focus_set()
        self.search_entry.select_range(0, "end")

    def clear_filters(self) -> None:
        """Reset both the search box and the category."""
        self.search_entry.delete(0, "end")
        self.query = ""
        self.set_category(None)
