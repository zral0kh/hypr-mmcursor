# CLAUDE.md

Context for agentic sessions on this repo. Read `README.md` for the design and
`PLAN.md` for the work queue.

## What this is

A Hyprland plugin that moves the cursor through physical millimetre space rather
than logical pixel space, so the pointer does not jump when crossing between
monitors of different pixel density.

Target hardware: DP-9 (2560×1440, 600×340mm, 4.235 px/mm) beside DP-10 (rotated
to 1080×1920, 300×530mm on the desk, 3.623 px/mm). Physically centred on one
horizon. Stock behaviour is 28.75mm off at the top edge.

## Hard rules

**Never mutate the incoming delta.** Consume it to advance the mm accumulator;
leave the event's delta untouched. `zwp_relative_pointer_v1` carries deltas
rather than deriving from position, so pointer-locked games are unaffected *by
construction*. Rescale a delta in place and every one of them breaks at once.
`applyRelative()` takes a const delta and returns a position — that asymmetry is
load-bearing, not style.

**The interposition point is exactly one arrow: delta → new global cursor
position.** Nothing else. Everything downstream of global position inherits the
correction for free: cursor rendering, surface-local motion, hit testing, focus.
Do not touch any of it.

**mm is the only accumulator.** Never round-trip mm → logical → mm on the fast
path. Logical is lossy; the error compounds until the two spaces silently
disagree.

**Clamp the accumulator, not just the projection.** `clampMM` is idempotent and
there are tests for it. Letting mm drift into dead space while the cursor parks
at an edge reintroduces exactly the hysteresis this design exists to eliminate.

**Absolute moves must be reconciled — by pulling, not pushing.** Relative motion
is not the only thing that moves a Wayland cursor: dispatchers, tablets, touch,
pointer-lock handoff and client warps all set logical position directly.

Do **not** hook each of those and push `reconcile()` from them. That enumeration
can never be proven complete, and it is what creates the mm → logical → mm
round-trip trap on the fast path. Instead compare the cursor against the position
read back after our own last write; if it differs, something else moved it. One
comparison, no flag to forget, and it covers paths added in future releases
without naming them.

That comparison must be against a **readback**, never the target we asked for.
`warpTo` runs positions through `closestValid`, which perturbs points near a seam,
so comparing against the request reports a phantom external move on every event
near an edge. `tests/test_apply.cpp` fails if this is changed.

## Architectural constraint

`src/geometry.*`, `src/layout_build.*`, `src/cursor_state.*`, `src/apply.hpp` have
**zero** Hyprland dependencies and must keep it that way. All logic that can be
wrong lives there and is unit-tested without a compositor. `src/plugin.cpp` is the
only file allowed to touch Hyprland internals, so that when a Hyprland update
breaks things the damage is contained to one file.

Do not "simplify" by pulling compositor types into the core.

## Verified vs unverified

Verified against **Hyprland 0.56.0** (commit `36b2e0cf`, the Arch/Omarchy
package), cross-checked against 0.56.1. Both compile and pass.

Core: written, tested, **76,272 checks** green under ASan+UBSan, clean at
`-Wconversion`, and mutation tested — deliberately broken cores were checked to
confirm the suites can actually fail.

`src/plugin.cpp`: written, builds clean, every Hyprland touchpoint checked
against the installed headers and the 0.56.0 sources with the establishing source
location cited inline. The `TODO(verify)` markers are gone because the questions
were answered, not because they were dropped.

**It has been loaded into a real compositor and passes 12 in-compositor
assertions** (`make vm-up && make vm-verify`). Still untested: pointer-locked
games, tablet/touch, and the EDID read path on real hardware. See `PLAN.md`
Phase 4.

Loading it the first time found two glue bugs that static reading had not: an ABI
guard comparing a full ABI string against a bare commit hash (so it could never
pass), and Hyprlang's undocumented asymmetry where a STRING config value needs
ONE dereference and INT/FLOAT need two (which crashed the compositor inside
std::string's constructor, with a backtrace pointing nowhere near the cause).
Both now carry a comment. The pure core was right and stayed right — which is the
argument for the split, not an accident of it.

Note a hard constraint discovered the hard way: **Hyprland cannot run truly
headless.** `CHeadlessBackend::drmFD()` returns -1 and the allocator comes from a
started backend's DRM fd, so headless-only dies with "no allocator available". It
needs a real GPU seat or a parent compositor. Do not plan a CI harness around
headless-only; it does not exist.

## Testing posture, addendum

**Ask before launching a nested compositor or a VM.** This runs on the user's
daily-driver desktop. Building and running containers non-interactively was
fine; spawning a second compositor was not welcome unannounced.

## Commands

```sh
make test             # compiler only, no Hyprland, ASan+UBSan, 3 suites
make plugin           # needs Hyprland headers; checks toolchain first
make check-toolchain  # compares your compiler against Hyprland's
make load / unload    # into a RUNNING compositor — VM or nested only, never live

make vm-up            # fetch + boot + provision the Arch test VM (needs qemu)
make vm-verify        # push tree, build inside, load plugin, run 12 assertions
make vm-ssh / vm-down
```

`make vm-verify` is the one that would catch a regression a Hyprland update
introduced. Run it before believing anything about runtime behaviour.

Run `make test` after every change to the core. It is fast. Coordinate bugs found
here cost nothing; the same bugs found by moving a mouse across a screen cost an
afternoon.

When touching the hook, change `src/apply.hpp` and not `plugin.cpp`. The
decision logic lives there precisely so it is testable, and
`tests/test_apply.cpp` simulates an adversarial compositor around it. Inlining
"just one condition" back into the hook is how it became untestable the first
time.

## Testing posture

Never develop against the live session. Use the VM (`make vm-up`), or a nested
Hyprland. Load manually with `hyprctl plugin load` — never autoload from config,
because a crash-on-init means recovering from a TTY, and this plugin has already
crashed a compositor on load once. Keep a keyboard-only escape route: you are
hooking the input path, so the plausible failure mode is "compositor alive, cursor
unusable".

When a plugin crash lands in `std::` internals, do not trust the crash report's
symbol names — they are nearest-symbol guesses and will name unrelated Hyprland
functions. Rebuild with `-g` and run `addr2line -Cfie` on the `mmcursor.so+0x...`
offsets; `test/vm/restart-and-load.sh` does this automatically.
