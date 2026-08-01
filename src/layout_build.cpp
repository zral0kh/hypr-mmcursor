#include "layout_build.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>

namespace pcs {

Vec2 physicalSizeMM(const MonitorDesc& d) {
    const double nativeW = d.overrideMMWidth.value_or(d.edidMMWidth);
    const double nativeH = d.overrideMMHeight.value_or(d.edidMMHeight);

    if (transformSwapsAxes(d.transform))
        return {nativeH, nativeW};
    return {nativeW, nativeH};
}

namespace {

// Placing a monitor above another is the same computation as placing one beside
// it with the axes swapped. Writing it once and indexing by axis is what stops
// the vertical case from becoming a second, subtly different implementation of
// the horizontal one — which is exactly how the old row builder came to handle
// only one of them.
constexpr int AXIS_X = 0;
constexpr int AXIS_Y = 1;

int    other(int axis) { return axis == AXIS_X ? AXIS_Y : AXIS_X; }
double comp(const Vec2& v, int axis) { return axis == AXIS_X ? v.x : v.y; }

void setComp(Vec2& v, int axis, double value) {
    if (axis == AXIS_X)
        v.x = value;
    else
        v.y = value;
}

double nearEdge(const Rect& r, int axis) { return axis == AXIS_X ? r.x : r.y; }
double extent(const Rect& r, int axis) { return axis == AXIS_X ? r.w : r.h; }
double farEdge(const Rect& r, int axis) { return nearEdge(r, axis) + extent(r, axis); }
double midpoint(const Rect& r, int axis) { return nearEdge(r, axis) + extent(r, axis) / 2.0; }

struct Node {
    const MonitorDesc* desc = nullptr;
    Vec2               size{};   // physical extent on the desk, mm
    Vec2               origin{}; // mm origin, once placed
    bool               placed = false;
};

double density(const Node& n, int axis) {
    const double mm = comp(n.size, axis);
    // plugin.cpp refuses a monitor with no physical size long before this, so
    // the guard is only here to stop a hand-built descriptor in a test from
    // producing infinities that then propagate into every other origin.
    return mm > 0.0 ? extent(n.desc->logical, axis) / mm : 1.0;
}

// Which of the three relations the logical layout states across a seam.
enum class Rel {
    Center,
    Near,
    Far,
};

struct CrossChoice {
    Rel    rel      = Rel::Center;
    double residual = 0.0; // logical px left over after the relation is applied
};

// The heart of the derivation. See the header for why this reads a relation
// rather than converting a coordinate.
CrossChoice chooseCross(const Node& parent, const Node& child, int cross, Align forced) {
    // A forced alignment means "ignore what the logical layout says", so the
    // residual is dropped along with the relation. That is what makes
    // align = center reproduce the old row builder exactly.
    switch (forced) {
        case Align::Start: return {Rel::Near, 0.0};
        case Align::Center: return {Rel::Center, 0.0};
        case Align::End: return {Rel::Far, 0.0};
        case Align::Derive: break;
    }

    const Rect& p = parent.desc->logical;
    const Rect& c = child.desc->logical;

    const double dNear   = nearEdge(c, cross) - nearEdge(p, cross);
    const double dFar    = farEdge(c, cross) - farEdge(p, cross);
    const double dCenter = midpoint(c, cross) - midpoint(p, cross);

    // Nearest relation wins. Ties resolve centre -> near -> far so the result
    // never depends on the order monitors arrived in.
    const double aNear   = std::fabs(dNear);
    const double aFar    = std::fabs(dFar);
    const double aCenter = std::fabs(dCenter);

    if (aCenter <= aNear && aCenter <= aFar)
        return {Rel::Center, dCenter};
    if (aNear <= aFar)
        return {Rel::Near, dNear};
    return {Rel::Far, dFar};
}

const char* edgeName(int mainAxis, bool childIsFar) {
    if (mainAxis == AXIS_X)
        return childIsFar ? "right-of" : "left-of";
    return childIsFar ? "below" : "above";
}

const char* relName(Rel rel, int cross) {
    switch (rel) {
        case Rel::Center: return "centre";
        case Rel::Near: return cross == AXIS_Y ? "top" : "left";
        case Rel::Far: return cross == AXIS_Y ? "bottom" : "right";
    }
    return "?";
}

// Place `child` against `parent`, adjacent along `main`.
//
// `deriveMainGap` distinguishes the two callers. A derived adjacency converts
// whatever logical gap the arrangement has; an explicitly stated relation does
// not, because deriving a gap from a logical layout that may contradict the
// stated relation is meaningless. Explicit means flush plus the seam bezel.
void placeAgainst(Node& child, const Node& parent, int main, bool childIsFar, double seamGapMM, Align forcedAlign, bool deriveMainGap, bool deriveResidual,
                  double extraCrossMM, BuildDiagnostics::Placement* note) {
    const int cross = other(main);

    // ---- main axis: gap ----
    double logicalGap = 0.0;
    if (deriveMainGap) {
        logicalGap = childIsFar ? nearEdge(child.desc->logical, main) - farEdge(parent.desc->logical, main) :
                                  nearEdge(parent.desc->logical, main) - farEdge(child.desc->logical, main);
        logicalGap = std::max(logicalGap, 0.0);
    }
    const double gapMM = seamGapMM + logicalGap / density(parent, main);

    const double parentMain = comp(parent.origin, main);
    const double childMain  = childIsFar ? parentMain + comp(parent.size, main) + gapMM : parentMain - gapMM - comp(child.size, main);

    // ---- cross axis: alignment ----
    //
    // The residual only means something when the logical layout actually states
    // the relation we are reproducing. If the user says "below" about two
    // panels Hyprland has side by side, the cross-axis leftover is the entire
    // width of a monitor and carrying it would fling the child across the desk.
    // Snap to the nearest relation and drop it.
    const CrossChoice CHOICE     = chooseCross(parent, child, cross, forcedAlign);
    const double      residualMM = deriveResidual ? CHOICE.residual / density(parent, cross) : 0.0;

    const double parentCross = comp(parent.origin, cross);
    double       childCross  = 0.0;
    switch (CHOICE.rel) {
        case Rel::Near: childCross = parentCross; break;
        case Rel::Far: childCross = parentCross + comp(parent.size, cross) - comp(child.size, cross); break;
        case Rel::Center: childCross = parentCross + (comp(parent.size, cross) - comp(child.size, cross)) / 2.0; break;
    }
    childCross += residualMM + extraCrossMM;

    setComp(child.origin, main, childMain);
    setComp(child.origin, cross, childCross);
    child.placed = true;

    if (note) {
        note->anchor = parent.desc->name;
        note->how    = std::format("{} {}", edgeName(main, childIsFar), relName(CHOICE.rel, cross));
        setComp(note->residualMM, cross, residualMM);
    }
}

// Are these two adjacent in the logical layout, and along which axis?
//
// Adjacent means: their ranges overlap on one axis and do not overlap on the
// other. Both cannot hold at once, so at most one axis ever matches. Diagonal
// neighbours share no edge and are deliberately not adjacent.
struct Adjacency {
    bool   valid      = false;
    int    mainAxis   = AXIS_X;
    bool   childIsFar = false;
    double gap        = 0.0;
    double overlap    = 0.0;
};

Adjacency adjacencyOf(const Node& parent, const Node& child) {
    const Rect& p = parent.desc->logical;
    const Rect& c = child.desc->logical;

    for (const int axis : {AXIS_X, AXIS_Y}) {
        const int    cross   = other(axis);
        const double overlap = std::min(farEdge(p, cross), farEdge(c, cross)) - std::max(nearEdge(p, cross), nearEdge(c, cross));
        if (overlap <= 0.0)
            continue;

        if (nearEdge(c, axis) >= farEdge(p, axis))
            return {true, axis, true, nearEdge(c, axis) - farEdge(p, axis), overlap};
        if (nearEdge(p, axis) >= farEdge(c, axis))
            return {true, axis, false, nearEdge(p, axis) - farEdge(c, axis), overlap};
    }
    return {};
}

// Same rule as Layout::firstMMOverlap: strict on both axes, because
// edge-adjacent panels share a boundary and that is the normal case.
bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

double seamGapFor(const BuildOptions& opts, const std::string& a, const std::string& b) {
    for (const auto& g : opts.seamGaps) {
        if ((g.a == a && g.b == b) || (g.a == b && g.b == a))
            return g.mm;
    }
    return opts.gapMM;
}

std::size_t findByName(const std::vector<Node>& nodes, const std::string& name) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].desc->name == name)
            return i;
    }
    return nodes.size();
}

void axisOfEdge(Edge e, int& mainAxis, bool& childIsFar) {
    switch (e) {
        case Edge::RightOf: mainAxis = AXIS_X, childIsFar = true; return;
        case Edge::LeftOf: mainAxis = AXIS_X, childIsFar = false; return;
        case Edge::Below: mainAxis = AXIS_Y, childIsFar = true; return;
        case Edge::Above: mainAxis = AXIS_Y, childIsFar = false; return;
    }
}

} // namespace

Layout buildLayout(std::vector<MonitorDesc> descs, const BuildOptions& opts, BuildDiagnostics* diag) {
    std::vector<Node> nodes;
    nodes.reserve(descs.size());
    for (const auto& d : descs)
        nodes.push_back(Node{&d, physicalSizeMM(d), {}, false});

    if (nodes.empty())
        return {};

    // Every scan below walks this order rather than the order monitors arrived
    // in, so an identical set of monitors always produces an identical layout.
    std::vector<std::size_t> byName(nodes.size());
    std::iota(byName.begin(), byName.end(), std::size_t{0});
    std::sort(byName.begin(), byName.end(), [&](std::size_t a, std::size_t b) { return nodes[a].desc->name < nodes[b].desc->name; });

    std::vector<std::size_t> placedOrder;
    placedOrder.reserve(nodes.size());

    const auto note = [&](std::size_t i) -> BuildDiagnostics::Placement* {
        if (!diag)
            return nullptr;
        diag->placements.push_back(BuildDiagnostics::Placement{nodes[i].desc->name, "", "", {}});
        return &diag->placements.back();
    };

    const auto warn = [&](std::string msg) {
        if (diag)
            diag->warnings.push_back(std::move(msg));
    };

    const auto finish = [&](std::size_t i, BuildDiagnostics::Placement* n) {
        nodes[i].origin.x += nodes[i].desc->offsetMM.x;
        nodes[i].origin.y += nodes[i].desc->offsetMM.y;
        placedOrder.push_back(i);
        if (n && (nodes[i].desc->offsetMM.x != 0.0 || nodes[i].desc->offsetMM.y != 0.0))
            n->how += std::format(" offset {:+.1f},{:+.1f}", nodes[i].desc->offsetMM.x, nodes[i].desc->offsetMM.y);
    };

    // ---- roots -------------------------------------------------------------
    //
    // An absolute placement is an anchor: it pins a monitor to the desk and
    // everything reachable grows out from it. With none stated we pick one
    // ourselves — leftmost, then topmost, then by name — which keeps the
    // resulting origin at 0,0 on the same panel the old row builder used.
    for (const std::size_t i : byName) {
        const auto& p = nodes[i].desc->placement;
        if (!p || !p->absoluteMM)
            continue;
        nodes[i].origin = *p->absoluteMM;
        nodes[i].placed = true;
        auto* n         = note(i);
        if (n)
            n->how = std::format("at {:.1f},{:.1f}", p->absoluteMM->x, p->absoluteMM->y);
        finish(i, n);
    }

    if (placedOrder.empty()) {
        // Prefer a monitor that has not been given a relation of its own. The
        // root is placed before anything else, so whatever relation it stated
        // gets ignored — and picking a relation-child as root would silently
        // discard the one instruction the user did give us.
        std::vector<std::size_t> candidates;
        for (const std::size_t i : byName) {
            const auto& p = nodes[i].desc->placement;
            if (!p || !p->edge)
                candidates.push_back(i);
        }
        const bool ALL_ANCHORED = candidates.empty();
        if (ALL_ANCHORED)
            candidates = byName;

        std::size_t root = candidates.front();
        for (const std::size_t i : candidates) {
            const Rect& a = nodes[i].desc->logical;
            const Rect& b = nodes[root].desc->logical;
            if (a.x < b.x || (a.x == b.x && a.y < b.y))
                root = i;
        }

        // Every monitor hangs off another one, so the relations form a cycle
        // and someone has to go first. Say whose instruction was dropped.
        if (ALL_ANCHORED)
            warn(std::format("{}: every monitor is anchored to another, so the relations are circular; using {} as the fixed point and ignoring its own placement",
                             nodes[root].desc->name, nodes[root].desc->name));

        nodes[root].origin = {0.0, 0.0};
        nodes[root].placed = true;
        auto* n            = note(root);
        if (n)
            n->how = "root";
        finish(root, n);
    }

    // ---- grow the tree -----------------------------------------------------
    //
    // Explicitly stated relations go first, because they are the user telling
    // us something the layout cannot. Everything else attaches by its cheapest
    // logical adjacency — flush seams before gapped ones, wider shared edges
    // before narrower.
    bool progress = true;
    while (progress) {
        progress = false;

        for (const std::size_t i : byName) {
            if (nodes[i].placed)
                continue;
            const auto& p = nodes[i].desc->placement;
            if (!p || !p->edge)
                continue;

            const std::size_t anchor = findByName(nodes, p->anchor);
            if (anchor == nodes.size() || !nodes[anchor].placed)
                continue;

            int  mainAxis   = AXIS_X;
            bool childIsFar = true;
            axisOfEdge(*p->edge, mainAxis, childIsFar);

            // Only trust the logical residual if the layout agrees with the
            // relation the user stated.
            const Adjacency ADJ            = adjacencyOf(nodes[anchor], nodes[i]);
            const bool      LAYOUT_AGREES  = ADJ.valid && ADJ.mainAxis == mainAxis;

            auto* n = note(i);
            placeAgainst(nodes[i], nodes[anchor], mainAxis, childIsFar, seamGapFor(opts, nodes[i].desc->name, nodes[anchor].desc->name),
                         p->align == Align::Derive ? opts.align : p->align, /*deriveMainGap=*/false, /*deriveResidual=*/LAYOUT_AGREES, p->offsetMM, n);
            if (n)
                n->how += " (config)";
            finish(i, n);
            progress = true;
        }
        if (progress)
            continue;

        std::size_t bestChild  = nodes.size();
        std::size_t bestParent = nodes.size();
        Adjacency   best;
        for (const std::size_t child : byName) {
            if (nodes[child].placed)
                continue;
            // A monitor with a stated anchor waits for that anchor; attaching
            // it somewhere else would silently ignore the config.
            const auto& p = nodes[child].desc->placement;
            if (p && p->edge)
                continue;

            for (const std::size_t parent : byName) {
                if (!nodes[parent].placed)
                    continue;
                const Adjacency ADJ = adjacencyOf(nodes[parent], nodes[child]);
                if (!ADJ.valid)
                    continue;
                if (bestChild != nodes.size() && !(ADJ.gap < best.gap || (ADJ.gap == best.gap && ADJ.overlap > best.overlap)))
                    continue;
                best       = ADJ;
                bestChild  = child;
                bestParent = parent;
            }
        }

        if (bestChild != nodes.size()) {
            auto* n = note(bestChild);
            placeAgainst(nodes[bestChild], nodes[bestParent], best.mainAxis, best.childIsFar,
                         seamGapFor(opts, nodes[bestChild].desc->name, nodes[bestParent].desc->name), opts.align, /*deriveMainGap=*/true, /*deriveResidual=*/true, 0.0, n);
            finish(bestChild, n);
            progress = true;
        }
    }

    // ---- whatever is left --------------------------------------------------
    //
    // A monitor Hyprland placed touching nothing, or one whose stated anchor
    // does not exist. Neither is recoverable into an exact answer, so convert
    // its logical origin through the root's density and say so out loud. Going
    // inert instead would mean an unplugged-and-replugged monitor could disable
    // the plugin until the config is edited.
    const Node& root = nodes[placedOrder.front()];
    for (const std::size_t i : byName) {
        if (nodes[i].placed)
            continue;

        const auto& p = nodes[i].desc->placement;
        if (p && p->edge)
            warn(std::format("{}: anchor '{}' is not present or not placeable; falling back to its logical position", nodes[i].desc->name, p->anchor));
        else
            warn(std::format("{}: touches no other monitor in the logical layout; falling back to its logical position", nodes[i].desc->name));

        Vec2 origin = {root.origin.x + (nodes[i].desc->logical.x - root.desc->logical.x) / density(root, AXIS_X),
                       root.origin.y + (nodes[i].desc->logical.y - root.desc->logical.y) / density(root, AXIS_Y)};

        // A guess must never manufacture an overlap. The caller refuses a layout
        // whose mm rects collide — reasonably, since which panel owns a point
        // would come down to declaration order — so a fallback that lands on top
        // of another panel would disable the plugin and blame the user's config
        // for a position we invented.
        //
        // The case that reaches here in practice is a compositor mid-configura-
        // tion, where two monitors briefly share a logical rect: the converted
        // offset is then zero and this lands exactly on the root.
        //
        // finish() adds the user's own offset afterwards, so collide-test where
        // the panel will actually end up, not where it is about to be stored.
        const Vec2 OFF = nodes[i].desc->offsetMM;
        const Rect LANDING{origin.x + OFF.x, origin.y + OFF.y, nodes[i].size.x, nodes[i].size.y};

        bool collides = false;
        double clearX = 0.0;
        for (const std::size_t j : placedOrder) {
            const Rect PLACED{nodes[j].origin.x, nodes[j].origin.y, nodes[j].size.x, nodes[j].size.y};
            clearX = std::max(clearX, PLACED.right());
            if (overlaps(LANDING, PLACED))
                collides = true;
        }

        if (collides) {
            // Park it clear to the right of everything placed so far. Nothing
            // can then overlap it in x, and a second fallback stacks beyond
            // this one because clearX is recomputed each time.
            origin.x = clearX + opts.gapMM - OFF.x;
            warn(std::format("{}: its fallback position collided with an already-placed monitor; parked clear at {:.1f}mm", nodes[i].desc->name, clearX + opts.gapMM));
        }

        nodes[i].origin = origin;
        nodes[i].placed = true;
        auto* n         = note(i);
        if (n)
            n->how = collides ? "fallback, parked clear" : "fallback";
        finish(i, n);
    }

    std::vector<MonitorMap> maps;
    maps.reserve(nodes.size());
    for (const std::size_t i : placedOrder)
        maps.push_back(MonitorMap{nodes[i].desc->name, Rect{nodes[i].origin.x, nodes[i].origin.y, nodes[i].size.x, nodes[i].size.y}, nodes[i].desc->logical});

    Layout layout;
    layout.setMonitors(std::move(maps));
    return layout;
}

double defaultMMPerUnit(const Layout& layout) {
    if (layout.empty())
        return 1.0;
    const auto&  m       = layout.monitors().front();
    const double density = (m.pxPerMMx() + m.pxPerMMy()) / 2.0;
    return density > 0.0 ? 1.0 / density : 1.0;
}

} // namespace pcs
