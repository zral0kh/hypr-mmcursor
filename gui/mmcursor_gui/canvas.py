"""The mm-space canvas: draws the layout `hyprctl -j mmcursor` reports, and
turns drags into `hyprctl mmcursor place` calls. It computes screen<->mm
scaling for itself (that's drawing, not layout) but never decides where a
monitor "should" go — every rectangle it paints is either the server's last
reported mm rect or a provisional drag position that gets sent to the plugin
and then re-confirmed from the next poll.
"""

from __future__ import annotations

import re
import time

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk, Gtk  # noqa: E402

from .hyprctl import MmcursorState, MonitorState  # noqa: E402

# A fixed, deliberately theme-independent palette — this canvas reads like a
# design tool's artboard (Figma/Illustrator convention) rather than trying to
# track GTK/Adwaita accent colors through cairo, which would fight the system
# theme instead of complementing it.
_BG = (0.114, 0.122, 0.133)
_GRID = (1.0, 1.0, 1.0, 0.05)
_GRID_AXIS = (1.0, 1.0, 1.0, 0.12)
_TEXT = (0.92, 0.93, 0.95)
_TEXT_DIM = (0.65, 0.67, 0.72)
_CURSOR = (0.35, 0.85, 0.95)
_GUIDE = (0.98, 0.72, 0.25)
_WARN_BORDER = (0.95, 0.35, 0.35)

_PALETTE = [
    (0.36, 0.55, 0.95),   # blue
    (0.93, 0.55, 0.30),   # orange
    (0.45, 0.80, 0.55),   # green
    (0.80, 0.45, 0.85),   # violet
    (0.95, 0.75, 0.30),   # amber
    (0.40, 0.80, 0.85),   # teal
]

_SNAP_PX = 10.0          # snap catch radius, screen px
_MIN_SCALE = 0.02
_MAX_SCALE = 4.0
_PREVIEW_INTERVAL = 1.0 / 25.0  # throttle for the live hyprctl place() calls

_ARROW_OFFSET = 24.0     # screen px from the edge to the arrow's centre
_ARROW_HIT_R = 13.0      # click radius, screen px
_TOUCH_EPS_MM = 0.5      # tolerance for "this edge is touching a neighbour"

_ABS_EXPR = re.compile(r"^\s*([+-]?\d+(?:\.\d+)?)\s*$")
_ADD_SUB_EXPR = re.compile(r"^\s*([+-]?\d+(?:\.\d+)?)\s*([+-])\s*(\d+(?:\.\d+)?)\s*$")


def _parse_offset_expr(text: str) -> float | None:
    """Accepts either a bare number (an absolute value, replacing whatever was
    prefilled) or `<base><+|-><delta>` (add/subtract from a base — normally
    the prefilled existing value, but the user is free to edit that part too).
    Anything else is a typo, not a value."""
    m = _ABS_EXPR.match(text)
    if m:
        return float(m.group(1))
    m = _ADD_SUB_EXPR.match(text)
    if m:
        base, op, delta = float(m.group(1)), m.group(2), float(m.group(3))
        return base + delta if op == "+" else base - delta
    return None


def _color_for(index: int) -> tuple[float, float, float]:
    return _PALETTE[index % len(_PALETTE)]


def _rounded_rect(cr, x: float, y: float, w: float, h: float, r: float) -> None:
    r = min(r, w / 2, h / 2)
    cr.new_sub_path()
    cr.arc(x + w - r, y + r, r, -3.14159 / 2, 0)
    cr.arc(x + w - r, y + h - r, r, 0, 3.14159 / 2)
    cr.arc(x + r, y + h - r, r, 3.14159 / 2, 3.14159)
    cr.arc(x + r, y + r, r, 3.14159, 3.0 * 3.14159 / 2)
    cr.close_path()


class MonitorCanvas(Gtk.DrawingArea):
    """Signals (via plain callables, not GObject signals — this stays a small,
    directly-wired widget rather than growing its own signal API):

      on_select(name: str | None)
      on_preview(name: str, x_mm: float, y_mm: float)   -- throttled, mid-drag
      on_committed(name: str, x_mm: float, y_mm: float) -- once, on release
    """

    def __init__(self) -> None:
        super().__init__()
        self.set_can_focus(True)
        self.set_hexpand(True)
        self.set_vexpand(True)

        self.on_select = None
        self.on_preview = None
        self.on_committed = None

        self._state: MmcursorState | None = None
        self._overlay: dict[str, tuple[float, float]] = {}
        self._selected: str | None = None
        self._dragging: str | None = None
        self._panning = False
        self._guides: list[tuple[str, float]] = []  # ("v"|"h", mm coordinate)
        self._last_preview_t = 0.0

        self._scale = 1.0            # screen px per mm
        self._origin_mm = (0.0, 0.0)  # world mm point at the canvas's top-left
        self._auto_fit = True
        self._last_pointer = (0.0, 0.0)
        self._pan_start = (0.0, 0.0)
        self._drag_start_mm = (0.0, 0.0)

        self.set_draw_func(self._draw)
        self.connect("resize", lambda *_a: self._maybe_fit())

        drag = Gtk.GestureDrag.new()
        drag.connect("drag-begin", self._on_drag_begin)
        drag.connect("drag-update", self._on_drag_update)
        drag.connect("drag-end", self._on_drag_end)
        self.add_controller(drag)

        scroll = Gtk.EventControllerScroll.new(Gtk.EventControllerScrollFlags.BOTH_AXES)
        scroll.connect("scroll", self._on_scroll)
        self.add_controller(scroll)

        motion = Gtk.EventControllerMotion.new()
        motion.connect("motion", lambda _c, x, y: setattr(self, "_last_pointer", (x, y)))
        self.add_controller(motion)

        click = Gtk.GestureClick.new()
        click.connect("pressed", self._on_click_pressed)
        self.add_controller(click)

    # -- state -------------------------------------------------------------

    def set_state(self, state: MmcursorState) -> None:
        self._state = state
        # A monitor mid-drag keeps its overlay position for smoothness; a
        # monitor NOT being dragged should always reflect the server, so a
        # save/reload or an external hotplug is visible immediately.
        self._overlay = {k: v for k, v in self._overlay.items() if k == self._dragging}
        self._maybe_fit()
        self.queue_draw()

    def selected(self) -> str | None:
        return self._selected

    def select(self, name: str | None) -> None:
        self._selected = name
        self.queue_draw()

    def fit_to_view(self) -> None:
        self._auto_fit = True
        self._recompute_fit()
        self.queue_draw()

    # -- geometry ------------------------------------------------------------

    def _rect_for(self, m: MonitorState) -> tuple[float, float, float, float]:
        x, y = self._overlay.get(m.name, (m.mm_x, m.mm_y))
        return x, y, m.mm_w, m.mm_h

    def _bbox(self) -> tuple[float, float, float, float]:
        if not self._state or not self._state.monitors:
            return 0.0, 0.0, 400.0, 300.0
        xs0, ys0, xs1, ys1 = [], [], [], []
        for m in self._state.monitors:
            x, y, w, h = self._rect_for(m)
            xs0.append(x); ys0.append(y); xs1.append(x + w); ys1.append(y + h)
        return min(xs0), min(ys0), max(xs1) - min(xs0), max(ys1) - min(ys0)

    def _maybe_fit(self) -> None:
        if self._auto_fit:
            self._recompute_fit()

    def _recompute_fit(self) -> None:
        w = self.get_width() or 1
        h = self.get_height() or 1
        bx, by, bw, bh = self._bbox()
        pad = 48.0
        avail_w, avail_h = max(w - 2 * pad, 1.0), max(h - 2 * pad, 1.0)
        scale = min(avail_w / max(bw, 1.0), avail_h / max(bh, 1.0))
        scale = max(_MIN_SCALE, min(_MAX_SCALE, scale))
        self._scale = scale
        cx, cy = bx + bw / 2.0, by + bh / 2.0
        self._origin_mm = (cx - (w / 2.0) / scale, cy - (h / 2.0) / scale)

    def _world_to_screen(self, wx: float, wy: float) -> tuple[float, float]:
        ox, oy = self._origin_mm
        return (wx - ox) * self._scale, (wy - oy) * self._scale

    def _screen_to_world(self, sx: float, sy: float) -> tuple[float, float]:
        ox, oy = self._origin_mm
        return ox + sx / self._scale, oy + sy / self._scale

    def _hit_test(self, sx: float, sy: float) -> str | None:
        if not self._state:
            return None
        for m in reversed(self._state.monitors):
            x, y, w, h = self._rect_for(m)
            x0, y0 = self._world_to_screen(x, y)
            x1, y1 = self._world_to_screen(x + w, y + h)
            if x0 <= sx <= x1 and y0 <= sy <= y1:
                return m.name
        return None

    # -- snapping --------------------------------------------------------

    def _snap(self, name: str, x: float, y: float, w: float, h: float) -> tuple[float, float, list[tuple[str, float]]]:
        if not self._state:
            return x, y, []
        threshold = _SNAP_PX / self._scale
        edges_x: list[float] = []
        edges_y: list[float] = []
        for m in self._state.monitors:
            if m.name == name:
                continue
            ox, oy, ow, oh = self._rect_for(m)
            edges_x += [ox, ox + ow, ox + ow / 2.0]
            edges_y += [oy, oy + oh, oy + oh / 2.0]

        guides: list[tuple[str, float]] = []

        def closest(candidates: list[float], my_values: list[float]) -> tuple[float | None, float | None]:
            best_delta, best_target = None, None
            for target in candidates:
                for mine in my_values:
                    d = target - mine
                    if abs(d) <= threshold and (best_delta is None or abs(d) < abs(best_delta)):
                        best_delta, best_target = d, target
            return best_delta, best_target

        dx, target_x = closest(edges_x, [x, x + w / 2.0, x + w])
        dy, target_y = closest(edges_y, [y, y + h / 2.0, y + h])
        if dx is not None:
            x += dx
            guides.append(("v", target_x))
        if dy is not None:
            y += dy
            guides.append(("h", target_y))
        return x, y, guides

    # -- outward nudge arrows -------------------------------------------

    def _free_axes(self, name: str) -> tuple[bool, bool, bool, bool]:
        """(free_left, free_right, free_top, free_bottom) for one monitor.

        An axis (x or y) counts as bounded — and loses BOTH of its arrows,
        not just the touching one — the moment either side of it touches a
        neighbour. Nudging along a bounded axis at all would either open a
        gap on the touching side or drive straight through the neighbour;
        since gaps between monitors are never allowed, the whole axis is off
        limits, not just the edge that happens to be in contact.
        """
        if not self._state:
            return True, True, True, True
        m = self._state.monitor(name)
        if not m:
            return True, True, True, True
        if len(self._state.monitors) < 2:
            # Nothing to be free RELATIVE TO — a single monitor has no anchor
            # or neighbour, so an absolute nudge here wouldn't mean anything.
            return False, False, False, False
        x, y, w, h = self._rect_for(m)

        x_bounded = False
        y_bounded = False
        for o in self._state.monitors:
            if o.name == name:
                continue
            ox, oy, ow, oh = self._rect_for(o)
            gap = self._state.gap_between(name, o.name)

            if min(y + h, oy + oh) - max(y, oy) > _TOUCH_EPS_MM:
                if abs(ox - (x + w) - gap) < _TOUCH_EPS_MM or abs(x - (ox + ow) - gap) < _TOUCH_EPS_MM:
                    x_bounded = True
            if min(x + w, ox + ow) - max(x, ox) > _TOUCH_EPS_MM:
                if abs(oy - (y + h) - gap) < _TOUCH_EPS_MM or abs(y - (oy + oh) - gap) < _TOUCH_EPS_MM:
                    y_bounded = True

        return not x_bounded, not x_bounded, not y_bounded, not y_bounded

    def _arrow_points(self, name: str) -> list[tuple[str, float, float]]:
        """[(direction, screen_x, screen_y), ...] for whichever of this
        monitor's edges are free to move. The single geometry both drawing
        and click hit-testing use, so they can never disagree."""
        if not self._state:
            return []
        m = self._state.monitor(name)
        if not m:
            return []
        x, y, w, h = self._rect_for(m)
        free_l, free_r, free_t, free_b = self._free_axes(name)
        x0, y0 = self._world_to_screen(x, y)
        x1, y1 = self._world_to_screen(x + w, y + h)
        midx, midy = (x0 + x1) / 2.0, (y0 + y1) / 2.0

        pts: list[tuple[str, float, float]] = []
        if free_t:
            pts.append(("up", midx, y0 - _ARROW_OFFSET))
        if free_b:
            pts.append(("down", midx, y1 + _ARROW_OFFSET))
        if free_l:
            pts.append(("left", x0 - _ARROW_OFFSET, midy))
        if free_r:
            pts.append(("right", x1 + _ARROW_OFFSET, midy))
        return pts

    def _arrow_hit_test(self, sx: float, sy: float) -> tuple[str, str] | None:
        if not self._state:
            return None
        for m in self._state.monitors:
            for direction, ax, ay in self._arrow_points(m.name):
                if (sx - ax) ** 2 + (sy - ay) ** 2 <= _ARROW_HIT_R ** 2:
                    return m.name, direction
        return None

    def _open_arrow_editor(self, name: str, direction: str) -> None:
        m = self._state.monitor(name) if self._state else None
        if not m:
            return
        self.select(name)
        if callable(self.on_select):
            self.on_select(name)

        x, y = self._rect_for(m)[:2]
        axis, op, base = {
            "up":    ("y", "-", y),
            "down":  ("y", "+", y),
            "left":  ("x", "-", x),
            "right": ("x", "+", x),
        }[direction]

        found = next((p for p in self._arrow_points(name) if p[0] == direction), None)
        if found is None:
            return
        _, ax, ay = found

        popover = Gtk.Popover()
        popover.set_parent(self)
        popover.set_autohide(True)
        popover.connect("closed", lambda p: p.unparent())

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6,
                       margin_top=8, margin_bottom=8, margin_start=8, margin_end=8)
        box.append(Gtk.Label(label=f"{name} · {axis} (mm)", xalign=0.0, css_classes=["caption", "dim-label"]))
        entry = Gtk.Entry()
        entry.set_width_chars(16)
        entry.set_text(f"{base:.2f} {op} ")
        entry.connect("changed", lambda e: e.remove_css_class("error"))
        box.append(entry)
        popover.set_child(box)
        popover.set_pointing_to(Gdk.Rectangle(int(ax) - 1, int(ay) - 1, 2, 2))

        def commit(*_a) -> None:
            result = _parse_offset_expr(entry.get_text())
            if result is None:
                entry.add_css_class("error")
                return
            cur = self._state.monitor(name) if self._state else None
            cx, cy = self._rect_for(cur)[:2] if cur else (x, y)
            new_x, new_y = (result, cy) if axis == "x" else (cx, result)
            popover.popdown()
            if callable(self.on_committed):
                self.on_committed(name, new_x, new_y)

        entry.connect("activate", commit)
        popover.popup()
        entry.grab_focus()
        entry.set_position(-1)

    def _draw_arrow_glyph(self, cr, direction: str, ax: float, ay: float) -> None:
        cr.save()
        cr.arc(ax, ay, _ARROW_HIT_R, 0, 2 * 3.14159)
        cr.set_source_rgba(1.0, 1.0, 1.0, 0.14)
        cr.fill()

        size = 6.0
        cr.set_source_rgba(1.0, 1.0, 1.0, 0.9)
        cr.set_line_width(2.0)
        if direction == "up":
            cr.move_to(ax - size, ay + size * 0.6); cr.line_to(ax, ay - size); cr.line_to(ax + size, ay + size * 0.6)
        elif direction == "down":
            cr.move_to(ax - size, ay - size * 0.6); cr.line_to(ax, ay + size); cr.line_to(ax + size, ay - size * 0.6)
        elif direction == "left":
            cr.move_to(ax + size * 0.6, ay - size); cr.line_to(ax - size, ay); cr.line_to(ax + size * 0.6, ay + size)
        else:  # right
            cr.move_to(ax - size * 0.6, ay - size); cr.line_to(ax + size, ay); cr.line_to(ax - size * 0.6, ay + size)
        cr.stroke()
        cr.restore()

    def _draw_arrows(self, cr) -> None:
        if not self._state:
            return
        for m in self._state.monitors:
            if m.name == self._dragging:
                continue  # avoid arrow-vs-drag ambiguity while it's moving
            for direction, ax, ay in self._arrow_points(m.name):
                self._draw_arrow_glyph(cr, direction, ax, ay)

    # -- input -------------------------------------------------------------

    def _on_click_pressed(self, _g, _n_press: int, x: float, y: float) -> None:
        hit = self._arrow_hit_test(x, y)
        if hit is not None:
            self._open_arrow_editor(*hit)

    def _on_drag_begin(self, _g, x: float, y: float) -> None:
        if self._arrow_hit_test(x, y) is not None:
            # An arrow click, not a monitor drag; the click gesture handles
            # it. Leave both drag/pan state unset so drag-update is a no-op.
            self._dragging = None
            self._panning = False
            return
        name = self._hit_test(x, y)
        self._panning = name is None
        self._dragging = name
        self.grab_focus()
        if name is None:
            self._pan_start = self._origin_mm
            return
        self.select(name)
        if callable(self.on_select):
            self.on_select(name)
        m = self._state.monitor(name) if self._state else None
        self._drag_start_mm = self._rect_for(m)[:2] if m else (0.0, 0.0)

        # Pin every OTHER monitor at its current position before this one
        # moves. Without this, a monitor with no placement of its own that's
        # anchored to whichever one we're about to drag — the common case in
        # a simple two-monitor desk, where the satellite is "right-of" the
        # one you just grabbed — gets re-derived relative to wherever this
        # drag ends up, and visibly drags along with it. That's the plugin's
        # relation model working as designed, just not what a direct-
        # manipulation UI should look like. A pin is a live override like any
        # drag preview: never written to disk unless that monitor is itself
        # dragged, so an untouched monitor keeps its real relational
        # placement in the saved config.
        if self._state and callable(self.on_preview):
            for other in self._state.monitors:
                if other.name != name:
                    ox, oy = self._rect_for(other)[:2]
                    self.on_preview(other.name, ox, oy)

    def _on_drag_update(self, _g, dx: float, dy: float) -> None:
        if self._panning:
            ox, oy = self._pan_start
            self._origin_mm = (ox - dx / self._scale, oy - dy / self._scale)
            self._auto_fit = False
            self.queue_draw()
            return
        if not self._dragging or not self._state:
            return
        m = self._state.monitor(self._dragging)
        if not m:
            return
        wx = self._drag_start_mm[0] + dx / self._scale
        wy = self._drag_start_mm[1] + dy / self._scale
        wx, wy, guides = self._snap(self._dragging, wx, wy, m.mm_w, m.mm_h)
        self._overlay[self._dragging] = (wx, wy)
        self._guides = guides
        self.queue_draw()

        now = time.monotonic()
        if now - self._last_preview_t >= _PREVIEW_INTERVAL and callable(self.on_preview):
            self._last_preview_t = now
            self.on_preview(self._dragging, wx, wy)

    def _on_drag_end(self, _g, dx: float, dy: float) -> None:
        if self._panning:
            self._panning = False
            return
        name = self._dragging
        self._dragging = None
        self._guides = []
        if name and name in self._overlay and self._state:
            x, y = self._overlay[name]
            m = self._state.monitor(name)
            if m:
                x, y = self._resolve_overlaps(name, x, y, m.mm_w, m.mm_h)
                self._overlay[name] = (x, y)
            if callable(self.on_committed):
                self.on_committed(name, x, y)
        self.queue_draw()

    def _resolve_overlaps(self, name: str, x: float, y: float, w: float, h: float) -> tuple[float, float]:
        """Called once, on release: guarantee the position about to be
        committed doesn't overlap anything, before it ever reaches the
        plugin. `_snap()` during the drag only catches near-misses within a
        screen-pixel threshold; a monitor dropped well inside another one
        needs an actual push, or the plugin hard-refuses the whole layout
        (mmcursor and Hyprland stay up, but cursor motion goes stock —
        "it breaks" from the outside). Each pass pushes out of one
        overlapping neighbour along whichever axis needs the smaller nudge,
        landing flush against that edge — which is itself a clean snap, not
        just a rescue. Repeated for chains: pushing clear of one monitor can
        land on top of another.
        """
        if not self._state:
            return x, y
        for _ in range(len(self._state.monitors)):
            moved = False
            for o in self._state.monitors:
                if o.name == name:
                    continue
                ox, oy, ow, oh = self._rect_for(o)
                if x < ox + ow and ox < x + w and y < oy + oh and oy < y + h:
                    overlap_x = min(x + w, ox + ow) - max(x, ox)
                    overlap_y = min(y + h, oy + oh) - max(y, oy)
                    if overlap_x < overlap_y:
                        x = ox - w if x < ox else ox + ow
                    else:
                        y = oy - h if y < oy else oy + oh
                    moved = True
            if not moved:
                break
        return x, y

    def _on_scroll(self, _c, _dx: float, dy: float) -> bool:
        factor = 1.1 if dy < 0 else (1 / 1.1 if dy > 0 else 1.0)
        if factor == 1.0:
            return True
        px, py = self._last_pointer
        before = self._screen_to_world(px, py)
        self._scale = max(_MIN_SCALE, min(_MAX_SCALE, self._scale * factor))
        after = self._screen_to_world(px, py)
        ox, oy = self._origin_mm
        self._origin_mm = (ox + (before[0] - after[0]), oy + (before[1] - after[1]))
        self._auto_fit = False
        self.queue_draw()
        return True

    # -- drawing -------------------------------------------------------------

    def _draw_grid(self, cr, width: float, height: float) -> None:
        step = 10.0
        while step * self._scale < 45.0:
            step *= 5.0 if step * self._scale < 15 else 2.0
        ox, oy = self._origin_mm
        x0 = (ox // step) * step
        y0 = (oy // step) * step

        x = x0
        while True:
            sx, _ = self._world_to_screen(x, 0)
            if sx > width:
                break
            if sx >= 0:
                axis = abs(x) < 1e-6
                cr.set_source_rgba(*(_GRID_AXIS if axis else _GRID))
                cr.set_line_width(1.5 if axis else 1.0)
                cr.move_to(sx, 0); cr.line_to(sx, height); cr.stroke()
            x += step

        y = y0
        while True:
            _, sy = self._world_to_screen(0, y)
            if sy > height:
                break
            if sy >= 0:
                axis = abs(y) < 1e-6
                cr.set_source_rgba(*(_GRID_AXIS if axis else _GRID))
                cr.set_line_width(1.5 if axis else 1.0)
                cr.move_to(0, sy); cr.line_to(width, sy); cr.stroke()
            y += step

    def _draw_monitor(self, cr, index: int, m: MonitorState) -> None:
        x, y, w, h = self._rect_for(m)
        sx0, sy0 = self._world_to_screen(x, y)
        sx1, sy1 = self._world_to_screen(x + w, y + h)
        sw, sh = sx1 - sx0, sy1 - sy0

        r, g, b = _color_for(index)
        selected = m.name == self._selected

        cr.save()
        _rounded_rect(cr, sx0, sy0, sw, sh, 10.0)
        cr.set_source_rgba(r, g, b, 0.28 if not selected else 0.4)
        cr.fill_preserve()
        cr.set_source_rgba(r, g, b, 0.95)
        cr.set_line_width(2.5 if selected else 1.5)
        cr.stroke()
        cr.restore()

        cr.save()
        _rounded_rect(cr, sx0, sy0, sw, sh, 10.0)
        cr.clip()
        cr.set_source_rgb(*_TEXT)
        cr.select_font_face("sans-serif")
        cr.set_font_size(15)
        cr.move_to(sx0 + 14, sy0 + 26)
        cr.show_text(m.display_name)

        cr.set_source_rgb(*_TEXT_DIM)
        cr.set_font_size(12)
        line = sy0 + 45
        if m.description:
            # display_name is already the description; show the Hyprland
            # port name too, since that's what mmcursor-place/-gap/-offset
            # in hyprland.conf actually key on.
            cr.move_to(sx0 + 14, line)
            cr.show_text(m.name)
            line += 16
        cr.move_to(sx0 + 14, line)
        cr.show_text(f"{w:.0f} x {h:.0f} mm")
        line += 16
        cr.move_to(sx0 + 14, line)
        cr.show_text(f"{m.px_per_mm_x:.2f} px/mm")
        if m.placed_how:
            label = m.placed_how if not m.placed_anchor else f"{m.placed_how} {m.placed_anchor}"
            line += 16
            cr.move_to(sx0 + 14, line)
            cr.show_text(label)
        cr.restore()

    def _draw_guides(self, cr, width: float, height: float) -> None:
        cr.set_source_rgba(*_GUIDE)
        cr.set_line_width(1.0)
        cr.set_dash([4.0, 4.0])
        for kind, coord in self._guides:
            if kind == "v":
                sx, _ = self._world_to_screen(coord, 0)
                cr.move_to(sx, 0); cr.line_to(sx, height)
            else:
                _, sy = self._world_to_screen(0, coord)
                cr.move_to(0, sy); cr.line_to(width, sy)
            cr.stroke()
        cr.set_dash([])

    def _draw_cursor(self, cr) -> None:
        if not self._state or not self._state.cursor_mm:
            return
        sx, sy = self._world_to_screen(*self._state.cursor_mm)
        cr.save()
        cr.set_source_rgba(*_CURSOR, 0.25)
        cr.arc(sx, sy, 10.0, 0, 2 * 3.14159)
        cr.fill()
        cr.set_source_rgba(*_CURSOR, 0.95)
        cr.arc(sx, sy, 3.5, 0, 2 * 3.14159)
        cr.fill()
        cr.restore()

    def _draw(self, _area, cr, width: float, height: float) -> None:
        cr.set_source_rgb(*_BG)
        cr.paint()
        self._draw_grid(cr, width, height)

        if self._state:
            for i, m in enumerate(self._state.monitors):
                self._draw_monitor(cr, i, m)
            self._draw_arrows(cr)
            self._draw_guides(cr, width, height)
            self._draw_cursor(cr)

        if not self._state or not self._state.monitors:
            cr.set_source_rgb(*_TEXT_DIM)
            cr.select_font_face("sans-serif")
            cr.set_font_size(14)
            cr.move_to(24, 30)
            cr.show_text("no monitors reported — is the plugin enabled and loaded?")
