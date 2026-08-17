# mmcursor

A Hyprland plugin that moves the cursor through **physical space** instead of
logical pixel space, so the pointer does not jump when it crosses between
monitors of different pixel density.

## The problem

Hyprland positions monitors by offsets in logical pixels. Two panels of
different density agree with physical reality at exactly one point — wherever
you aligned them — and diverge everywhere else.

| | resolution | physical | density |
|---|---|---|---|
| Main | 2560×1440 | 600×340 mm | 4.235 px/mm |
| Secondary (rotated) | 1080×1920 | 300×530 mm | 3.623 px/mm |

Centred on one horizon, `auto-center-right` makes the middle of the seam
seamless and everything else wrong. At the top edge the cursor lands **28.75 mm**
from where your hand expects.

## How it works

Millimetres are canonical; logical pixels are a projection.

```
input delta → mm accumulator → clamp to the panels' union → affine map → logical px
```

Per monitor the map is between two known rectangles:

```
logical = origin + (p_mm − mm_origin) × (logical_size / mm_size)
```

There is no seam correction anywhere in the code. Panels adjacent on the desk
are adjacent in mm, so the discontinuity stops being expressible.

The desk layout is derived from the arrangement Hyprland already has active, so
`auto`, `auto-center-right`, rotations and scales all just work. Physical sizes
come from EDID.

## Install

Needs Hyprland **0.56.0 or 0.56.1**, x86_64, and a classic `hyprland.conf` (Lua
configs cannot carry plugin keywords).

```sh
sudo pacman -S --needed hyprland pkgconf binutils nlohmann-json
make plugin
```

`binutils` is easy to miss and not optional: `findFunctionsByName` shells out to
`nm`, so any hooking plugin fails to load without it. The plugin must also be
built with the **same compiler as Hyprland** — the API passes C++ objects, so a
mismatch crashes at load rather than failing to build. `make plugin` runs
`check-toolchain` first and refuses on a mismatch.

Try it before making it permanent:

```sh
hyprctl plugin load  $PWD/build/mmcursor.so
hyprctl mmcursor
hyprctl plugin unload $PWD/build/mmcursor.so
```

Then add to `~/.config/hypr/hyprland.conf` — on Omarchy, the personal section at
the bottom:

```ini
plugin = /home/you/Projects/hypr-mmcursor/build/mmcursor.so
```

Three things to get right:

- **Absolute path.** `plugin =` does not expand `~` (`source =` does), and the
  resulting failure does **not** appear in `hyprctl configerrors` — the config
  parsed fine, only the load failed.
- **The same path everywhere.** The double-load guard compares path *strings*,
  so two spellings of one file install two hooks and apply the correction twice.
- **Not `exec-once`.** `plugin` is its own keyword.

A clean `configerrors` does not mean it loaded. Check `hyprctl plugin list`.

### After a Hyprland upgrade

The plugin API has no ABI stability, so an upgrade means rebuilding. The ABI
guard turns a stale build into a refusal plus a notification, not a crash. On
Omarchy, drop this in `~/.config/omarchy/hooks/post-update.d/` (executable, no
`.sample` suffix):

```bash
#!/bin/bash
cd /home/you/Projects/hypr-mmcursor || exit 0
make plugin || notify-send -u critical "mmcursor" "Rebuild failed — plugin will not load"
```

It takes effect at the next login, which is correct: the rebuilt plugin matches
the new Hyprland, not the one still running.

## Configure

Usually nothing. Placement is derived and EDID is normally right.

```
plugin {
    mmcursor {
        enabled     = true
        sensitivity = 1.0
        gap_mm      = 0.0        # bezel between panels
        align       = derive     # or top | center | bottom, forcing every seam

        # Physical size override, NATIVE orientation — rotation is applied for
        # you. Only needed when EDID lies; headless outputs always report 0x0.
        mmcursor-monitor = Main, 600, 340

        # Placement overrides. Rarely needed.
        mmcursor-place  = Secondary, at, 620, -95
        mmcursor-place  = Third, below, Main, left, 12
        mmcursor-gap    = Main, Secondary, 22
        mmcursor-offset = Secondary, 0, -4
    }
}
```

`mmcursor-place` takes `NAME, at, x_mm, y_mm` or
`NAME, right-of|left-of|above|below, ANCHOR [, align] [, offset_mm]`. Alignment
is `derive`, `top`/`left`, `center` or `bottom`/`right` — the two spellings are
the same thing, named for whichever axis reads naturally. An absolute placement
also makes that monitor the layout root, which is what sets pointer feel.

Prefer `mmcursor-offset` over `mmcursor-place`: it corrects a derivation that is
otherwise right, and survives a resolution change.

**Config changes never need a rebuild.** Hyprland watches `hyprland.conf`, so
saving is enough.

```sh
hyprctl mmcursor                        # layout, densities, how each panel was placed
hyprctl mmcursor version                # release + commit + the ABI it was built for
hyprctl mmcursor reload                 # force a re-read after `hyprctl keyword`
hyprctl mmcursor place  Secondary 620 -95   # try a position, immediately
hyprctl mmcursor offset Secondary 0 -4      # nudge, immediately
```

`place` and `offset` do not persist — positioning a monitor is a tape-measure
job, and the next reload drops them, so a tuning session cannot silently become
your configuration.

Leave `cursor:hotspot_padding` at 0. It holds the cursor N px inside the layout
while our clamp stops at the true edge, wasting up to N px when walking back off
one. The plugin warns if you set it.

### Versioning

Semantic versioning, with the commit appended when the build is not from a
release tag:

```
0.2.1                 # release build
0.2.1+7be5c2f-dirty   # built from a working tree
```

---

# Design

## Four invariants

**Never mutate the delta.** Consume it to advance the accumulator; leave the
event's delta alone. `zwp_relative_pointer_v1` carries deltas rather than
deriving them from position, so a pointer-locked game is unaffected *by
construction*. `applyRelative()` takes a delta and returns a position precisely
so this is hard to get wrong.

**mm is the only accumulator.** Never round-trip mm → logical → mm on the fast
path; logical is lossy and the error compounds.

**Clamp the accumulator, not just the projection.** Otherwise mm drifts into
dead space while the cursor sits parked at an edge, and coming back lands
somewhere else. `clampMM` is idempotent.

**Reconcile absolute moves by pulling, not pushing.** Dispatchers, tablets,
touch, pointer-lock handoff and client warps all set logical position directly.
Hooking each of them cannot be proven complete and forces the lossy round-trip
onto the fast path. Instead compare the cursor against the position read back
after our *own* last write; if it differs, something else moved it.

That comparison must be a **readback**, never the position we requested —
`warpTo` runs positions through `closestValid`, so comparing against the request
reports a phantom external move near every edge. Reconcile is therefore lazy:
`hyprctl mmcursor` shows `(external move pending reconcile)` in between, which is
the mechanism working.

## Deriving the desk layout

A logical offset cannot be converted to millimetres by dividing by a density,
because the offset spans two panels with different ones. Secondary sits at
logical `y = -240` against Main's 1440px/340mm; dividing gives 56.7 mm. The true
answer is 95 mm.

What the layout states is a *relation*: both centres sit at logical `y = 720`.
Reproduce that physically and it is exact. So for each seam the builder takes
the closest of three candidates — near edges flush, far edges flush, centres
flush — reproduces it exactly in mm, and converts only the residual through the
anchor's density. No tolerance knob.

Monitors attach by a spanning tree over logical adjacency, cheapest seam first,
so stacks, Ls and grids work and declaration order never matters. Every
placement is reported:

```
Main       mm [   0.00    0.00  600.00x340.00]  …  <- root
Secondary  mm [ 600.00  -95.00  300.00x530.00]  …  <- right-of centre Main
```

`gap_mm` defaults to 0, collapsing real bezels. That is a deliberate lie — the
honest alternative is a dead zone where the cursor visibly stalls. Collapsing at
build time rather than at projection time keeps `clampMM` idempotent.

## The hook site

`Pointer::CPointerManager::move(const Vector2D&)`. Three reasons:

- It receives a pure logical delta — the arrow, and nothing else.
- It is **downstream of relative-pointer dispatch**: `sendRelativeMotion` runs at
  `InputManager.cpp:154`, `Pointer::mgr()->move()` at `:155`. Locked clients
  already hold the untouched delta, so the invariant holds by construction.
- The delta is already **accelerated**, so our constant is purely a speed knob.

`mouseMoveUnified` would hand us an absolute position Hyprland has already
computed and already clamped — the wrong cross-seam decision, already made.

We do not warp. We rewrite the delta to `target - current` and call the
original, whose `newPos = current + delta` reproduces `target` exactly, so its
NaN guard, input-capture handling, clamping, damage and focus all still run.

A function hook is a last resort and Hyprland says so. There is no alternative:
`input.mouse.move` on the EventBus emits `MOUSECOORDSFLOORED`, an absolute
floored position, and no event carries relative motion.
(`registerCallbackDynamic` is a no-op returning `nullptr`; `Event::bus()` is the
only working path.)

Because the whole liability is one four-line hook, that is also the argument for
upstreaming — `ROADMAP.md` item 4.

## Repo layout

```
src/geometry.*        Rect, projection, clamping        no Hyprland deps
src/layout_build.*    active layout → desk layout       no Hyprland deps
src/cursor_state.*    the mm accumulator                no Hyprland deps
src/apply.hpp         reconcile + correct decision      no Hyprland deps
src/plugin.cpp        Hyprland glue — the only hyprland #include

tests/test_geometry.cpp       81 checks
tests/test_model.cpp      60,569 checks   properties, fuzz, differential vs stock
tests/test_placement.cpp     402 checks   every arrangement and override
tests/test_apply.cpp      15,628 checks   hook logic vs a simulated compositor

test/vm/                  Arch VM: three in-compositor suites
test/vpointer/            feeds real relative motion via wlr-virtual-pointer
```

Everything that can be logically wrong lives outside `plugin.cpp` and is tested
without a compositor. When `plugin.cpp` breaks on a Hyprland update, the damage
is contained to one file.

## Testing

```sh
make test                      # 4 suites, compiler only, ASan+UBSan
make plugin                    # needs Hyprland headers
make vm-up && make vm-verify   # boot the Arch VM, run the in-compositor suites
make vm-down
```

The VM needs `qemu-base cloud-image-utils` and `/dev/kvm`; `make vm-up` fetches
the image into `build/vm/`. Guest packages are provisioned by
`test/vm/user-data.in`. `bochs-display` is enough — no
`qemu-hw-display-virtio-*` needed.

Do not develop against your live session. Use the VM or a nested Hyprland, load
by hand rather than autoloading, and keep a keyboard-only escape route: you are
hooking the input path, so the plausible failure is "compositor alive, cursor
unusable". `hyprctl output create headless` gives synthetic monitors with
arbitrary density for seam testing.

## Status

Verified against Hyprland 0.56.0 and 0.56.1.

- 76,692 unit checks, mutation tested, no compositor needed
- 88 in-compositor assertions across three suites: one desk in detail, every
  layout and override via `hyprland.conf` reloads, and the autoload startup path
- Working on real hardware, including the EDID read path that VM tests bypass

Measured: 47.05 mm of physical travel moves the cursor 199 logical px on one
panel and 170 on the other, with zero physical vertical drift across the seam.

Unverified, not known-broken: pointer-locked games, tablet and touch, DPMS.
Scale ≠ 1 and hotplug are covered in the VM but not on hardware.

## Known limits

**The cursor bitmap straddling a seam looks disjoint** until the hotspot
crosses. A global position is a single point — the hotspot — and it inherits the
correction; the bitmap is drawn once per overlapping monitor from each one's
still-logical origin. Cosmetic, bounded by the cursor's size, and the same class
of artifact as a window straddling the seam at two scales. Fixing it means
moving monitor placement itself into physical space, which is a compositor
feature.

**Placement is axis-aligned and flat.** A panel tilted, or nearer to you than
its neighbour, cannot be expressed.

**Pointer warps are adopted, not corrected.** A client asking for a specific
logical pixel gets it; we only reconcile mm to match.

**The pointer moves fewer pixels per hand-inch on the lower-density panel.**
That is the point, and some people dislike it.

The union of differently-sized rectangles is not convex, so "nearest point in
the union" behaves oddly at outer corners — harmless, they are corners you
cannot move past.

## Provenance

Nothing tracked is a binary you have to trust; a checkout plus the dependencies
above reproduces everything.

`test/vpointer/wlr-virtual-pointer-unstable-v1.xml` is vendored verbatim from
Hyprland `v0.56.0`, because neither `/usr/share/hyprland/protocols` nor
`/usr/share/wlr-protocols` exists on Arch and there is nothing on the system to
generate bindings from. The Makefile prefers a system copy if one appears.

`src/plugin.cpp` cites Hyprland source lines as evidence for each claim about
compositor internals. The `.cpp` files are not in the `hyprland` package, so to
re-check them:

```sh
curl -L https://github.com/hyprwm/Hyprland/archive/refs/tags/v0.56.0.tar.gz | tar xz
curl -L https://github.com/hyprwm/aquamarine/archive/refs/tags/v0.14.0.tar.gz | tar xz
```

Not vendored — ~50 MB, only needed when re-verifying, and a stale copy would be
worse than none.

## See also

`TUTORIAL.md` is a guided read of the whole plugin. `ROADMAP.md` has what is
left plus a reference section of hard-won Hyprland facts. `CLAUDE.md` has the
rules for working on it.
