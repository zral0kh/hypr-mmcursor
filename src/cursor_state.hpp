// cursor_state.hpp — the mm accumulator.
//
// This is the whole behavioural core, and it is intentionally tiny. Two entry
// points:
//
//   applyRelative()  — the fast path. Input delta -> mm -> clamp -> logical.
//   reconcile()      — something else moved the cursor; adopt its position.
//
// The invariant that matters: mm is the only accumulator. We never do
// mm -> logical -> mm on the fast path, because logical is a lossy projection
// (integer-ish pixel space, differing densities) and round-tripping through it
// accumulates error until the two spaces silently disagree.
//
// reconcile() is the pressure valve. Relative pointer motion is NOT the only
// thing that moves a Wayland cursor: `hyprctl dispatch movecursor`, tablet and
// touch input, pointer-lock/confinement handoff when a game grabs or releases,
// and client-side warps all set an absolute logical position directly. Every
// one of those bypasses applyRelative(). If they are not fed back in, mm state
// and the compositor's real cursor drift apart, and the symptom is the
// maddening kind: everything is fine until you alt-tab out of a fullscreen
// game, and then the seam is subtly wrong until you restart.
//
// Treat the compositor as authoritative at those points, not our own state.
//
// One more rule, and it is the one a refactor is most likely to break:
//
//   NEVER MUTATE THE INCOMING DELTA.
//
// applyRelative() takes a delta by const reference and returns a position. That
// asymmetry is deliberate. The delta belongs to the event, and the event is
// still going to be dispatched to zwp_relative_pointer_v1 clients, which
// consume deltas rather than deriving from cursor position. Rescale it in place
// — "simplifying" this to a void function that adjusts the event — and every
// pointer-locked game gets our mm conversion applied to its raw input.
//
// The interposition point is exactly one arrow: delta -> new global position.
// Nothing else. Everything downstream of global position inherits the fix for
// free and must not be touched.

#pragma once

#include "geometry.hpp"

#include <optional>

namespace pcs {

class CursorState {
  public:
    void setLayout(const Layout* layout) { m_layout = layout; }
    void setMMPerUnit(double v) { m_mmPerUnit = v; }
    double mmPerUnit() const { return m_mmPerUnit; }

    bool valid() const { return m_valid; }
    Vec2 positionMM() const { return m_mm; }

    // Fast path. `delta` is libinput's accelerated delta in its nominal
    // pixel-ish unit. Returns the logical position to hand the compositor, or
    // nullopt if we have no usable layout (caller should then fall through to
    // stock behaviour rather than dropping the event).
    std::optional<Vec2> applyRelative(const Vec2& delta);

    // Adopt an absolute logical position that originated elsewhere.
    // Returns false if the position maps to no monitor, in which case we mark
    // ourselves invalid rather than inventing a plausible-looking mm value.
    bool reconcile(const Vec2& logical);

    void invalidate() { m_valid = false; }

  private:
    const Layout* m_layout    = nullptr;
    Vec2          m_mm        = {};
    double        m_mmPerUnit = 1.0;
    bool          m_valid     = false;
};

} // namespace pcs
