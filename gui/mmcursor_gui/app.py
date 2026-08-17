"""The window: a header bar, a mm-space canvas, and a sidebar that mirrors
whatever `hyprctl -j mmcursor` last reported. Polls on a timer rather than
holding any kind of persistent connection — hyprctl is a request/response
CLI, there is nothing to subscribe to, and polling means a plugin reload or
monitor hotplug just shows up on the next tick instead of needing a handler.
"""

from __future__ import annotations

from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk  # noqa: E402

from . import writeback  # noqa: E402
from .canvas import MonitorCanvas  # noqa: E402
from .hyprctl import HyprctlError, MmcursorState, get_state, reload_config, set_place  # noqa: E402

_POLL_MS = 700


class MonitorRow(Gtk.ListBoxRow):
    def __init__(self, name: str) -> None:
        super().__init__()
        self.name = name
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2, margin_top=8, margin_bottom=8, margin_start=12, margin_end=12)
        self.title = Gtk.Label(xalign=0, css_classes=["heading"])
        self.subtitle = Gtk.Label(xalign=0, css_classes=["dim-label", "caption"])
        box.append(self.title)
        box.append(self.subtitle)
        self.set_child(box)

    def update(self, m) -> None:
        self.title.set_label(m.name)
        placed = m.placed_how if not m.placed_anchor else f"{m.placed_how} {m.placed_anchor}"
        self.subtitle.set_label(f"{m.mm_w:.0f}×{m.mm_h:.0f} mm  ·  {m.px_per_mm_x:.2f} px/mm  ·  {placed}")


class MmcursorGuiWindow(Adw.ApplicationWindow):
    def __init__(self, app: Adw.Application) -> None:
        super().__init__(application=app, title="mmcursor")
        self.set_default_size(1080, 680)

        self._dirty: dict[str, tuple[float, float]] = {}
        self._rows: dict[str, MonitorRow] = {}

        toolbar = Adw.ToolbarView()
        header = Adw.HeaderBar()
        toolbar.add_top_bar(header)

        fit_btn = Gtk.Button(icon_name="zoom-fit-best-symbolic", tooltip_text="Fit layout to view")
        fit_btn.connect("clicked", lambda *_: self.canvas.fit_to_view())
        header.pack_start(fit_btn)

        reset_btn = Gtk.Button(icon_name="edit-undo-symbolic", tooltip_text="Discard unsaved drags (reload hyprland.conf)")
        reset_btn.connect("clicked", self._on_reset)
        header.pack_start(reset_btn)

        self.save_btn = Gtk.Button(label="Apply & Reload")
        self.save_btn.add_css_class("suggested-action")
        self.save_btn.set_sensitive(False)
        self.save_btn.connect("clicked", self._on_save)
        header.pack_end(self.save_btn)

        self.banner = Adw.Banner(revealed=False)

        split = Adw.OverlaySplitView(sidebar_width_fraction=0.28, max_sidebar_width=340)

        sidebar_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.status_group = Adw.PreferencesGroup(title="Layout")
        self.status_label = Gtk.Label(xalign=0, wrap=True, margin_start=12, margin_end=12, margin_bottom=8, css_classes=["dim-label", "caption"])
        sidebar_box.append(self.status_label)

        self.listbox = Gtk.ListBox(css_classes=["boxed-list"], margin_start=12, margin_end=12, margin_bottom=12)
        self.listbox.set_selection_mode(Gtk.SelectionMode.SINGLE)
        self.listbox.connect("row-selected", self._on_row_selected)
        sidebar_box.append(self.listbox)

        hint = Gtk.Label(
            xalign=0, wrap=True, margin_start=12, margin_end=12, margin_top=4,
            css_classes=["dim-label", "caption"],
            label="Drag a monitor to move it · scroll to zoom · drag empty space to pan.\n"
                  "Click a free-edge arrow to nudge by a typed amount.\n"
                  "Everything previews live; nothing is written to disk until Apply.",
        )
        sidebar_box.append(hint)
        split.set_sidebar(sidebar_box)

        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        content.append(self.banner)
        self.canvas = MonitorCanvas()
        self.canvas.on_select = self._on_canvas_select
        self.canvas.on_preview = self._on_preview
        self.canvas.on_committed = self._on_committed
        content.append(self.canvas)
        split.set_content(content)

        toolbar.set_content(split)
        self.set_content(toolbar)

        self._state: MmcursorState | None = None
        self._suppress_row_signal = False
        GLib.idle_add(self._poll)
        GLib.timeout_add(_POLL_MS, self._poll)

    # -- polling / state -----------------------------------------------------

    def _poll(self) -> bool:
        try:
            state = get_state()
        except HyprctlError as exc:
            self.banner.set_title(f"hyprctl error: {exc}")
            self.banner.set_revealed(True)
            return True

        self._state = state
        self.canvas.set_state(state)
        self._sync_sidebar(state)

        if state.warnings:
            self.banner.set_title(state.warnings[0] if len(state.warnings) == 1 else f"{state.warnings[0]} (+{len(state.warnings) - 1} more — see hyprctl mmcursor)")
            self.banner.set_revealed(True)
        elif state.pending:
            self.banner.set_title(f"deferring: {state.pending}")
            self.banner.set_revealed(True)
        elif not state.layout_active:
            self.banner.set_title("plugin has no active layout — disabled or waiting on monitors")
            self.banner.set_revealed(True)
        else:
            self.banner.set_revealed(False)

        return True

    def _sync_sidebar(self, state: MmcursorState) -> None:
        self.status_label.set_label(
            f"mmcursor {state.version}  ·  gap {state.gap_mm:.1f}mm  ·  align {state.align}\n"
            f"rebuilds {state.rebuilds}  deferred {state.deferred}  refused {state.refused}"
        )

        names = {m.name for m in state.monitors}
        for name in list(self._rows):
            if name not in names:
                self.listbox.remove(self._rows.pop(name))

        self._suppress_row_signal = True
        for m in state.monitors:
            row = self._rows.get(m.name)
            if row is None:
                row = MonitorRow(m.name)
                self._rows[m.name] = row
                self.listbox.append(row)
            row.update(m)
            if m.name == self.canvas.selected():
                self.listbox.select_row(row)
        self._suppress_row_signal = False

    # -- interaction ----------------------------------------------------

    def _on_canvas_select(self, name: str | None) -> None:
        row = self._rows.get(name) if name else None
        self._suppress_row_signal = True
        self.listbox.select_row(row)
        self._suppress_row_signal = False

    def _on_row_selected(self, _box, row) -> None:
        if self._suppress_row_signal:
            return
        self.canvas.select(row.name if row else None)

    def _on_preview(self, name: str, x_mm: float, y_mm: float) -> None:
        try:
            set_place(name, x_mm, y_mm)
        except HyprctlError:
            pass  # a dropped preview frame during a drag is not worth a dialog

    def _on_committed(self, name: str, x_mm: float, y_mm: float) -> None:
        try:
            set_place(name, x_mm, y_mm)
        except HyprctlError as exc:
            self.banner.set_title(f"could not place {name}: {exc}")
            self.banner.set_revealed(True)
            return
        self._dirty[name] = (x_mm, y_mm)
        self.save_btn.set_sensitive(True)
        self._poll()

    def _on_reset(self, *_a) -> None:
        try:
            reload_config()
        except HyprctlError as exc:
            self.banner.set_title(f"reload failed: {exc}")
            self.banner.set_revealed(True)
            return
        self._dirty.clear()
        self.save_btn.set_sensitive(False)
        self._poll()

    def _on_save(self, *_a) -> None:
        if not self._dirty:
            return
        path = writeback.save_placements(self._dirty)
        conf = Path(GLib.get_home_dir()) / ".config" / "hypr" / "hyprland.conf"
        sourced = writeback.is_sourced_from(conf, path)
        try:
            reload_config()
        except HyprctlError as exc:
            self.banner.set_title(f"saved to {path}, but reload failed: {exc}")
            self.banner.set_revealed(True)
            return
        self._dirty.clear()
        self.save_btn.set_sensitive(False)
        if not sourced:
            self.banner.set_title(f"saved to {path} — add `source = {path}` to hyprland.conf so it survives a real restart")
            self.banner.set_revealed(True)
        self._poll()


class MmcursorGuiApp(Adw.Application):
    def __init__(self) -> None:
        super().__init__(application_id="dev.mmcursor.gui")
        self.connect("activate", self._on_activate)

    def _on_activate(self, *_a) -> None:
        win = self.props.active_window or MmcursorGuiWindow(self)
        win.present()


def main() -> int:
    return MmcursorGuiApp().run(None)
