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

## What it does

- **Canvas** — every monitor drawn as a rectangle in real mm proportions,
  labelled with its physical size and `px/mm` density, colour-coded, with the
  live cursor position overlaid as a dot (from `cursor.mm` in the JSON dump).
  Scroll to zoom, drag empty space to pan, drag a monitor to move it.
- **Snapping** — dragging a monitor snaps its edges and centre-lines to any
  other monitor's edges/centre-lines within ~10 screen px, with a guide line
  while it's active. Off by construction once you're outside that radius —
  there's no modifier to fight.
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
