"""Themed UI helpers: fonts, buttons, glass colours, DPI and popup placement."""

from __future__ import annotations

import ctypes
import functools
import math
import sys
import tkinter as tk
from collections.abc import Callable
from dataclasses import dataclass
from tkinter import font as tkfont

from .animation import Animator, blend_colors
from .themes import Theme

BASE_DPI = 96.0

# --------------------------------------------------------------------------- DPI

_dpi_initialised = False


def enable_dpi_awareness() -> None:
    """Ask Windows for real pixels *before* Tk creates its first window.

    Without this Windows bitmap-scales the whole Tk window on 125%/150%
    displays, which makes every font look blurry. Everything here is best
    effort: older Windows, Wine and non-Windows platforms simply keep Tk's
    own defaults instead of failing to start.
    """
    global _dpi_initialised
    if _dpi_initialised or sys.platform != "win32":
        return
    _dpi_initialised = True
    try:
        user32 = ctypes.windll.user32
        # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (void*) -4 (Win10 1703+)
        try:
            if user32.SetProcessDpiAwarenessContext(ctypes.c_ssize_t(-4)):
                return
        except (AttributeError, OSError, TypeError, ValueError):
            pass
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
            return
        except (AttributeError, OSError, ValueError):
            pass
        try:
            user32.SetProcessDPIAware()
        except (AttributeError, OSError, ValueError):
            pass
    except (AttributeError, OSError, ValueError):
        return


def pixel_density(widget: tk.Misc) -> float:
    """Return the pixels-per-inch reported for ``widget``'s display."""
    try:
        dpi = float(widget.winfo_fpixels("1i"))
    except (tk.TclError, ValueError, TypeError):
        return BASE_DPI
    return dpi if math.isfinite(dpi) and dpi > 0 else BASE_DPI


def ui_scale(widget: tk.Misc) -> float:
    """Return a quarter-step scale factor for hard-coded pixel values.

    Tk already scales point-based fonts and ``c``/``m`` units, but plain pixel
    values (paddings, widths, geometry strings) are not scaled at all. Round to
    0.25 steps so a 96.02 dpi reading does not nudge every widget by a pixel.
    """
    density = pixel_density(widget)
    raw = density / BASE_DPI
    if raw <= 1.0:
        return 1.0
    return min(3.0, max(1.0, round(raw * 4) / 4))


def px(widget: tk.Misc, value: float) -> int:
    """Convert a design-space pixel value into real screen pixels."""
    if not value:
        return 0
    return max(1, int(round(value * ui_scale(widget))))


def configure_scaling(root: tk.Misc) -> float:
    """Align Tk's point-to-pixel conversion with the real display density."""
    density = pixel_density(root)
    scale = ui_scale(root)
    if density > BASE_DPI * 1.05:
        try:
            root.tk.call(
                "tk", "scaling", "-displayof", str(root), round(density / 72.0, 4)
            )
        except tk.TclError:
            pass
    return scale


# ------------------------------------------------------------------ screen area


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class _RECT(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


class _MONITORINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", ctypes.c_ulong),
        ("rcMonitor", _RECT),
        ("rcWork", _RECT),
        ("dwFlags", ctypes.c_ulong),
    ]


def _win_work_area(x: int, y: int) -> tuple[int, int, int, int] | None:
    """Return the work area of the monitor containing ``(x, y)`` on Windows."""
    try:
        user32 = ctypes.windll.user32
        user32.MonitorFromPoint.argtypes = [_POINT, ctypes.c_uint]
        user32.MonitorFromPoint.restype = ctypes.c_void_p
        user32.GetMonitorInfoW.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_MONITORINFO),
        ]
        user32.GetMonitorInfoW.restype = ctypes.c_int
        monitor = user32.MonitorFromPoint(_POINT(int(x), int(y)), 2)
        if not monitor:
            return None
        info = _MONITORINFO()
        info.cbSize = ctypes.sizeof(_MONITORINFO)
        if not user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
            return None
        rect = info.rcWork
        if rect.right <= rect.left or rect.bottom <= rect.top:
            return None
        return (int(rect.left), int(rect.top), int(rect.right), int(rect.bottom))
    except (AttributeError, OSError, TypeError, ValueError):
        return None


def work_area(widget: tk.Misc, x: int, y: int) -> tuple[int, int, int, int]:
    """Return ``(left, top, right, bottom)`` of the usable monitor at ``x, y``.

    Coordinates may be negative on a multi-monitor setup, so a plain
    ``winfo_screenwidth`` clamp is not enough. Windows uses the real monitor
    work area; other platforms fall back to Tk's virtual root.
    """
    if sys.platform == "win32":
        area = _win_work_area(x, y)
        if area is not None:
            return area
    try:
        left = int(widget.winfo_vrootx())
        top = int(widget.winfo_vrooty())
        width = int(widget.winfo_vrootwidth())
        height = int(widget.winfo_vrootheight())
        if width > 0 and height > 0:
            return left, top, left + width, top + height
    except (tk.TclError, ValueError, TypeError):
        pass
    try:
        return 0, 0, int(widget.winfo_screenwidth()), int(widget.winfo_screenheight())
    except (tk.TclError, ValueError, TypeError):
        return 0, 0, 1920, 1080


def clamp_rect(
    x: int,
    y: int,
    width: int,
    height: int,
    area: tuple[int, int, int, int],
    margin: int = 8,
) -> tuple[int, int]:
    """Keep a ``width x height`` rectangle fully inside ``area``."""
    left, top, right, bottom = area
    margin = max(0, min(margin, (right - left) // 4, (bottom - top) // 4))
    max_x = max(left + margin, right - width - margin)
    max_y = max(top + margin, bottom - height - margin)
    return (
        max(left + margin, min(int(x), max_x)),
        max(top + margin, min(int(y), max_y)),
    )


def center_window(
    window: tk.Misc,
    parent: tk.Misc | None = None,
    *,
    width: int | None = None,
    height: int | None = None,
) -> tuple[int, int]:
    """Center ``window`` on ``parent`` (or on the monitor) and clamp it there."""
    window.update_idletasks()
    try:
        w = int(width or window.winfo_reqwidth())
        h = int(height or window.winfo_reqheight())
    except (tk.TclError, ValueError, TypeError):
        return 0, 0
    if parent is not None:
        try:
            cx = parent.winfo_rootx() + parent.winfo_width() // 2
            cy = parent.winfo_rooty() + parent.winfo_height() // 2
        except (tk.TclError, ValueError, TypeError):
            parent = None
    if parent is None:
        try:
            cx = window.winfo_screenwidth() // 2
            cy = window.winfo_screenheight() // 2
        except (tk.TclError, ValueError, TypeError):
            cx, cy = 640, 360
    left, top, right, bottom = work_area(window, cx, cy)
    w = max(1, min(w, right - left))
    h = max(1, min(h, bottom - top))
    x, y = clamp_rect(cx - w // 2, cy - h // 2, w, h, (left, top, right, bottom), 0)
    try:
        window.geometry(f"{w}x{h}+{x}+{y}")
    except tk.TclError:
        pass
    return x, y


# ------------------------------------------------------------------------ fonts


@functools.lru_cache(maxsize=1)
def _families() -> frozenset[str]:
    try:
        return frozenset(name.casefold() for name in tkfont.families())
    except tk.TclError:
        return frozenset()


def ui_font(
    size: int = 10, bold: bool = False
) -> tuple[str, int] | tuple[str, int, str]:
    """Return a platform-friendly UI font tuple.

    Font sizes are points: Tk scales them with the display density, which keeps
    text crisp instead of bitmap-stretched. The font list itself is cached, so
    switching themes does not enumerate the whole system font table again.
    """
    families = _families()
    family = "TkDefaultFont"
    for candidate in ("Microsoft YaHei UI", "Segoe UI", "Noto Sans CJK SC", "Helvetica"):
        if candidate.casefold() in families:
            family = candidate
            break
    return (family, size, "bold") if bold else (family, size)


# ------------------------------------------------------------------- glass look


@dataclass(frozen=True, slots=True)
class GlassPalette:
    """Layered colours that read as frosted glass on both themes."""

    shadow: str
    edge: str
    border: str
    body: str
    list_bg: str
    detail: str
    highlight: str
    select: str


def glass_palette(theme: Theme) -> GlassPalette:
    """Derive a translucent-looking palette from the active theme."""
    dark = theme.key == "dark"
    return GlassPalette(
        shadow=blend_colors(theme.shadow, theme.panel, 0.55 if dark else 0.84),
        edge=blend_colors(theme.shadow, theme.panel, 0.75 if dark else 0.92),
        border=blend_colors(theme.border, theme.accent, 0.22),
        body=blend_colors(theme.surface, theme.accent, 0.10 if dark else 0.035),
        list_bg=blend_colors(theme.surface, theme.accent, 0.06 if dark else 0.02),
        detail=blend_colors(theme.panel, theme.accent, 0.08 if dark else 0.04),
        highlight=blend_colors(theme.accent, theme.surface, 0.72),
        select=blend_colors(theme.accent_soft, theme.accent, 0.12),
    )


# ---------------------------------------------------------------- context menus


def context_sequences() -> tuple[str, ...]:
    """Mouse and keyboard sequences that should open a context menu."""
    sequences = ["<Button-3>", "<Shift-F10>", "<App>"]
    if sys.platform == "darwin":
        # Button-2 is middle-click paste on X11, so only use it on macOS.
        sequences.extend(("<Button-2>", "<Control-Button-1>"))
    return tuple(sequences)


def bind_context(widget: tk.Misc, handler: Callable[[tk.Event], None]) -> None:
    """Bind every platform-appropriate context-menu trigger to ``widget``."""
    for sequence in context_sequences():
        try:
            widget.bind(sequence, handler, add="+")
        except tk.TclError:
            continue


def frameless(window: tk.Misc) -> None:
    """Remove the native title bar while keeping a Windows taskbar entry.

    ``overrideredirect`` hides the whole OS frame (including the blue caption
    bar), but on Windows it also marks the window as a tool window, which drops
    it from the taskbar. Restoring ``WS_EX_APPWINDOW`` keeps Alt-Tab working.
    """
    try:
        window.overrideredirect(True)
    except tk.TclError:
        return
    if sys.platform == "win32":
        try:
            # ``after_idle`` runs before Tk has created the native wrapper, so
            # GetParent() returns 0 and the style never changes. Wait until the
            # window is mapped; the helper retries a few times if needed.
            window.after(200, lambda: _windows_taskbar_entry(window, 0))
        except (tk.TclError, RuntimeError):
            return


def _windows_taskbar_entry(window: tk.Misc, attempt: int = 0) -> None:
    """Restore WS_EX_APPWINDOW and force the shell to list the window."""
    try:
        if not window.winfo_exists():
            return
        user32 = ctypes.windll.user32
        hwnd = user32.GetParent(window.winfo_id())
        if not hwnd:
            if attempt < 20:
                try:
                    window.after(
                        100, lambda: _windows_taskbar_entry(window, attempt + 1)
                    )
                except (tk.TclError, RuntimeError):
                    return
            return
        gwl_exstyle = -20
        ws_ex_toolwindow = 0x00000080
        ws_ex_appwindow = 0x00040000
        user32.GetWindowLongW.argtypes = [ctypes.c_void_p, ctypes.c_int]
        user32.GetWindowLongW.restype = ctypes.c_long
        user32.SetWindowLongW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long]
        style = int(user32.GetWindowLongW(hwnd, gwl_exstyle))
        ws_ex_layered = 0x00080000
        # WS_EX_LAYERED makes Windows draw a soft drop shadow around the
        # borderless window, which is the only way to separate it from the
        # desktop since overrideredirect() removes the native frame.
        desired = (style | ws_ex_appwindow | ws_ex_layered) & ~ws_ex_toolwindow
        geometry = window.geometry()
        if desired != style:
            user32.SetWindowLongW(hwnd, gwl_exstyle, desired)
            # Keep the window fully opaque (alpha 255); without this a layered
            # window defaults to invisible. LWA_ALPHA == 2.
            user32.SetLayeredWindowAttributes.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint,
                ctypes.c_byte,
                ctypes.c_uint,
            ]
            user32.SetLayeredWindowAttributes(hwnd, 0, 255, 2)
            # Hiding and re-showing makes the shell re-evaluate the taskbar
            # entry; SW_SHOWNOACTIVATE avoids stealing the user's focus.
            user32.ShowWindow(hwnd, 0)
            user32.ShowWindow(hwnd, 4)
        user32.SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x27)
        try:
            window.geometry(geometry)
        except tk.TclError:
            return
    except (AttributeError, OSError, TypeError, ValueError, tk.TclError):
        return


def popup_menu(menu: tk.Menu, widget: tk.Misc, event: tk.Event | None = None) -> None:
    """Post ``menu`` at the pointer, or near ``widget`` for keyboard triggers.

    Tk fills ``x_root``/``y_root`` with (0, 0) for keyboard triggers such as
    ``<App>`` and ``<Shift-F10>``; posting there looks like "nothing happened".
    """
    event_type = str(getattr(event, "type", "") or "")
    keyboard = "KeyPress" in event_type or event_type == "2"
    x = getattr(event, "x_root", None) if event is not None and not keyboard else None
    y = getattr(event, "y_root", None) if event is not None and not keyboard else None
    if (
        not isinstance(x, int)
        or not isinstance(y, int)
        or (x <= 0 and y <= 0)
    ):
        try:
            x = widget.winfo_rootx() + min(widget.winfo_width() // 3, px(widget, 240))
            y = widget.winfo_rooty() + min(widget.winfo_height() // 3, px(widget, 80))
        except (tk.TclError, ValueError, TypeError):
            return
    left, top, right, bottom = work_area(widget, x, y)
    gap = px(widget, 8)
    x = max(left, min(int(x), max(left, right - gap)))
    y = max(top, min(int(y), max(top, bottom - gap)))
    try:
        menu.tk_popup(x, y)
    except tk.TclError:
        return
    finally:
        try:
            menu.grab_release()
        except tk.TclError:
            pass


# --------------------------------------------------------------------- controls


class HoverButton(tk.Button):
    """A flat button that animates its background on hover."""

    def __init__(
        self,
        master: tk.Misc,
        text: str = "",
        command: Callable[[], None] | None = None,
        kind: str = "ghost",
        **kwargs: object,
    ) -> None:
        super().__init__(
            master,
            text=text,
            command=command,
            bd=0,
            relief="flat",
            cursor="hand2",
            takefocus=0,
            **kwargs,
        )
        self.configure(padx=px(self, 10), pady=px(self, 4))
        self.kind = kind
        self.theme: Theme | None = None
        self._animator = Animator(self)
        self.bind("<Enter>", self._on_enter)
        self.bind("<Leave>", self._on_leave)

    def destroy(self) -> None:
        """Drop pending hover frames before the widget goes away."""
        self._animator.cancel_all()
        super().destroy()

    def _base(self) -> str:
        if self.theme is None:
            return "#ffffff"
        return self.theme.accent if self.kind == "primary" else self.theme.surface

    def apply_theme(self, theme: Theme) -> None:
        """Recolour the button for ``theme``."""
        self._animator.cancel_all()
        self.theme = theme
        primary = self.kind == "primary"
        self.configure(
            font=ui_font(9),
            bg=self._base(),
            fg="#ffffff" if primary else theme.text,
            activebackground=(
                blend_colors(theme.accent, "#ffffff", 0.18)
                if primary
                else theme.accent_soft
            ),
            activeforeground="#ffffff" if primary else theme.text,
            disabledforeground=theme.muted,
            highlightthickness=0 if primary else px(self, 1),
            highlightbackground=(
                theme.surface if primary else blend_colors(theme.border, theme.surface, 0.35)
            ),
            highlightcolor=theme.accent,
        )

    def _on_enter(self, _event: tk.Event) -> None:
        if self.theme is None or not self.winfo_exists():
            return
        target = (
            blend_colors(self.theme.accent, "#ffffff", 0.15)
            if self.kind == "primary"
            else self.theme.accent_soft
        )
        start = self._base()
        self._animator.animate(
            "hover",
            90,
            lambda t: self.configure(bg=blend_colors(start, target, t)),
            max_frames=8,
        )

    def _on_leave(self, _event: tk.Event) -> None:
        if self.theme is None or not self.winfo_exists():
            return
        target = self._base()
        start = str(self.cget("background"))
        self._animator.animate(
            "hover",
            90,
            lambda t: self.configure(bg=blend_colors(start, target, t)),
            max_frames=8,
        )


class Separator(tk.Frame):
    """A one-pixel themed divider."""

    def __init__(self, master: tk.Misc, orient: str = "horizontal") -> None:
        width = 1 if orient == "vertical" else 0
        height = 0 if orient == "vertical" else 1
        super().__init__(master, width=width, height=height, bd=0)
        self.orient = orient
        self.configure(
            width=px(self, width) if width else 0,
            height=px(self, height) if height else 0,
        )

    def apply_theme(self, theme: Theme) -> None:
        """Recolour the divider."""
        self.configure(bg=theme.border)
