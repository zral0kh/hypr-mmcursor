// apply.hpp — the motion-application step, with the compositor abstracted out.
//
// This file exists because of a specific problem. The logic that decides what to
// do with an incoming delta — when to adopt an external cursor position, what
// corrected delta to hand back, when to get out of the way — is the most
// error-prone code in the project, and until it lived here it was stranded in
// plugin.cpp where nothing could test it. That is exactly backwards from this
// repo's whole thesis: everything that can be logically wrong belongs in a file
// with no Hyprland dependency and a test around it.
//
// So the arrow stays a single arrow, but its logic is stated here as a pure
// function of three observable values:
//
//     (current logical position, last-seen logical position, delta)
//         -> the delta to hand the compositor's own move()
//
// plugin.cpp is reduced to reading those three values, calling this, and writing
// the result back. See tests/test_apply.cpp, which simulates a whole session —
// including the compositor's own clamping and third-party warps — without a
// compositor anywhere in sight.
//
// ZERO Hyprland dependencies. Keep it that way.

#pragma once

#include "cursor_state.hpp"
#include "geometry.hpp"

#include <optional>

namespace pcs {

struct MotionPlan {
    // The delta to pass to the compositor's own relative-move function.
    //
    // nullopt means "pass the original delta through untouched" — we have no
    // usable state and stock behaviour is better than a guess. Note this is a
    // *replacement* delta, never a mutation of the event's delta: by the time
    // the compositor's move() is reached, relative-pointer clients have already
    // been handed the original. See the hook comment in plugin.cpp.
    std::optional<Vec2> correctedDelta;

    // True if an external absolute move was detected and adopted this call.
    // Purely for tests and the debug dump; the caller does not need to act on it.
    bool adoptedExternal = false;
};

// One relative-motion event.
//
//   cursor          the mm accumulator, mutated in place
//   currentLogical  the compositor's cursor position right now
//   lastSeenLogical the position read back immediately after our own last write
//   delta           the incoming logical delta
//
// The reconcile decision is a pull, not a push: if currentLogical differs from
// lastSeenLogical then something other than us moved the cursor since we last
// wrote it, and the compositor is authoritative. This replaces the alternative
// design — hooking every absolute-motion path and setting a "this warp is ours"
// re-entrancy flag — which cannot be proven complete and which round-trips
// mm -> logical -> mm on the fast path. Here the lossy round-trip happens only
// when an external move actually occurred, which is precisely when the mm state
// was already worthless.
//
// lastSeenLogical MUST be a value that was read back from the compositor after
// our previous write, never the target we asked for. The compositor clamps what
// we hand it, so comparing against our request reports a phantom external move
// every time the cursor sits near an edge.
inline MotionPlan planMotion(CursorState& cursor, const Vec2& currentLogical, const Vec2& lastSeenLogical, const Vec2& delta) {
    MotionPlan plan;

    const bool moved = currentLogical.x != lastSeenLogical.x || currentLogical.y != lastSeenLogical.y;

    if (!cursor.valid() || moved) {
        plan.adoptedExternal = true;
        if (!cursor.reconcile(currentLogical))
            return plan; // on no known monitor: stay out of the way
    }

    const auto TARGET = cursor.applyRelative(delta);
    if (!TARGET)
        return plan; // no usable layout

    plan.correctedDelta = Vec2{TARGET->x - currentLogical.x, TARGET->y - currentLogical.y};
    return plan;
}

} // namespace pcs
