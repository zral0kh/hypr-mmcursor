// geometry.hpp — pure geometry for physical-space cursor mapping.
//
// Deliberately has ZERO Hyprland dependencies. Everything in here is testable
// with a plain g++ invocation and no compositor. All the logic that can
// actually be *wrong* lives here; the plugin glue is dumb by design.
//
// Two coordinate spaces:
//   * mm      — physical desk space, millimetres. Ground truth. Origin and
//               axis directions are arbitrary but shared by all monitors;
//               +x right, +y down, matching Hyprland's convention.
//   * logical — Hyprland's global layout space, post-scale, post-transform.
//
// The cursor's canonical position is ALWAYS mm. Logical is derived on demand
// and never fed back in as state.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pcs {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    double right() const { return x + w; }
    double bottom() const { return y + h; }
    Vec2   center() const { return {x + w / 2.0, y + h / 2.0}; }

    // Half-open on the far edges so two edge-adjacent rects never both claim
    // the shared boundary. Ownership of a shared edge goes to the right/lower
    // rect, which matches Hyprland's own containment convention.
    bool contains(const Vec2& p) const {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }

    // Nearest point on/inside the rect. Identity if already inside.
    Vec2 clamp(const Vec2& p) const;
};

// One monitor's correspondence between the desk and the compositor layout.
//
// The two rects describe the same physical panel. `logical` comes from
// Hyprland (m_position / m_size — already scaled and transformed). `mm` is
// the panel's real extent and real place on the desk.
//
// Scale is derived, never configured: px_per_mm = logical.w / mm.w. x and y
// are handled independently because EDID physical sizes are rounded to whole
// centimetres and rarely match the pixel aspect ratio exactly.
struct MonitorMap {
    std::string name;
    Rect        mm;
    Rect        logical;

    double pxPerMMx() const { return logical.w / mm.w; }
    double pxPerMMy() const { return logical.h / mm.h; }

    Vec2 toLogical(const Vec2& p_mm) const;
    Vec2 toMM(const Vec2& p_logical) const;
};

class Layout {
  public:
    void setMonitors(std::vector<MonitorMap> monitors);

    const std::vector<MonitorMap>& monitors() const { return m_monitors; }
    bool                           empty() const { return m_monitors.empty(); }

    const MonitorMap* monitorForMM(const Vec2& p_mm) const;
    const MonitorMap* monitorForLogical(const Vec2& p_logical) const;

    // Nearest point within the union of all mm rects.
    //
    // This is what keeps the mm accumulator honest. If the cursor were allowed
    // to drift into dead space it would come back displaced (move right into a
    // bezel gap, move back, land somewhere else) — the exact hysteresis this
    // whole design exists to avoid. Clamping is idempotent, so re-clamping a
    // clamped point is a no-op.
    //
    // Caveat: the union of differently-sized rects is not convex, so at the
    // outer corners "nearest point" can behave non-monotonically. Harmless in
    // practice — those are screen corners you cannot move past anyway.
    Vec2 clampMM(const Vec2& p_mm) const;

    // mm -> logical. nullopt only if the layout is empty.
    std::optional<Vec2> toLogical(const Vec2& p_mm) const;

    // logical -> mm. Used to reconcile absolute cursor moves that bypass the
    // relative-motion path (dispatchers, tablets, pointer-lock release,
    // client-side warps). nullopt if the point is on no monitor.
    std::optional<Vec2> toMM(const Vec2& p_logical) const;

    // Names of the first pair of monitors whose mm rects overlap, if any.
    //
    // The row builder cannot produce an overlap — it stacks panels left to
    // right. explicitMMOrigin can, and a user who mistypes one gets two panels
    // claiming the same desk space. That is not a recoverable condition: which
    // monitor a point belongs to becomes declaration order, and the cursor
    // teleports between them for reasons nothing in the config explains. Callers
    // should treat a result here as a configuration error and refuse to run,
    // rather than projecting through whichever rect happened to be listed first.
    std::optional<std::pair<std::string, std::string>> firstMMOverlap() const;

  private:
    std::vector<MonitorMap> m_monitors;
};

} // namespace pcs
