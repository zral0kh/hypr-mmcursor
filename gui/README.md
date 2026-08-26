# mmcursor-gui

A drag-to-arrange view over the mm layout `hyprctl -j mmcursor` reports.

It is intentionally *not* a general monitor-configuration tool like
`nwg-displays` (which manages logical/pixel position, resolution and scale
through `wlr-output-management`). This tool only moves monitors in the
**physical mm space** the mmcursor plugin uses for cursor motion — it never
computes its own geometry. Everything drawn comes from `buildLayout` via
`hyprctl -j mmcursor`, and every drag is previewed by asking the plugin to
rebuild through that same path (`hyprctl mmcursor place`). See
"The alignment GUI" in `../ROADMAP.md` for why that separation matters.

## Requirements

- Hyprland running with the mmcursor plugin loaded (`hyprctl mmcursor` should
  print a layout, not "EMPTY").
- Python 3.11+, PyGObject, GTK4, libadwaita. On Arch:

  ```sh
  sudo pacman -S python-gobject gtk4 libadwaita
  ```

## Running

```sh
./mmcursor-gui
```

## Installing a launcher (Omarchy / Hyprland)

Three independent, standard pieces — none of them touch Omarchy's own
files under `~/.local/share/omarchy/`, which are managed by `omarchy update`
and shouldn't be hand-edited:

1. **A command on PATH.** Symlink the entry point into `~/.local/bin` (already
   on PATH on Omarchy):

   ```sh
   ln -s "$(pwd)/mmcursor-gui" ~/.local/bin/mmcursor-gui
   ```

2. **An app-launcher entry**, so `SUPER+Space` (walker) finds it by name.
   Create `~/.local/share/applications/mmcursor-gui.desktop`:

   ```ini
   [Desktop Entry]
   Type=Application
   Name=mmcursor
   Comment=Drag-to-arrange the physical monitor layout mmcursor uses
   Exec=mmcursor-gui
   Terminal=false
   Categories=Settings;HardwareSettings;
   ```

3. **A keybinding**, in `~/.config/hypr/bindings.conf`:

   ```
   bind = SUPER CTRL, M, exec, mmcursor-gui
   ```

Why not Omarchy's own `Setup → Monitors` menu item? It's hardcoded inside
`~/.local/share/omarchy/bin/omarchy-menu` (`open_in_editor
~/.config/hypr/monitors.conf`) — that file is Omarchy source, not user
config, and editing it directly gets silently reverted (or conflicts) on the
next `omarchy update`. The keybinding above is the supported equivalent.

## What it does

- **Canvas** — every monitor drawn as a rectangle in real mm proportions,
  labelled with its EDID description ("Dell U2720Q") when it has one — the
  Hyprland port name (`DP-9`) alongside it, since that's what config keywords
  actually key on — falling back to the port name alone otherwise. Handy for
  telling panels apart while setting up hardware you don't recognise yet.
  Physical size and `px/mm` density are labelled too, colour-coded, with the
  live cursor position overlaid as a dot (from `cursor.mm` in the JSON dump).
  Scroll to zoom, drag empty space to pan, drag a monitor to move it.
- **Environment name** — a free-text field above the monitor list, purely
  cosmetic (saved as a `# environment: ...` comment in the layout file, never
  seen by Hyprlang) for telling apart the layout files from different desks.
  Shown in the window title.
- **Snapping** — dragging a monitor snaps its edges and centre-lines to any
  other monitor's edges/centre-lines within ~10 screen px, with a guide line
  while it's active. Off by construction once you're outside that radius —
  there's no modifier to fight. On release, a second pass guarantees the
  dropped position doesn't overlap anything else — pushed out along whichever
  axis needs the smaller nudge, landing flush against that edge — since a
  raw overlapping drop makes the plugin hard-refuse the whole layout.
- **Dragging pins everyone else first.** The moment you grab a monitor, every
  other monitor gets a live "stay exactly here" override before the drag
  moves anything. Without this, a monitor with no placement of its own that's
  anchored to whichever one you just grabbed — the satellite in a simple
  two-monitor desk, say — gets re-derived relative to wherever you drop it,
  and visibly drags along: correct plugin behaviour (relations track their
  anchor), surprising direct-manipulation UX. The pins are live-only, so an
  untouched monitor still keeps its real relational placement in the saved
  config — only monitors you actually drag end up in the dirty set.
- **Nudge arrows** — every edge that's free to move gets a small outward
  arrow. Clicking one opens a text field prefilled with the current
  coordinate and the operator that arrow implies (up: `− `, down: `+ `,
  left: `− `, right: `+ `) — type the amount and press Enter to add/subtract
  it, or clear the field and type a bare number for an absolute position. An
  axis with a neighbour touching it on *either* side loses both its arrows:
  since gaps between monitors are never allowed, that whole axis is fixed,
  not just the touching edge — e.g. a monitor sandwiched between two others
  gets no left/right arrows at all, only up/down.
- **Live preview** — drags call `hyprctl mmcursor place` (throttled to ~25Hz)
  so what you see updating is the plugin's actual rebuilt layout, not a local
  guess. Nothing is written to disk yet: hyprland.conf is untouched, and the
  overrides vanish on the next config reload if you never hit Apply.
- **Sidebar** — one row per monitor (mm size, density, *how* it was placed —
  root / derived / an explicit relation / a drag), plus rebuild/deferred/
  refused counters and any active warnings, so a bad layout is diagnosable
  without switching to a terminal.
- **Apply & Reload** — writes every monitor you actually dragged this session
  to `~/.config/hypr/mmcursor-layout.conf` as an absolute `mmcursor-place`
  line, then runs `hyprctl reload`. Monitors you didn't touch are left alone —
  dragging one panel doesn't freeze everyone else's derived/relational
  placement. This file is fully owned by the tool; hand edits to *other*
  monitors' lines in it survive a save. The first time, add this to
  `hyprland.conf` yourself (the tool warns if it looks missing):

  ```
  source = ~/.config/hypr/mmcursor-layout.conf
  ```

  If that line isn't there yet, Apply writes the file but **skips the
  reload** and warns instead of running it: a reload always drops every live
  preview override, and if the file isn't sourced nothing replaces what it
  drops — the arrangement you just built would visibly revert to fully
  derived, which reads as "Apply reset my layout" even though nothing was
  ever actually persisted for it to revert from.

- **Discard (↺)** — `hyprctl reload`, dropping any unsaved live overrides and
  going back to whatever hyprland.conf currently says.

## Known limitations

- Physical-size overrides (`mmcursor-monitor`) and per-seam gaps
  (`mmcursor-gap`) are config-only right now — there's no live `hyprctl`
  setter for them yet (only `place`/`offset` have one), so the GUI can't
  preview a bezel or a corrected EDID size without a reload. Extending
  `plugin.cpp`'s live-override map to those would be the natural next step,
  mirroring `g_liveOrigins`/`g_liveOffsets`.
- No undo stack beyond "Discard" (which throws away *all* unsaved drags, not
  just the last one).
