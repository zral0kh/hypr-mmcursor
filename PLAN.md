# PLAN

Where this stands and what happens next.

Verified against **Hyprland 0.56.0** (commit `36b2e0cf`, the Omarchy/Arch
package) and cross-checked against **0.56.1** in a container. Both compile and
pass.

## State

**Done and tested.** `geometry`, `layout_build`, `cursor_state`, `apply` — the
whole behavioural core, including the motion-application logic that used to be
stranded in `plugin.cpp`. No Hyprland dependency. 76,272 checks under ASan+UBSan,
clean at `-Wconversion`. `make test` needs nothing but a compiler.

**Run and verified in a compositor.** `src/plugin.cpp` loads into a real
Hyprland, installs its hook, and passes 12 scripted in-compositor assertions
driven through the real relative-pointer path (`make vm-verify`). Every Hyprland
touchpoint is verified against the headers and sources with the establishing
source location cited inline.

Loading it for the first time found two bugs that no amount of static reading had
caught; both are fixed and both now have a comment explaining the trap:

- **The ABI guard could never pass.** It compared `__hyprland_api_get_hash()` —
  the server's *full ABI string* — against `getHyprlandVersion().hash`, a *bare
  commit hash*. Those can never be equal, so the plugin refused to load every
  time. The correct comparison is against `__hyprland_api_get_client_hash()`.
  Worth knowing: Hyprland does **not** check the hash itself, only the
  `PLUGIN_API_VERSION` string (`"0.1"`, unchanged for years), so this guard is
  the plugin's own responsibility and the difference between "refuses to load"
  and "corrupts memory in the input path".

- **Hyprlang dereferences strings differently from numbers, and it crashed the
  compositor.** For a `STRING` the slot from `getDataStaticPtr()` holds the char
  pointer *itself* (one dereference); for `INT`/`FLOAT` it holds a pointer to the
  number (two). A generic template using two for everything read the first 8
  bytes of the string's characters as a pointer. It did not fail at the cast — it
  failed later inside `std::string`'s constructor, and the crash report's
  nearest-symbol guesses pointed at unrelated Hyprland functions. Hyprland's own
  code shows the asymmetry on adjacent lines (`debug/HyprCtl.cpp:1687-1689`).

The lesson worth keeping: the pure core was right the first time and stayed
right; both bugs were in the glue, which is exactly the split this repo is built
around. Neither was reachable without a compositor, which is why Phase 4 was
worth the trouble.

---

## Phase 0 — Verification — DONE

All four questions are answered from source. Details and line citations live in
the comments in `src/plugin.cpp`; the short version:

- **Is there a plugin event hook for pointer motion?** **No usable one.**
  `Event::bus()->m_events.input.mouse.move` is `Cancellable<Vector2D>` and looks
  perfect, but it emits `MOUSECOORDSFLOORED` — an absolute, *floored* position —
  from inside `mouseMoveUnified` (`InputManager.cpp:269`). It is the verdict, not
  the delta. No event carries relative motion, so the function hook is forced.

  Separately: `HyprlandAPI::registerCallbackDynamic` is now a **no-op returning
  nullptr** (`PluginAPI.cpp:36-45`, body commented out). The old sketch's monitor
  and config callbacks did nothing. `Event::bus()` is the only working path.

- **What is the relative-motion entry point, and which delta?**
  `Pointer::CPointerManager::move(const Vector2D&)`, `PointerManager.cpp:831`.
  The delta is **accelerated** — `onMouseMoved` picks `unaccel` only under
  `input:force_no_accel` (`InputManager.cpp:146`) and passes the result down. So
  libinput's profile is already applied and `mmPerUnit` is purely a speed knob,
  which was the good outcome.

  Crucially this site sits *downstream* of `sendRelativeMotion`
  (`InputManager.cpp:154` precedes `:155`), so pointer-locked clients have
  already been handed the untouched delta before we run. The "never mutate the
  delta" invariant holds **by construction**, not by care.

- **Monitor fields.** `m_position`, `m_size`, `m_transform` all exist;
  `m_size` is already transformed and scaled (`Monitor.cpp:1057`) and
  `logicalBox()` is exactly `{m_position, m_size}` (`:1766`) — so it is the rect
  the cursor lives in. EDID is `m_output->physicalSize`, native orientation,
  `{0,0}` when unknown. The monitor list moved to
  `State::monitorState()->monitors()`.

  This path is **already verified against real hardware**: `hyprctl monitors`
  prints the same expression (`HyprCtl.cpp:278`), and on the target desk it
  reports 600×340 and 530×300, matching the config.

- **How does a plugin set cursor position?** `Pointer::mgr()->warpTo(logical)`,
  and `Pointer::mgr()->position()` reads it back. We do not call `warpTo`
  directly — see Phase 1.

---

## Phase 1 — The apply path — DONE

- [x] **The returned position is wired up**, but not by warping. We rewrite the
      delta to `target - current` and call the original `move()`. Its own
      `newPos = current + delta` then reproduces `target` exactly, so we inherit
      its NaN guard, its input-capture handling, `warpTo`, clamping, damage and
      focus for free. Calling `warpTo` ourselves would skip all of it.

      Note the parameter we rewrite is *not* the event's delta — that one is long
      gone by this point. It is Hyprland's internal "advance the cursor by this
      much" instruction, and rewriting it is equivalent to setting a position.

- [x] **The reentrancy trap is gone, structurally.** This plan originally called
      for a "this warp is ours" flag plus a hook on every absolute-motion path.
      Both were dropped for something stronger.

      `m_pointerPos` is written in exactly three places (`warpTo` at
      `PointerManager.cpp:821`, `warpAbsolute` at `:915-918` and `:937`) and all
      are observable through `position()`. So instead of *pushing* `reconcile()`
      from each absolute path — an enumeration that can never be proven complete,
      and the thing that created the round-trip trap in the first place — we
      **pull**: compare the cursor against the value read back after our own last
      write. Differ ⇒ something else moved it ⇒ the compositor is authoritative.

      This catches dispatchers, tablet and touch, pointer-lock and confinement
      handoff, client warps, workspace and focus warps, and anything added in a
      future release, without naming any of them and with no flag to forget. The
      lossy `mm → logical → mm` round-trip now happens *only* when an external
      move actually occurred — exactly when the mm state was already worthless.

      The comparison must be against a **readback**, never the requested target:
      `warpTo` runs positions through `closestValid` (`:721-796`), which perturbs
      points near a seam. `tests/test_apply.cpp` fails if this is changed, and
      mutation testing confirms it (see Testing below).

- [x] Reconcile on rebuild. `rebuildLayout()` invalidates and re-adopts the
      compositor's current position, so a hotplug no longer leaves stale state.

- [x] The decision logic moved out of `plugin.cpp` into `src/apply.hpp`
      (`planMotion`), which is pure and tested. `plugin.cpp` now reads two
      values, calls it, and writes the result back.

---

## Phase 2 — Usability — DONE

- [x] `sensitivity`, `gap_mm` and `align` are config values; `mmcursor-monitor`
      stays a keyword because it repeats per monitor.
- [x] `hyprctl mmcursor` dumps enable state, mm-per-unit, gap, align, the mm
      position, the compositor cursor, the last readback (flagging a pending
      reconcile), and per-monitor mm/logical rects with densities. A hyprctl
      command rather than a dispatcher, because a dispatcher can only return
      success/error.
- [x] Refuses to load rather than half-working: on a version-hash mismatch, on a
      non-legacy (Lua) config where our keyword could never be read, if the
      `move` symbol is missing or ambiguous, or if the hook fails to install.
- [x] Warns on monitors with no physical size, on overlapping mm rects, and on
      non-zero `cursor:hotspot_padding` (see Known interactions).
- [ ] `explicitMMOrigin` config syntax for non-row arrangements. Still the lowest
      priority — not needed for the DP-9/DP-10 desk. The core supports it and
      `firstMMOverlap()` guards the mistake it invites; only the config parsing is
      missing.

---

## Phase 3 — Testing — DONE for everything reachable without a compositor

`make test` builds and runs three suites, 76,272 checks:

| suite | checks | what it pins |
|---|---|---|
| `test_geometry` | 81 | the pieces compute what they claim |
| `test_model` | 60,563 | Tiers 1–4 below, plus degenerate cases |
| `test_apply` | 15,628 | the hook's decision logic against a simulated compositor |

- **Tier 1, properties/fuzz.** Seeded xorshift streams: the accumulator never
  leaves the closed mm union; `clampMM` is exactly idempotent over 50k random
  points; a path plus its exact reverse returns to origin, with the excursion
  bounded so the no-clamp precondition holds by construction rather than by hope.
- **Tier 2, differential vs stock.** A twenty-line stock model (accumulate in
  logical, carry across seams unchanged) with its own independent clamp — sharing
  our code would prove nothing. Identical input through both, both trajectories
  converted to mm: ours is continuous across the seam, stock jumps the documented
  28.75mm.
- **Tier 3, no-op equivalence.** At uniform 1 px/mm density the two models agree
  **bit-identically** over 20k random steps. A second variant at 2 px/mm asserts
  agreement to 1e-9, because there mm and logical no longer coincide and exactness
  would be a coincidence rather than a property.
- **Tier 4, the headline property.** 10mm of travel moves DP-9 by `10 × 4.2353`
  logical px and DP-10 by `10 × 3.6226`, the pixel-travel ratio equals the density
  ratio, and the physical travel is 10mm on both. This is the guarantee the plugin
  exists to provide and it was previously asserted nowhere.
- **The reentrancy trap**, in `test_apply` against a fake `CPointerManager` that
  clamps like `closestValid` and gets warped behind our back: our own writes are
  never mistaken for external moves (5000 consecutive events, zero false
  adoptions, exactly one compositor move per event), external warps are detected
  on the very next event and exactly once, and the corrected delta lands on the
  intended target.
- **Degenerate cases.** Single monitor, empty layout, zero delta, NaN and
  infinite deltas, absurd-but-finite deltas, `buildLayout` idempotence and
  declaration-order independence, overlapping mm rects.

**Mutation tested.** Both suites were checked against deliberately broken cores
to prove they can fail: swapped projection axes, removed accumulator clamp,
removed NaN guard, ignored rotation, centre-align → top-align, removed
external-move detection, unconditional reconcile, sign-flipped corrected delta,
target-as-delta, and — the one this plan called out — storing the requested
target instead of the readback. Every one is caught.

---

## Phase 4 — Running it — DONE

```sh
make vm-up        # fetch the Arch cloud image, seed cloud-init, boot, provision
make vm-verify    # push the tree, build inside, load the plugin, assert
make vm-down
```

**A truly headless test environment is not possible.** `CHeadlessBackend::drmFD()`
returns `-1` (aquamarine `Headless.cpp:132`) and the allocator is taken from a
started backend's DRM fd (`Backend.cpp:164-177`), so headless-only dies with
"Cannot open backend: no allocator available". Hyprland needs a real GPU seat with
DRM master, or a parent Wayland compositor. A container gives neither. This was
tested, not assumed — hence the VM.

The VM is the official Arch cloud image under qemu, with `seatd` for the seat and
**`bochs-display`** for the GPU. Notably *no* virtio-gpu packages are needed:
`reopenDRMNode` takes a DRM lease on the primary node when it holds master
(`Backend.cpp:330-339`), so the GBM allocator never needs a `renderD*` node, and
bochs-drm being KMS-only turns out not to matter.

Caveat worth knowing: with bochs there is no EGL device matching the DRM node, so
aquamarine's *renderer* fails ("CDRMRenderer: Can't create renderer, no matching
devices found") and the log spams. The compositor still runs, and the input path,
cursor position, hyprctl and our hook are all fully exercised — which is all this
test needs. If you ever want working rendering in there, install
`qemu-hw-display-virtio-vga-gl` and use `-display egl-headless`, which needs host
GPU access.

The two heads reproduce the real desk including the rotation, so the transform
path is exercised rather than assumed:

```
Virtual-1  2560x1440             mm 600x340   -> 4.2667 x 4.2353 px/mm
Virtual-2  1920x1080 transform 3 mm 530x300   -> 3.6000 x 3.6226 px/mm
           => logical 1080x1920 at 2560,-240, mm origin y = -95
```

Both densities and the `-95` mm origin match `test_geometry.cpp` exactly.

`test/vm/verify.sh` asserts 12 properties, all passing, all with numbers
predicted by the unit tests rather than read off a previous run:

- **Seam continuity.** 1500 relative events of +2px in x only: physical y is
  unchanged (`mm y` 0.00 → 0.00) while logical y moves 0 → **104.15**, the value
  `test_geometry.cpp` predicts. Stock would have held y=0, i.e. 28.75mm too high.
- **No hysteresis.** An exact reverse path returns to the same mm x and y, and
  outbound travel equals `events × delta × mmPerUnit` to 0.01mm.
- **The headline property, live.** Identical 47.05mm of physical travel produces
  **199 logical px on Virtual-1 and 170 on Virtual-2**, and the ratio equals the
  density ratio. This is the whole point of the project, measured in a compositor.
- **External warps adopted.** After `hyprctl dispatch movecursor`, mm agrees with
  where the cursor actually is.

Note the pull-model reconcile is **lazy by design**: it happens on the next
relative event, not at warp time. Until then `hyprctl mmcursor` shows
`(external move pending reconcile)`, which is the mechanism working, not a bug —
`verify.sh` sends a null motion event to settle it before measuring.

Still open, and only reachable with real hardware:

- [ ] Confirm a pointer-locked game is unaffected. The invariant says it must be
      *by construction* — the relative-pointer send happens upstream of our hook
      — but a virtual pointer plus a locked client would prove it.
- [ ] Tablet and touch, which share `CPointerManager::move` for relative motion
      but go through `warpAbsolute` otherwise.
- [ ] Load it on the actual DP-9/DP-10 desk, which is the only place the EDID
      read path (as opposed to the override path) is exercised end to end.

---

## Known interactions

**`cursor:hotspot_padding` must be 0** (its default). It holds the cursor N
logical px inside the layout while our mm clamp stops at the true panel edge, so
mm ends up describing a position the cursor may not occupy and walking back off
an edge wastes up to N px — the exact hysteresis this project removes everywhere
else. The plugin warns when it is non-zero, and `test_apply` pins the bounded
guarantee (discrepancy ≤ padding; exactly zero at padding 0).

Modelling Hyprland's padding geometry inside the core was rejected deliberately:
it would couple the tested core to a compositor quirk, and Hyprland's own padding
check tests containment against a *single* monitor rect, so it already creates
dead zones at internal seams that have nothing to do with us.

**`nm` (binutils) is a runtime dependency.** `findFunctionsByName` shells out to
`nm -D -j` (`PluginAPI.cpp:392-406`) and matches on the *mangled* line, so we
search for the full mangled symbol rather than `"move"`, which would scan 134
unrelated symbols.

---

## Longer term

Consider upstreaming rather than maintaining a plugin. The architecture is a
clean fit for a compositor feature:

```
monitor = DP-9, ..., physical_mm, 600x340
input:physical_cursor_space = true
```

Inside the compositor, four problems evaporate at once: you sit upstream of
relative-pointer dispatch, you see every warp natively, there is no IPC, and
there is no ABI churn — and the function hook, which is the single largest
maintenance liability left, disappears entirely.

There is demonstrated demand — Hyprland discussion #11927 asks for separating UI
scaling from positioning-space scaling, because aligning monitors currently
forces a scale that makes text hard to read. The mm-as-ground-truth framing is a
better answer to that request than the option actually requested.

Caveat: a PR touching a compositor's input path is a negotiation with
maintainers, not a weekend. The consolation is that the contested part will be
policy — config surface, defaults, naming — and the part already built and
tested is the part that isn't.
