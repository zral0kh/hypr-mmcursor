#include "layout_build.hpp"

#include <algorithm>

namespace pcs {

Vec2 physicalSizeMM(const MonitorDesc& d) {
    const double nativeW = d.overrideMMWidth.value_or(d.edidMMWidth);
    const double nativeH = d.overrideMMHeight.value_or(d.edidMMHeight);

    if (transformSwapsAxes(d.transform))
        return {nativeH, nativeW};
    return {nativeW, nativeH};
}

Layout buildLayout(std::vector<MonitorDesc> descs, const BuildOptions& opts) {
    std::vector<MonitorMap> maps;
    maps.reserve(descs.size());

    std::vector<MonitorDesc*> row;
    std::vector<MonitorDesc*> fixed;

    for (auto& d : descs) {
        if (d.explicitMMOrigin)
            fixed.push_back(&d);
        else
            row.push_back(&d);
    }

    // Preserve the arrangement the user already has in Hyprland.
    std::sort(row.begin(), row.end(), [](const MonitorDesc* a, const MonitorDesc* b) { return a->logical.x < b->logical.x; });

    // Vertical reference: the physical span of the first monitor in the row.
    double refTop    = 0.0;
    double refHeight = 0.0;
    if (!row.empty())
        refHeight = physicalSizeMM(*row.front()).y;

    double cursorX = 0.0;
    bool   first   = true;

    for (MonitorDesc* d : row) {
        const Vec2 size = physicalSizeMM(*d);

        if (!first)
            cursorX += opts.gapMM;

        double y = 0.0;
        switch (opts.align) {
            case VAlign::Top: y = refTop; break;
            case VAlign::Bottom: y = refTop + refHeight - size.y; break;
            case VAlign::Center: y = refTop + (refHeight - size.y) / 2.0; break;
        }
        y += d->offsetMM;

        maps.push_back(MonitorMap{d->name, Rect{cursorX, y, size.x, size.y}, d->logical});

        cursorX += size.x;
        first = false;
    }

    for (MonitorDesc* d : fixed) {
        const Vec2 size = physicalSizeMM(*d);
        maps.push_back(MonitorMap{d->name, Rect{d->explicitMMOrigin->x, d->explicitMMOrigin->y + d->offsetMM, size.x, size.y}, d->logical});
    }

    Layout layout;
    layout.setMonitors(std::move(maps));
    return layout;
}

double defaultMMPerUnit(const Layout& layout) {
    if (layout.empty())
        return 1.0;
    const auto& m = layout.monitors().front();
    const double density = (m.pxPerMMx() + m.pxPerMMy()) / 2.0;
    return density > 0.0 ? 1.0 / density : 1.0;
}

} // namespace pcs
