# mmcursor

A Hyprland plugin that moves the cursor through **physical space** instead of
logical pixel space.

## The problem

Hyprland positions monitors by rigid offsets in logical pixels. When two panels
have different pixel densities, logical space and physical space only agree at
the single point where you aligned them. Everywhere else they diverge, and the
cursor jumps when it crosses the seam.

Concretely, on the desk this was written for:

| | resolution | physical | density along the seam |
|---|---|---|---|
| DP-9 (Lenovo P27h-30) | 2560×1440 | 600×340 mm | 4.235 px/mm |
| DP-10 (Acer CB242Y, rotated) | 1080×1920 | 300×530 mm | 3.623 px/mm |

Physically centred on the same horizon, `auto-center-right` makes the centre
seamless and everything else wrong. At the top edge of DP-9 the cursor lands
**28.75 mm** away from where your hand says it should. There is a test asserting
exactly that number, because it is the thing this plugin exists to delete.

## The model

Millimetres are the ground truth. Logical pixels are a projection.

```
                 relative input delta
                          │
                          ▼
              mm accumulator (canonical)
                          │
                clamp to union of panels
                          │
                          ▼
        per-monitor affine projection → logical px
                          │
                          ▼
                     compositor
```

Per monitor the projection is a plain affine map between two known rectangles:

```
logical = M.logical_origin + (p_mm − M.mm_origin) × (M.logical_size / M.mm_size)
```

No warping, no special-casing the seam, no correction term. The seam problem
does not get fixed so much as it stops being expressible.

### Why the config is in millimetres

Because that is the unit the hardware already emits. EDID carries the panel's
physical size, Hyprland surfaces it as `physical size (mm)`, and it is the only
number in the system that describes the real world. "How far apart are my
monitors vertically, in mm" is answerable with a tape measure. The logical-pixel
equivalent is only answerable by recomputing it every time a scale changes.

### Four invariants worth not breaking

**Never mutate the delta.** Consume it to advance the mm accumulator; leave the
event's own delta untouched. This is what keeps the blast radius correct.
`zwp_relative_pointer_v1` does not derive from cursor position — it carries
deltas — so a pointer-locked game is unaffected *by construction*, not because
anything special-cased it. Rescale the delta in place and you corrupt every
relative-pointer client at once. `applyRelative()` takes a delta and returns a
position precisely so this is hard to get wrong; it is exactly the kind of
indirection a later refactor will helpfully "simplify" into a bug.

The interposition point is one arrow: **delta → new global cursor position**.
Everything downstream derives from global position and inherits the correction
for free — surface-local `wl_pointer.motion`, hit testing, focus, drag tracking.
None of it needs to know millimetres exist.

One honest exception, since a global position is a single *point*: what inherits
the correction is the hotspot. The cursor *bitmap* is drawn once per monitor it
overlaps, each from that monitor's own still-logical origin, so while the image
straddles a seam its two halves do not line up. Confirmed on hardware, cosmetic,
and out of scope by construction — see "Known limits".

**mm is the only accumulator.** Never round-trip mm → logical → mm on the fast
path. Logical is lossy and the error compounds until the two spaces silently
disagree.

**Clamp the accumulator, not just the projection.** If mm is allowed to drift
into dead space while the cursor sits visually parked at an edge, coming back
lands you somewhere else. That is the hysteresis this design exists to avoid.
`clampMM` is idempotent; there is a test for it and a test for the walk-back
case.

**Absolute moves must be reconciled — by pulling, not pushing.** Relative motion
is not the only thing that moves a Wayland cursor. `hyprctl dispatch movecursor`,
tablets, touch, pointer-lock handoff when a game grabs or releases, and
client-side warps all set a logical position directly. Skip this and everything
works fine until you alt-tab out of a fullscreen game.

The tempting fix is to hook each of those and push `reconcile()` from them. Don't:
that enumeration can never be proven complete, and it forces an mm → logical → mm
round-trip onto the fast path. Instead compare the cursor against the position
read back after our *own* last write. If it differs, something else moved it. One
comparison, no flag to forget, and it covers paths added in future Hyprland
releases without naming any of them.

Two consequences worth knowing. The comparison must be against a **readback**,
never the position we requested — the compositor clamps what we hand it, so
comparing against the request reports a phantom external move near every edge. And
reconcile is therefore **lazy**: it happens on the next relative event, so
`hyprctl mmcursor` will show `(external move pending reconcile)` in between. That
is the mechanism working.

### Bezel gaps

`BuildOptions::gapMM` defaults to `0.0`, which makes panels edge-adjacent in mm
space even though real bezels exist. That is a deliberate lie: the honest
alternative is a dead zone where the cursor is "in the bezel" and visibly
stalls. Set it to a real measurement if you would rather have physical honesty.

Collapsing at build time rather than special-casing gaps at projection time is
what keeps `clampMM` idempotent.

## Layout

```
src/geometry.hpp/.cpp        Rect, projection, clamping.       No Hyprland deps.
src/layout_build.hpp/.cpp    EDID + transform → desk layout.   No Hyprland deps.
src/cursor_state.hpp/.cpp    The mm accumulator.               No Hyprland deps.
src/apply.hpp                Reconcile + correct decision.     No Hyprland deps.
src/plugin.cpp               Hyprland glue. The only file with a hyprland #include.

tests/test_geometry.cpp         81 checks — the pieces compute what they claim.
tests/test_model.cpp        60,563 checks — the headline properties, and a
                                           differential vs stock behaviour.
tests/test_apply.cpp        15,628 checks — the hook's logic against a
                                           simulated, adversarial compositor.

test/vm/run.sh               Arch VM: fetch, seed, boot, provision.
test/vm/hyprland.conf        Reproduces this desk on two virtual heads.
test/vm/restart-and-load.sh  Restart + load + triage a crash, inside the VM.
test/vm/user-data.in         cloud-init template; `run.sh seed` fills it in.
test/vm/verify.sh            12 assertions inside a running compositor.
test/vpointer/               Feeds real relative motion via wlr-virtual-pointer.
test/nested.conf             Alternative to the VM: a nested-session config.

README.md                    This file — the design, and why it is shaped this way.
ROADMAP.md                   What is left to do, plus the facts worth not
                             rediscovering (hook site, the two glue bugs, why
                             headless is impossible).
CLAUDE.md                    Rules for agentic sessions on this repo.
```

The split is the point. Everything that can be logically wrong lives outside
`plugin.cpp` and is covered by tests. When `plugin.cpp` breaks on a Hyprland
update — and it will — the fix stays confined to that file.

`apply.hpp` exists because of that rule rather than in spite of it. The logic
deciding when to adopt an external cursor position and what corrected delta to
return is the most error-prone code here, and while it lived in `plugin.cpp`
nothing could test it. So it moved, and `plugin.cpp` was reduced to reading two
values, calling `planMotion`, and writing the result back.

## Status

Verified against **Hyprland 0.56.0** (the current Arch/Omarchy package) and
cross-checked against 0.56.1. Both compile and pass.

The core is written and tested — 76,272 checks, no compositor needed, mutation
tested. `src/plugin.cpp` is written, builds clean, and every Hyprland touchpoint
carries the source location that establishes it.

**It runs.** In the test VM it passes 12 scripted assertions driven through the
real relative-pointer path (`make vm-up && make vm-verify`). The headline result:
identical 47.05mm of physical travel moves the cursor 199 logical px on one panel
and 170 on the other, and purely horizontal input produces zero physical vertical
drift across the seam while logical y moves by the predicted 104.15px.

**And it works on the real desk.** Loaded on the DP-9/DP-10 hardware, ordinary
movement, crossing between panels and hammering the outer boundaries all behave
correctly. Edge hammering is the interesting one: it is where an mm accumulator
that had drifted outside the clamped region would surface as hysteresis, and it
doesn't. That run also exercises the EDID *read* path, which every VM test had
bypassed — virtual outputs have no usable EDID, so all of them went through the
`mmcursor-monitor` override branch instead.

Not yet put under load — none of it known-broken, just unverified: pointer-locked
games, tablet and touch, monitor hotplug and DPMS, and any scale other than 1. See
`ROADMAP.md` item 1.

### The hook site

`Pointer::CPointerManager::move(const Vector2D&)`. Three things make it the right
one:

It receives a pure logical delta — the arrow, and nothing else.

It sits **downstream of relative-pointer dispatch**. In `onMouseMoved`,
`sendRelativeMotion` runs at `InputManager.cpp:154` and `Pointer::mgr()->move()`
at `:155`. So pointer-locked clients have already been handed the untouched
libinput delta before we are called: the "never mutate the delta" invariant holds
*by construction*, not because anything here is careful. A game cannot see our
correction even in principle.

The delta is **accelerated** — `onMouseMoved` only picks `unaccel` under
`input:force_no_accel` — so libinput's profile is already applied and our
mm-per-unit constant is purely a speed knob.

Contrast `CInputManager::mouseMoveUnified`, which hands you an absolute position
Hyprland has *already* computed and *already* clamped. It has made the wrong
cross-seam decision before you see it, and you would be reverse-engineering a
delta to undo work just done. Get the delta, not the verdict.

We do not warp directly. We rewrite the delta to `target - current` and call the
original, whose own `newPos = current + delta` then reproduces `target` exactly —
so the NaN guard, input-capture handling, clamping, damage and focus all still
run. That parameter is not the event's delta; it is Hyprland's internal "advance
the cursor by this much", and rewriting it *is* setting a position.

### Why a function hook at all

Hyprland's guidelines call function hooks a last resort and the easiest thing to
break between versions. They're right, and there is no alternative here.
`Event::bus()->m_events.input.mouse.move` is `Cancellable<Vector2D>` and looks
perfect, but it emits `MOUSECOORDSFLOORED` — an absolute, *floored* position —
from inside `mouseMoveUnified`. No event carries relative motion.

(Note `HyprlandAPI::registerCallbackDynamic` is now a no-op returning `nullptr`;
`Event::bus()` is the only working path for events.)

Because the whole liability is concentrated in one four-line hook, that is also
the strongest argument for upstreaming this instead — see `ROADMAP.md` item 4.

## Config

```
plugin {
    mmcursor {
        enabled = true

        # name, native width mm, native height mm, vertical offset mm
        # Sizes are in the panel's NATIVE orientation; rotation is applied
        # for you. Use 0 to trust EDID.
        mmcursor-monitor = DP-9,  600, 340, 0
        mmcursor-monitor = DP-10, 530, 300, 0
    }
}
```

EDID physical sizes are wrong often enough that the override path is not
optional. Yours look plausible — 600×340 and 530×300 are sane for a 27" and a
24" — but do not trust the field in general, and headless outputs always report
`0x0`.

## Loading it on startup

Once you have verified it by hand, make it permanent with the `plugin` keyword in
`~/.config/hypr/hyprland.conf` — on Omarchy, the "add any other personal Hyprland
configuration below" section at the bottom is the right place, since everything
above it is sourced from `~/.local/share/omarchy/` and must not be edited.

```ini
# mmcursor — cursor motion in physical mm across mismatched-density monitors
plugin = /home/you/Projects/hypr-mmcursor/build/mmcursor.so
```

Not `exec-once` in `autostart.conf`. `plugin` is a distinct keyword handled by
`handlePlugin`; `exec-once` runs a program and would do nothing here.

### Use an absolute path — `plugin =` does not expand `~`

This is the one that costs you an afternoon, because the failure is quiet and the
error appears in the wrong place.

Hyprland stores the declared path **verbatim** (`handlePlugin`,
`ConfigManager.cpp:1887-1895`) and hands it straight to `dlopen`
(`PluginSystem.cpp:71-82`). Neither expands a tilde. So `plugin = ~/Projects/...`
fails at *load* time with a notification, while `hyprctl configerrors` stays
**clean** — because the config parsed perfectly; only the load failed. If you go
looking for a config error you will not find one.

What makes it easy to trip over is the asymmetry inside the same file: `source =`
*does* expand `~`, because it globs the path with `GLOB_TILDE`
(`ConfigManager.cpp:1816`). `plugin =` does not touch the string at all.

### Use the *same* path everywhere

Hyprland refuses to load a plugin twice by path string — `getPluginByPath`, then
`"Cannot load a plugin twice!"`. But that check compares **strings**, so two
different spellings of the same file slip past it. A second `pluginInit` then
installs a **second hook** on `CPointerManager::move`, and the mm correction gets
applied twice.

So pick one canonical absolute path and use it for the config line *and* for any
manual `hyprctl plugin load`. If you have been testing by hand, unload first:

```sh
hyprctl plugin unload /home/you/Projects/hypr-mmcursor/build/mmcursor.so
```

### Why autoloading is safe here

Autoloading an input-path plugin sounds reckless. It is defensible on 0.56 because
there are two independent layers underneath it, plus one of our own:

1. `pluginInit` runs inside `setjmp` + try/catch (`PluginSystem.cpp:113-126`).
   A **fatal signal** during init is caught and the plugin unloaded. Observed
   working twice while this plugin was being developed — both crashes left the
   compositor running.
2. A config-declared plugin that fails to load logs an error, raises a
   notification, and returns (`PluginSystem.cpp:226-233`). Startup continues.
3. Our own ABI guard refuses to load against a different Hyprland build at all.

Together those make the recurring real-world case — Hyprland gets updated, the ABI
string changes, the stale `.so` no longer matches — degrade to **"no plugin, plus a
notification"** rather than "no desktop". That is the whole reason the ABI guard
exists.

**The residual risk, stated plainly:** the `setjmp` only wraps *init*. A crash in
the hook during ordinary motion is **not** caught and would take the session down —
and while autoloaded, on every login. Recovery is a TTY (`Ctrl+Alt+F3`) and
commenting out the `plugin` line. That is why you load it by hand and use it for a
while before putting it in the config, not the other way round.

### Rebuild after every Hyprland update

Hyprland's plugin API has no ABI stability, so an update means the plugin stops
loading until rebuilt. Nothing breaks; it just silently stops working, which is
worse in its own way. On Omarchy the idiomatic fix is a post-update hook — drop an
executable file (no `.sample` suffix) in `~/.config/omarchy/hooks/post-update.d/`:

```bash
#!/bin/bash
# Rebuild mmcursor after a Hyprland upgrade; without this the ABI guard will
# correctly refuse to load it. Takes effect at the next login, which is right:
# the rebuilt plugin matches the NEW Hyprland, not the one still running.
cd /home/you/Projects/hypr-mmcursor || exit 0
make plugin || notify-send -u critical "mmcursor" "Rebuild failed — plugin will not load"
```

`make plugin` runs `check-toolchain` first, so a compiler mismatch fails loudly
here instead of crashing at load. If you also update with plain `pacman -Syu`, a
`/etc/pacman.d/hooks/` hook on `Target = hyprland` catches every path, at the cost
of needing root.

After editing the config, validate:

```sh
hyprctl reload && hyprctl configerrors
```

Remember that a clean `configerrors` does **not** mean the plugin loaded — check
`hyprctl plugin list` and `hyprctl mmcursor` for that.

## Dependencies

Four separate sets, because they genuinely are separate — the whole point of the
architecture is that the interesting half needs none of the heavy ones.

**To run the unit tests: a C++23 compiler. That is the entire list.** No Hyprland,
no headers, no compositor, no GPU. 76,272 checks in a couple of seconds.

```sh
make test
```

**To build the plugin:**

| need | why |
|---|---|
| `hyprland` | Arch's package ships the headers at `/usr/include/hyprland` |
| the **same compiler** Hyprland was built with | the plugin API passes C++ objects, so a mismatch crashes at load rather than failing to build. GCC 16.1.1 for Arch's 0.56.0. `make check-toolchain` refuses to proceed on a mismatch |
| `pkgconf` | build flags, via `hyprland.pc` (pulls in pixman, libdrm, cairo, …) |

**To *use* the plugin at runtime:**

| need | why |
|---|---|
| Hyprland with a **matching ABI string** | the plugin compares `__hyprland_api_get_hash()` against its own and refuses to load otherwise. Verified on 0.56.0 and 0.56.1 |
| **`binutils`** | not optional and easy to miss: `HyprlandAPI::findFunctionsByName` shells out to `nm`, so *any* hooking plugin fails to resolve its target without it |
| **x86_64** | Hyprland's function hooks are x86_64-only |
| a classic **`hyprland.conf`** | plugin config *keywords* do not exist under a Lua config, so `mmcursor-monitor` would be unreadable. The plugin refuses to load rather than silently run on an uncorrectable EDID-only layout |

**To run the in-compositor tests** — host side:

```sh
sudo pacman -S --needed qemu-base cloud-image-utils
```

plus `curl`, `openssh`, `tar` and access to `/dev/kvm` (world-writable by
default). Nothing else; no host Python, and no display.

Do not download the cloud image by hand — `make vm-up` fetches it into
`build/vm/` and resizes it. A copy in the project root is ignored by git but
wastes 555MB.

Guest side is provisioned automatically by `test/vm/user-data.in`:
`hyprland seatd base-devel git binutils wayland python pkgconf`. Three of those
were originally present only by luck — `binutils` and `wayland` via Hyprland's own
dependencies, `python` via cloud-init — so they are named explicitly.

Notably **not** needed: any `qemu-hw-display-virtio-*` package. `bochs-display`
works, because `reopenDRMNode` takes a DRM lease on the primary node and the GBM
allocator never needs a render node.

## Building

```sh
make test             # compiler only, no Hyprland, ASan+UBSan, 3 suites
make plugin           # needs Hyprland headers; checks the toolchain first
make check-toolchain  # compare your compiler against Hyprland's

make vm-up            # fetch, boot and provision the Arch test VM
make vm-verify        # build inside it, load the plugin, run 12 assertions
make vm-down
```

## Artifacts and provenance

Nothing in this repo is generated-but-untracked, and nothing tracked is a binary
you have to take on trust. A checkout plus the dependencies above reproduces
everything. Verified by copying only the non-ignored files into an empty
directory and building from scratch.

| artifact | produced by | tracked? |
|---|---|---|
| `build/test_geometry`, `build/test_model`, `build/test_apply` | `make test` | no |
| `build/mmcursor.so` | `make plugin` | no |
| `test/vpointer/vpointer` | `make -C test/vpointer` | no |
| `test/vpointer/*-protocol.c`, `*-client-protocol.h` | `wayland-scanner`, from the vendored XML | no |
| `build/vm/arch.qcow2` | `test/vm/run.sh fetch` | no |
| `build/vm/seed.iso`, `build/vm/user-data` | `test/vm/run.sh seed`, from `test/vm/user-data.in` | no |
| `build/vm/id_ed25519{,.pub}` | `test/vm/run.sh seed` — a throwaway keypair, generated so no personal key ends up in a disposable VM | no |

Two tracked files are copies of upstream material, so their origin is worth
stating rather than leaving as mystery bytes:

**`test/vpointer/wlr-virtual-pointer-unstable-v1.xml`** — vendored verbatim from
the Hyprland source tree, `protocols/` directory, tag `v0.56.0`. This is *not*
redundant: neither `/usr/share/hyprland/protocols` nor `/usr/share/wlr-protocols`
exists on an Arch install, so there is nothing on the system to generate the
client bindings from. `test/vpointer/Makefile` prefers a system copy if one ever
appears and falls back to this one. To refresh it:

```sh
curl -L https://raw.githubusercontent.com/hyprwm/Hyprland/v0.56.0/protocols/wlr-virtual-pointer-unstable-v1.xml \
     -o test/vpointer/wlr-virtual-pointer-unstable-v1.xml
```

**The Hyprland source, for re-checking the citations.** `src/plugin.cpp` cites
specific lines (`PointerManager.cpp:831`, `InputManager.cpp:154`, and so on) as
the evidence for each claim it makes about compositor internals. Those citations
are only useful if you can open the files. Headers come with the `hyprland`
package, but the `.cpp` files do not, so:

```sh
curl -L https://github.com/hyprwm/Hyprland/archive/refs/tags/v0.56.0.tar.gz | tar xz
# aquamarine, for the allocator and headless-backend claims:
curl -L https://github.com/hyprwm/aquamarine/archive/refs/tags/v0.14.0.tar.gz | tar xz
```

Deliberately not vendored — it is ~50MB, it is only needed when re-verifying, and
a stale copy would be worse than none.

## Testing safely

Do not develop this against your live session.

**Nested Hyprland.** Run a nested instance inside a window on your normal
session and load the plugin there. A segfault kills the nest, not your desktop.
This is the single most valuable safety measure and Hyprland's own plugin docs
recommend it.

**Headless outputs for the seam.** `hyprctl output create headless` gives you a
synthetic monitor whose resolution, scale, position and transform you can set
freely. Make two with deliberately mismatched densities and test crossings
without touching real hardware. They report `physical size (mm): 0x0`, so you
need the `mmcursor-monitor` override path working first — which you want
anyway.

**Never autoload during development.** Load with `hyprctl plugin load
/path/to/mmcursor.so`. Hyprland does catch a crash during plugin init (see
"Loading it on startup"), but it does *not* catch one in the hook afterwards — and
a plugin listed in your config that dies on mouse movement means fixing it from a
TTY, every login. Autoload once it is boring, not while you are changing it.

**Keep an escape hatch.** Ctrl+Alt+F3, or SSH in from another machine. You are
hooking the *input* path, so the plausible failure mode is "compositor alive,
cursor unusable" — you want a keyboard-only way out.

**Test the maths outside the compositor.** `make test` is 20 ms. Coordinate bugs
found in a unit test cost nothing; the same bugs found by moving a mouse across
a screen cost an afternoon.

## Known limits

**The cursor *bitmap* straddling a seam still looks wrong, and cannot be fixed
here.** Confirmed on real hardware: move the pointer so the cursor's body overlaps
the seam while the hotspot is still on the first panel, and the two halves are
visibly disjoint. As soon as the hotspot crosses, it snaps to correct.

That is the design working, not failing. The interposition point is one arrow —
delta → global cursor position — and a position is a single point, the hotspot.
Everything downstream inherits the correction *for that point*. The bitmap,
though, is drawn once per monitor it overlaps, and each monitor places it from the
global position using its own scale and logical origin. The monitors are still
edge-adjacent in *logical* space, so the half drawn on one panel and the half drawn
on the other do not line up physically.

This is the same class of artifact as a window straddling the seam rendering at two
different scales, and it is pre-existing. Fixing it means changing the monitors'
logical placement, which changes window layout, workspace geometry and hit testing
along with it — i.e. the compositor-level feature discussed in `ROADMAP.md` item 4,
not something a plugin at this interposition point should attempt. It is bounded
(at most the cursor's size, only while straddling) and purely cosmetic: the hotspot,
which is what actually clicks, is correct throughout.

If it bothers you, `cursor:hotspot_padding` is worth an experiment — Hyprland's
clamp tests containment against a *single* monitor rect, so a non-zero padding
should keep the cursor away from seams entirely and suppress the straddle. Note the
tradeoff is real and measured: it reintroduces up to that many pixels of dead travel
at every edge, which is the next limit.

**`cursor:hotspot_padding` must be 0**, which is its default. It holds the cursor
N logical px inside the layout, while our mm clamp stops at the true panel edge.
mm then describes a position the cursor is not allowed to occupy, and walking back
off an edge wastes up to N px — the exact hysteresis this project removes
everywhere else. The plugin warns if you set it, and `test_apply.cpp` pins the
bounded version (discrepancy ≤ padding, exactly zero at 0).

Modelling Hyprland's padding geometry inside the core was rejected on purpose: it
would couple the tested core to a compositor quirk, and Hyprland's own padding
check tests containment against a *single* monitor rect, so it already creates
dead zones at internal seams that have nothing to do with us.

**binutils is a runtime dependency.** `findFunctionsByName` shells out to `nm` to
resolve the hook target, so a plugin that hooks anything silently fails to load
without it.

**Build with the same compiler as Hyprland.** The plugin API passes C++ objects,
so a mismatch crashes at load rather than failing to build. `make check-toolchain`
compares your compiler against the one recorded in the Hyprland binary and
refuses to proceed if they differ.


Interactive window dragging should inherit the fix, since drag tracking is
downstream of cursor position. What is *not* fixed is that a window straddling
the seam renders at two different scales — but that is pre-existing and
orthogonal to anything here.

Pointer warps issued by clients or dispatchers are adopted, not corrected. If
something asks for a specific logical pixel, it gets that pixel; we only
reconcile our mm state to match. That is the right call — those callers mean
logical coordinates.

The row builder handles a single horizontal row, ordered by existing logical x.
Anything else needs `explicitMMOrigin` per monitor.

Pointer moves fewer pixels per hand-inch on the lower-density panel. That is
correct, and some people hate it.

The union of differently-sized rectangles is not convex, so "nearest point in
the union" can behave oddly at the outer corners. Harmless — those are screen
corners you cannot move past anyway.
