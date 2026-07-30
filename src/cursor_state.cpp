#include "cursor_state.hpp"

#include <cmath>

namespace pcs {

std::optional<Vec2> CursorState::applyRelative(const Vec2& delta) {
    if (!m_layout || m_layout->empty())
        return std::nullopt;

    // A misbehaving device, or a driver quirk, can hand over a non-finite
    // delta. One of them reaching the accumulator poisons it permanently: NaN
    // survives every subsequent addition, every comparison in clampMM returns
    // false, and the cursor never recovers for the life of the session.
    //
    // This guard lives here rather than in the plugin on purpose. It is an
    // invariant of the accumulator, not of the compositor glue, so it belongs
    // in the file that has tests.
    const double dx = std::isfinite(delta.x) ? delta.x : 0.0;
    const double dy = std::isfinite(delta.y) ? delta.y : 0.0;

    if (!m_valid) {
        // No trustworthy mm state. Seed at the centre of the first monitor
        // rather than guessing; the next reconcile() will correct us.
        m_mm    = m_layout->monitors().front().mm.center();
        m_valid = true;
    }

    Vec2 next{m_mm.x + dx * m_mmPerUnit, m_mm.y + dy * m_mmPerUnit};

    // Clamp the accumulator itself, not just the projection. Letting mm drift
    // outside the union and only clamping on the way out is what reintroduces
    // hysteresis: the cursor would sit visually parked at an edge while the
    // internal position kept travelling, and coming back would land somewhere
    // else entirely.
    m_mm = m_layout->clampMM(next);

    return m_layout->toLogical(m_mm);
}

bool CursorState::reconcile(const Vec2& logical) {
    if (!m_layout)
        return false;

    const auto mm = m_layout->toMM(logical);
    if (!mm) {
        m_valid = false;
        return false;
    }

    m_mm    = m_layout->clampMM(*mm);
    m_valid = true;
    return true;
}

} // namespace pcs
