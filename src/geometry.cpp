#include "geometry.hpp"

#include <algorithm>
#include <limits>

namespace pcs {

namespace {
double dist2(const Vec2& a, const Vec2& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return dx * dx + dy * dy;
}
} // namespace

Vec2 Rect::clamp(const Vec2& p) const {
    return {std::min(std::max(p.x, x), right()), std::min(std::max(p.y, y), bottom())};
}

Vec2 MonitorMap::toLogical(const Vec2& p_mm) const {
    return {logical.x + (p_mm.x - mm.x) * pxPerMMx(), logical.y + (p_mm.y - mm.y) * pxPerMMy()};
}

Vec2 MonitorMap::toMM(const Vec2& p_logical) const {
    return {mm.x + (p_logical.x - logical.x) / pxPerMMx(), mm.y + (p_logical.y - logical.y) / pxPerMMy()};
}

void Layout::setMonitors(std::vector<MonitorMap> monitors) {
    m_monitors = std::move(monitors);
}

const MonitorMap* Layout::monitorForMM(const Vec2& p_mm) const {
    for (const auto& m : m_monitors) {
        if (m.mm.contains(p_mm))
            return &m;
    }
    return nullptr;
}

const MonitorMap* Layout::monitorForLogical(const Vec2& p_logical) const {
    for (const auto& m : m_monitors) {
        if (m.logical.contains(p_logical))
            return &m;
    }
    return nullptr;
}

Vec2 Layout::clampMM(const Vec2& p_mm) const {
    if (m_monitors.empty())
        return p_mm;

    if (monitorForMM(p_mm))
        return p_mm;

    Vec2   best  = m_monitors.front().mm.clamp(p_mm);
    double bestD = dist2(best, p_mm);

    for (std::size_t i = 1; i < m_monitors.size(); ++i) {
        const Vec2   cand = m_monitors[i].mm.clamp(p_mm);
        const double d    = dist2(cand, p_mm);
        if (d < bestD) {
            bestD = d;
            best  = cand;
        }
    }
    return best;
}

std::optional<Vec2> Layout::toLogical(const Vec2& p_mm) const {
    if (m_monitors.empty())
        return std::nullopt;

    const Vec2 clamped = clampMM(p_mm);

    if (const auto* m = monitorForMM(clamped))
        return m->toLogical(clamped);

    // Landed exactly on an outer boundary that half-open containment excludes.
    // Fall back to whichever monitor's mm rect the point sits on the edge of.
    const MonitorMap* nearest  = &m_monitors.front();
    double            nearestD = dist2(nearest->mm.clamp(clamped), clamped);
    for (std::size_t i = 1; i < m_monitors.size(); ++i) {
        const double d = dist2(m_monitors[i].mm.clamp(clamped), clamped);
        if (d < nearestD) {
            nearestD = d;
            nearest  = &m_monitors[i];
        }
    }
    return nearest->toLogical(clamped);
}

std::optional<Vec2> Layout::toMM(const Vec2& p_logical) const {
    if (const auto* m = monitorForLogical(p_logical))
        return m->toMM(p_logical);
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> Layout::firstMMOverlap() const {
    // Strict inequality on both axes: edge-adjacent panels share a boundary and
    // that is the normal case, not an overlap. This matches Rect::contains,
    // which is half-open on the far edges precisely so a shared edge has exactly
    // one owner.
    for (std::size_t i = 0; i < m_monitors.size(); ++i) {
        for (std::size_t j = i + 1; j < m_monitors.size(); ++j) {
            const Rect& a = m_monitors[i].mm;
            const Rect& b = m_monitors[j].mm;

            const bool overlapX = a.x < b.right() && b.x < a.right();
            const bool overlapY = a.y < b.bottom() && b.y < a.bottom();

            if (overlapX && overlapY)
                return std::pair{m_monitors[i].name, m_monitors[j].name};
        }
    }
    return std::nullopt;
}

} // namespace pcs
