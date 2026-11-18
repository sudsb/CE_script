"""Small, cancellation-safe Tk animation helpers."""

from __future__ import annotations

import time
from collections.abc import Callable
from tkinter import Misc, TclError

Easing = Callable[[float], float]


def clamp(value: float, low: float = 0.0, high: float = 1.0) -> float:
    """Clamp ``value`` into the inclusive range ``low``..``high``."""
    return max(low, min(high, value))


def ease_out_cubic(t: float) -> float:
    """Fast start, gentle finish."""
    t = clamp(t)
    return 1.0 - (1.0 - t) ** 3


def ease_in_out_cubic(t: float) -> float:
    """Symmetric acceleration and deceleration."""
    t = clamp(t)
    if t < 0.5:
        return 4.0 * t**3
    return 1.0 - (-2.0 * t + 2.0) ** 3 / 2.0


def lerp(start: float, end: float, t: float) -> float:
    """Linearly interpolate between two numbers."""
    return start + (end - start) * clamp(t)


def _hex_to_rgb(value: str) -> tuple[int, int, int] | None:
    text = value.strip().lstrip("#")
    if len(text) != 6:
        return None
    try:
        return int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16)
    except ValueError:
        return None


def blend_colors(start: str, end: str, t: float) -> str:
    """Blend two ``#rrggbb`` colours; invalid input falls back to ``end``."""
    first = _hex_to_rgb(start)
    second = _hex_to_rgb(end)
    if first is None or second is None:
        return end
    ratio = clamp(t)
    red = int(first[0] + (second[0] - first[0]) * ratio)
    green = int(first[1] + (second[1] - first[1]) * ratio)
    blue = int(first[2] + (second[2] - first[2]) * ratio)
    return f"#{red:02x}{green:02x}{blue:02x}"


class Animator:
    """Run short frame-based tweens on a Tk widget.

    Every animation is keyed. Starting a key again cancels the previous run,
    which keeps hover, popup, and panel animations from piling up.
    """

    def __init__(self, widget: Misc) -> None:
        self.widget = widget
        self._jobs: dict[str, str] = {}

    def is_running(self, key: str) -> bool:
        """Return whether an animation with ``key`` is active."""
        return key in self._jobs

    def cancel(self, key: str) -> None:
        """Cancel one animation if it is active."""
        job = self._jobs.pop(key, None)
        if job is None:
            return
        try:
            self.widget.after_cancel(job)
        except TclError:
            return

    def cancel_all(self) -> None:
        """Cancel every animation owned by this animator."""
        for key in list(self._jobs):
            self.cancel(key)

    def animate(
        self,
        key: str,
        duration_ms: int,
        on_frame: Callable[[float], None],
        *,
        easing: Easing = ease_out_cubic,
        on_done: Callable[[], None] | None = None,
        max_frames: int = 16,
    ) -> None:
        """Animate ``on_frame(progress)`` over ``duration_ms`` milliseconds."""
        self.cancel(key)
        if duration_ms <= 0:
            try:
                on_frame(1.0)
                if on_done is not None:
                    on_done()
            except TclError:
                pass
            return
        started = time.perf_counter()
        frames = max(4, min(max_frames, duration_ms // 16))
        interval = max(8, duration_ms // frames)

        def step() -> None:
            if key not in self._jobs:
                return
            elapsed = (time.perf_counter() - started) * 1000.0
            progress = clamp(elapsed / duration_ms)
            try:
                on_frame(easing(progress))
            except TclError:
                self._jobs.pop(key, None)
                return
            if progress >= 1.0:
                self._jobs.pop(key, None)
                if on_done is not None:
                    try:
                        on_done()
                    except TclError:
                        pass
                return
            try:
                self._jobs[key] = self.widget.after(interval, step)
            except TclError:
                self._jobs.pop(key, None)

        try:
            self._jobs[key] = self.widget.after(0, step)
        except TclError:
            return
