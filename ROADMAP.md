# ROADMAP

Where this stands, and what it grows into.

`README.md` is the design. `CLAUDE.md` is the rules for working on it. This file is
what is left to do, plus the hard-won facts that would otherwise have to be
rediscovered.

Verified against **Hyprland 0.56.0** (commit `36b2e0cf`, the Omarchy/Arch package)
and cross-checked against **0.56.1**. Both compile and pass.

---

## Where this stands

**The behavioural core is done.** `geometry`, `layout_build`, `cursor_state`,
`apply` — zero Hyprland dependencies, **76,272 checks** under ASan+UBSan, clean at
`-Wconversion`, and mutation tested (deliberately broken cores were checked to
confirm the suites can actually fail). `make test` needs nothing but a compiler.

**The plugin works in a real compositor.** It passes **12 scripted in-compositor
assertions** in the test VM (`make vm-up && make vm-verify`), driven through the
real relative-pointer path. The headline result, measured rather than argued:
identical 47.05mm of physical travel moves the cursor **199 logical px on one panel
and 170 on the other**, and purely horizontal input produces **zero** physical
vertical drift across the seam while logical y moves by the predicted 104.15px.

**It works on the real desk.** Loaded on the DP-9/DP-10 hardware it behaves
correctly for ordinary cursor movement, crossing between the two panels, and
hammering the outer boundaries. That last one matters more than it sounds: edge
spamming is where a mm accumulator that drifted out of the clamped region would
show up as hysteresis, and it doesn't.

This also means the **EDID read path** is now exercised end to end. Every earlier
test used virtual outputs, which have no usable EDID, so all of them went through
the `mmcursor-monitor` override branch. On real hardware both panels report correct
EDID (600×340 and 530×300) and no override is needed.

One limit was predicted and is now confirmed: the cursor **bitmap** straddling a
seam still looks disjoint until the hotspot crosses. That is inherent to the
interposition point — a position is a single point, while the bitmap is drawn once
per overlapping monitor from still-logical monitor origins. Cosmetic, bounded, and
only fixable by moving monitor placement itself into physical space, which is item
4. See `README.md` "Known limits".

**What is still unverified** is listed in item 1. Nothing below is a known
defect — it is the list of things that have not been put under load yet.

---

## 1. Finish exercising it on real hardware

- [ ] **Rigorous / long-running use.** Ongoing. The interesting failures for this
      design are cumulative rather than immediate: slow drift, or a discrepancy
      that only appears after a monitor sleeps, a workspace switches, or the
      session has been up for days. `hyprctl mmcursor` is the instrument — compare
      `mm position` against `compositor cursor` after a long session.
- [ ] **A pointer-locked game.** The invariant says it must be unaffected *by
      construction* — `sendRelativeMotion` runs at `InputManager.cpp:154`, before
      `Pointer::mgr()->move()` at `:155`, so the client already holds the untouched
      delta before we are called. Verify anyway; "by construction" is a claim about
      code, not about reality. Alt-tabbing out of a fullscreen game is the specific
      case that historically breaks cursor state.
- [ ] **Tablet and touch.** They share `CPointerManager::move` for relative motion
      and so inherit the correction, but absolute motion goes through
      `warpAbsolute` and is only picked up by the lazy reconcile.
- [ ] **Monitor hotplug and DPMS.** `rebuildLayout()` runs on
      `monitor.layoutChanged`/`added`/`removed` and re-adopts the compositor's
      cursor, but only the VM's synthetic add/remove has been tested — not a real
      cable pull or a display waking up.
- [ ] **A scale other than 1.** Both panels are at scale 1 today. `m_size` is
      already scaled so the mathematics should be untouched, which is exactly the
      kind of "should" worth testing.

Safety, whenever loading into a live session: never autoload from config, keep a
keyboard-only escape route, and have the unload command typed but not entered
before you load. The failure mode here is "compositor alive, cursor unusable"
rather than a crash, so a filesystem snapshot is hygiene, not mitigation — the
plugin is never written to config, so nothing persists past a restart.

```sh
hyprctl plugin load  $PWD/build/mmcursor.so
hyprctl mmcursor
hyprctl plugin unload $PWD/build/mmcursor.so
```

---

## 2. Arbitrary monitor placement

Today the builder makes a single horizontal row aligned on one horizon. Real desks
are not that tidy.

- [ ] **Test the vertical offset that already exists.** `mmcursor-monitor = NAME,
      w_mm, h_mm, offset_mm` — the fourth field — is parsed and applied
      (`layout_build.cpp:54`), and it has **no test coverage at all**. It is
      implemented, not verified. Fix that before building anything on top of it.
- [ ] **Expose `explicitMMOrigin` in config.** The core already supports full 2D
      placement: `MonitorDesc::explicitMMOrigin` is honoured by `buildLayout`, such
      monitors are excluded from the row, and `firstMMOverlap()` already guards the
      mistake it invites. Only the config parsing is missing. Something like:

      ```
      mmcursor-place = DP-10, 620, -95      # mm origin, absolute on the desk
      ```

- [ ] **Decide what happens when placement is partial.** One monitor placed
      explicitly and two left to the row builder is ambiguous. Simplest honest
      rule: explicit monitors are anchors, the row is built around them, and any
      overlap is a config error — which `firstMMOverlap()` already reports.
- [ ] **Per-seam gaps** rather than one global `gap_mm`. Bezels differ between
      pairs, and a single value is a compromise.
- [ ] **Vertical stacking**, which the row builder cannot express at all. This is
      the case `explicitMMOrigin` exists for, and the point at which "row builder
      plus escape hatch" should probably become one general placement pass.

Keep all of this in the pure core. Placement is geometry, and geometry is the part
that is testable without a compositor.

---

## 3. The alignment GUI

The eventual target: drag monitors around in physical space and see the mm layout
update. The groundwork is in better shape than it looks, because the core already
speaks the right language — mm rectangles, not pixels.

Roughly in order:

- [ ] **Machine-readable state.** `hyprctl mmcursor` already prints the whole
      layout, and `debugDump` already *receives* an `eHyprCtlOutputFormat` that it
      currently ignores. Honouring it gives `hyprctl -j mmcursor` for near-free,
      and that is the GUI's read side done.
- [ ] **A runtime setter**, so the tool can preview a drag without rewriting
      `hyprland.conf` and forcing a reload on every mouse move. A hyprctl command
      taking a set of mm origins, applied through the same `buildLayout` path.
- [ ] **Config writeback** once a layout is accepted — the GUI proposes,
      `hyprland.conf` stays the source of truth.
- [ ] **The GUI itself.** `hyprtoolkit` is already present as a Hyprland dependency
      and is the obvious fit; the alternative is anything that can draw rectangles
      and shell out to `hyprctl`.

The property to preserve: the GUI is a *view over the mm layout*, not a second
source of truth. Everything it draws should come from the same `buildLayout` the
plugin uses, so a layout that looks right in the tool cannot behave differently in
the compositor. The moment the tool computes its own geometry, the two will
disagree and the disagreement will be invisible.

---

## 4. Upstreaming

Worth considering rather than maintaining a plugin forever. The architecture is a
clean fit for a compositor feature:

```
monitor = DP-9, ..., physical_mm, 600x340
input:physical_cursor_space = true
```

Inside the compositor four problems evaporate at once: you sit upstream of
relative-pointer dispatch, you see every warp natively, there is no IPC, and there
is no ABI churn — and the function hook, the single largest maintenance liability
left, disappears entirely.

There is demonstrated demand: Hyprland discussion #11927 asks for separating UI
scaling from positioning-space scaling, because aligning monitors currently forces
a scale that makes text hard to read. The mm-as-ground-truth framing is a better
answer to that request than the option actually requested.

Caveat: a PR touching a compositor's input path is a negotiation with maintainers,
not a weekend. The consolation is that the contested part will be policy — config
surface, defaults, naming — and the part already built and tested is the part that
isn't.

---

# Reference — facts worth not rediscovering

Everything below is settled. It is recorded because each item cost real time to
establish, and because a Hyprland update is exactly when you will want to re-check
it. `src/plugin.cpp` cites the establishing source line at every touchpoint; see
`README.md` for how to fetch the sources those citations refer to.

## The interposition point

`Pointer::CPointerManager::move(const Vector2D&)`, `PointerManager.cpp:831`.

**There is no usable event hook.** `Event::bus()->m_events.input.mouse.move` is
`Cancellable<Vector2D>` and looks perfect, but it emits `MOUSECOORDSFLOORED` — an
absolute, *floored* position — from inside `mouseMoveUnified`
(`InputManager.cpp:269`). It is the verdict, not the delta. No event carries
relative motion, so a function hook is forced.

Also note **`HyprlandAPI::registerCallbackDynamic` is a no-op returning `nullptr`**
(`PluginAPI.cpp:36-45`, body commented out). `Event::bus()` is the only working
path for events.

The delta arriving at `move()` is **accelerated** — `onMouseMoved` picks `unaccel`
only under `input:force_no_accel` (`InputManager.cpp:146`) — so libinput's profile
is already applied and `mmPerUnit` is purely a speed knob.

## Reconcile is a pull, not a push

`m_pointerPos` is written in exactly three places (`warpTo` at
`PointerManager.cpp:821`; `warpAbsolute` at `:915-918` and `:937`) and all are
observable through `position()`. So rather than hooking every absolute-motion path
and carrying a "this warp is ours" flag — an enumeration that can never be proven
complete, and the thing that creates the `mm → logical → mm` round-trip trap — we
compare against the position read back after our own last write.

The comparison must be against a **readback**, never the requested target:
`warpTo` runs positions through `closestValid` (`:721-796`), which perturbs points
near a seam. `tests/test_apply.cpp` fails if this is changed, and mutation testing
confirms it does.

Consequence worth remembering: reconcile is therefore **lazy**. It happens on the
next relative event, so `hyprctl mmcursor` will report
`(external move pending reconcile)` in between. That is the mechanism working.

## Two bugs that only a compositor could find

Both were in the glue. The pure core was right the first time and stayed right,
which is the argument for the split rather than an accident of it.

- **The ABI guard could never pass.** It compared `__hyprland_api_get_hash()` (the
  server's full ABI string) against `getHyprlandVersion().hash` (a bare commit
  hash), so it fired on every load. The correct comparison is against
  `__hyprland_api_get_client_hash()`. Hyprland does **not** check the hash itself —
  only the `PLUGIN_API_VERSION` string, `"0.1"` for years — so this guard is the
  plugin's own responsibility and the difference between "refuses to load" and
  "corrupts memory in the input path".

- **Hyprlang dereferences strings differently from numbers.** For a `STRING` the
  slot from `getDataStaticPtr()` holds the char pointer *itself* (one dereference);
  for `INT`/`FLOAT` it holds a pointer to the number (two). A generic template
  using two for everything read the first 8 bytes of the string's characters as a
  pointer. It did not fail at the cast — it crashed the compositor inside
  `std::string`'s constructor, and the crash report's nearest-symbol guesses named
  unrelated Hyprland functions. Hyprland's own code shows the asymmetry on adjacent
  lines (`debug/HyprCtl.cpp:1687-1689`).

When a plugin crash lands in `std::` internals, do not trust the report's symbol
names. Rebuild with `-g` and run `addr2line -Cfie` on the `mmcursor.so+0x...`
offsets; `test/vm/restart-and-load.sh` does this automatically.

## Hyprland cannot run truly headless

`CHeadlessBackend::drmFD()` returns `-1` (aquamarine `Headless.cpp:132`) and the
allocator is taken from a started backend's DRM fd (`Backend.cpp:164-177`), so a
headless-only Hyprland dies with "Cannot open backend: no allocator available". It
needs either a real GPU seat with DRM master or a parent Wayland compositor. A
container gives neither. **Do not plan a CI harness around headless-only; it does
not exist.** This was tested, not assumed.

Hence the VM (`test/vm/`): the Arch cloud image under qemu, with `seatd` for the
seat and **`bochs-display`** for the GPU. No virtio-gpu packages are needed —
`reopenDRMNode` takes a DRM lease on the primary node when it holds master
(`Backend.cpp:330-339`), so the GBM allocator never needs a `renderD*` node.

Caveat: with bochs there is no EGL device matching the DRM node, so aquamarine's
*renderer* fails and the log spams. The compositor still runs and the input path,
cursor position, hyprctl and the hook are all fully exercised, which is all the
test needs. For working rendering, install `qemu-hw-display-virtio-vga-gl` and use
`-display egl-headless`, which needs host GPU access.

## `plugin =` does not expand `~`, and the error surfaces in the wrong place

`handlePlugin` stores the declared path **verbatim** (`ConfigManager.cpp:1887-1895`)
and `loadPluginInternal` hands it straight to `dlopen` (`PluginSystem.cpp:71-82`).
Neither expands a tilde, so `plugin = ~/…/mmcursor.so` fails at *load* time with a
notification while `hyprctl configerrors` stays **clean** — the config parsed fine,
only the load failed. Hunting for a config error finds nothing.

The asymmetry is what makes it a trap: `source =` in the same file *does* expand
`~`, because it globs with `GLOB_TILDE` (`ConfigManager.cpp:1816`). Use absolute
paths for `plugin =`.

Related: Hyprland's double-load guard (`getPluginByPath` → "Cannot load a plugin
twice!") compares **path strings**, so two spellings of the same file slip past it
and a second `pluginInit` installs a **second hook** on `CPointerManager::move`,
double-applying the correction. Use one canonical absolute path everywhere. A
process-wide single-instance guard inside the plugin would close this off properly;
it is not implemented yet.

## Autoload is failure-resistant, but only up to init

Three layers make autoloading defensible: `pluginInit` runs inside `setjmp` +
try/catch and a fatal signal there is caught and the plugin unloaded
(`PluginSystem.cpp:113-126`, observed working twice); a config-declared plugin that
fails to load logs, notifies and returns without aborting startup
(`PluginSystem.cpp:226-233`); and our own ABI guard refuses a mismatched build
outright. So "Hyprland was updated and the `.so` is stale" degrades to *no plugin*,
not *no desktop*.

What is **not** covered: a crash in the hook during ordinary motion. `setjmp` only
wraps init. Autoloaded, that means a session that dies on mouse movement at every
login, recoverable only from a TTY. Hence: verify by hand first, autoload once it
is boring.

## `cursor:hotspot_padding` must be 0

Its default. It holds the cursor N logical px inside the layout while our mm clamp
stops at the true panel edge, so mm describes a position the cursor may not occupy
and walking back off an edge wastes up to N px — the exact hysteresis this project
removes everywhere else. The plugin warns when it is non-zero, and
`tests/test_apply.cpp` pins the bounded guarantee (discrepancy ≤ padding, exactly
zero at 0).

Modelling Hyprland's padding geometry inside the core was rejected deliberately: it
would couple the tested core to a compositor quirk, and Hyprland's own padding
check tests containment against a *single* monitor rect, so it already creates dead
zones at internal seams that have nothing to do with us.

## `nm` (binutils) is a runtime dependency

`findFunctionsByName` shells out to `nm -D -j` (`PluginAPI.cpp:392-406`) and
matches on the *mangled* line, which is why the hook searches for the full mangled
symbol rather than `"move"` — the latter would scan 134 unrelated symbols. Without
binutils, any hooking plugin fails to resolve its target.

## What the test suites cover

| suite | checks | pins |
|---|---|---|
| `tests/test_geometry.cpp` | 81 | the pieces compute what they claim |
| `tests/test_model.cpp` | 60,563 | properties/fuzz, a differential against a stock-Hyprland reference model, bit-identical no-op equivalence at uniform density, and the headline equal-travel property |
| `tests/test_apply.cpp` | 15,628 | the hook's decision logic against a simulated compositor that clamps like `closestValid` and gets warped behind our back |
| `test/vm/verify.sh` | 12 | the same properties inside a real compositor, driven through `zwlr_virtual_pointer_v1` |

The stock reference model in `test_model.cpp` carries its own independent clamp on
purpose: a differential test whose two sides share the code under test proves
nothing.
