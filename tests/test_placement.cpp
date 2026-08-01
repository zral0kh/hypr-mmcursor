// Placement tests: the desk layout derived from what Hyprland has active, and
// every way a user can override it.
//
//   make test
//
// The old row builder could only express one horizontal row, and it expressed
// everything else — a vertical stack, an L, a deliberate stagger — as that same
// row, silently. This file is the guard against that returning. No Hyprland, no
// compositor, no GPU.

#include "../src/cursor_state.hpp"
#include "../src/geometry.hpp"
#include "../src/layout_build.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace pcs;

static int g_failures = 0;
static int g_checks   = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

static void nearly(double a, double b, const std::string& what, double eps = 1e-9) {
    ++g_checks;
    if (std::fabs(a - b) > eps) {
        ++g_failures;
        std::printf("  FAIL  %s  (%.10f vs %.10f, delta %.3e)\n", what.c_str(), a, b, std::fabs(a - b));
    }
}

static void section(const char* name) {
    std::printf("\n%s\n", name);
}

// A monitor at a logical rect with a physical size in NATIVE orientation.
static MonitorDesc mon(const char* name, Rect logical, double mmW, double mmH, int transform = 0) {
    MonitorDesc d;
    d.name         = name;
    d.logical      = logical;
    d.edidMMWidth  = mmW;
    d.edidMMHeight = mmH;
    d.transform    = transform;
    return d;
}

static const MonitorMap* find(const Layout& l, const std::string& name) {
    for (const auto& m : l.monitors()) {
        if (m.name == name)
            return &m;
    }
    return nullptr;
}

// mm origin of a named monitor, or a sentinel that fails loudly.
static Vec2 originOf(const Layout& l, const std::string& name) {
    const auto* m = find(l, name);
    if (!m) {
        ++g_checks;
        ++g_failures;
        std::printf("  FAIL  no monitor named %s in the layout\n", name.c_str());
        return {-1e18, -1e18};
    }
    return {m->mm.x, m->mm.y};
}

static void originIs(const Layout& l, const std::string& name, double x, double y, const std::string& what) {
    const Vec2 o = originOf(l, name);
    nearly(o.x, x, what + " (mm x)");
    nearly(o.y, y, what + " (mm y)");
}

// Two 1000x1000 logical panels at 500x500mm: an exact 2 px/mm, so every
// expected number below stays legible.
static MonitorDesc square(const char* name, double lx, double ly) {
    return mon(name, Rect{lx, ly, 1000, 1000}, 500, 500);
}

int main() {
    section("a flush row is reproduced flush");
    {
        const Layout l = buildLayout({square("A", 0, 0), square("B", 1000, 0)});
        check(l.monitors().size() == 2, "two monitors");
        check(l.monitors().front().name == "A", "the root is emitted first");
        originIs(l, "A", 0, 0, "root sits at the origin");
        originIs(l, "B", 500, 0, "B is flush against A in mm");
    }

    section("a logical gap converts through the anchor's density");
    {
        // 200 logical px of dead space at A's 2 px/mm is 100mm of desk.
        const Layout l = buildLayout({square("A", 0, 0), mon("B", Rect{1200, 0, 1000, 1000}, 500, 500)});
        originIs(l, "B", 600, 0, "the gap is carried across as 100mm");
    }

    section("a vertical stack stays a vertical stack");
    {
        // The case the row builder could not express at all: same logical x,
        // stacked in y. It used to come out side by side, silently.
        const Layout l = buildLayout({square("A", 0, 0), square("B", 0, 1000)});
        originIs(l, "A", 0, 0, "top panel at the origin");
        originIs(l, "B", 0, 500, "bottom panel is below it, not beside it");
        check(!l.firstMMOverlap().has_value(), "a stack does not overlap");
    }

    section("an L, and a 2x2 grid");
    {
        const Layout l = buildLayout({square("A", 0, 0), square("B", 1000, 0), square("C", 0, 1000)});
        originIs(l, "A", 0, 0, "A");
        originIs(l, "B", 500, 0, "B is right of A");
        originIs(l, "C", 0, 500, "C is below A");

        const Layout g = buildLayout({square("A", 0, 0), square("B", 1000, 0), square("C", 0, 1000), square("D", 1000, 1000)});
        originIs(g, "D", 500, 500, "the diagonal corner tiles consistently from either neighbour");
        check(!g.firstMMOverlap().has_value(), "a 2x2 grid does not overlap");
    }

    section("the relation the layout states is the relation reproduced");
    {
        // A is 500mm tall; B is half the logical height and half the physical.
        const MonitorDesc A = square("A", 0, 0);

        const Layout top = buildLayout({A, mon("B", Rect{1000, 0, 1000, 500}, 500, 250)});
        originIs(top, "B", 500, 0, "logically top-flush -> physically top-flush");

        const Layout bottom = buildLayout({A, mon("B", Rect{1000, 500, 1000, 500}, 500, 250)});
        originIs(bottom, "B", 500, 250, "logically bottom-flush -> physically bottom-flush");

        const Layout centre = buildLayout({A, mon("B", Rect{1000, 250, 1000, 500}, 500, 250)});
        originIs(centre, "B", 500, 125, "logically centred -> physically centred");
    }

    section("a stagger keeps its residual");
    {
        // 100px below top-flush. Nearest relation is top (100) rather than
        // centre (150) or bottom (400), so: top-flush plus 100px/2ppmm = 50mm.
        const Layout l = buildLayout({square("A", 0, 0), mon("B", Rect{1000, 100, 1000, 500}, 500, 250)});
        originIs(l, "B", 500, 50, "the leftover offset converts through the anchor");
    }

    section("forcing an alignment drops the residual");
    {
        const std::vector<MonitorDesc> desk = {square("A", 0, 0), mon("B", Rect{1000, 100, 1000, 500}, 500, 250)};

        BuildOptions o;
        o.align = Align::Center;
        originIs(buildLayout(desk, o), "B", 500, 125, "align=center ignores what the layout says");

        o.align = Align::Start;
        originIs(buildLayout(desk, o), "B", 500, 0, "align=top ignores what the layout says");

        o.align = Align::End;
        originIs(buildLayout(desk, o), "B", 500, 250, "align=bottom ignores what the layout says");
    }

    section("explicit absolute placement anchors the desk");
    {
        MonitorDesc   b = square("B", 1000, 0);
        PlacementSpec p;
        p.absoluteMM = Vec2{900, 40};
        b.placement  = p;

        const Layout l = buildLayout({square("A", 0, 0), b});
        originIs(l, "B", 900, 40, "B is exactly where it was told to be");
        check(l.monitors().front().name == "B", "an absolutely placed monitor becomes the root");
    }

    section("explicit relations override the layout");
    {
        // Hyprland has them side by side; the user says one is below the other.
        MonitorDesc   b = square("B", 1000, 0);
        PlacementSpec p;
        p.edge      = Edge::Below;
        p.anchor    = "A";
        b.placement = p;

        const Layout l = buildLayout({square("A", 0, 0), b});
        originIs(l, "B", 0, 500, "B goes below A, and the contradicted residual is dropped");

        // An explicit alignment and offset on the same relation.
        PlacementSpec q;
        q.edge      = Edge::Below;
        q.anchor    = "A";
        q.align     = Align::End;
        q.offsetMM  = 7.0;
        b.placement = q;
        // Cross axis of `below` is x; End aligns right edges (both 500mm wide,
        // so x = 0) and the offset then nudges along that same axis.
        originIs(buildLayout({square("A", 0, 0), b}), "B", 7, 500, "align and offset are honoured on the cross axis");
    }

    section("per-seam gaps override the global one");
    {
        BuildOptions o;
        o.gapMM    = 5.0;
        o.seamGaps = {SeamGap{"B", "A", 22.0}}; // named in the other order on purpose

        const Layout l = buildLayout({square("A", 0, 0), square("B", 1000, 0), square("C", 0, 1000)}, o);
        originIs(l, "B", 522, 0, "the A<->B seam uses its own bezel");
        originIs(l, "C", 0, 505, "every other seam still uses the global gap");
    }

    section("offsets move the panel, and anything anchored to it");
    {
        std::vector<MonitorDesc> desk = {square("A", 0, 0), square("B", 1000, 0)};
        desk[0].offsetMM             = Vec2{10, 20};

        const Layout l = buildLayout(desk);
        originIs(l, "A", 10, 20, "the offset moves A");
        originIs(l, "B", 510, 20, "B hangs off A and moves with it");

        // The legacy 4th field of mmcursor-monitor lands in the same place.
        std::vector<MonitorDesc> legacy = {square("A", 0, 0), square("B", 1000, 0)};
        legacy[1].offsetMM.y            = -12.0;
        originIs(buildLayout(legacy), "B", 500, -12, "a vertical-only offset still works");
    }

    section("declaration order never changes the result");
    {
        std::vector<MonitorDesc> desk = {square("A", 0, 0), square("B", 1000, 0), square("C", 0, 1000), square("D", 1000, 1000)};
        const Layout             ref  = buildLayout(desk);

        std::vector<int> idx = {0, 1, 2, 3};
        std::sort(idx.begin(), idx.end());
        int permutations = 0;
        do {
            std::vector<MonitorDesc> shuffled;
            for (const int i : idx)
                shuffled.push_back(desk[static_cast<std::size_t>(i)]);

            const Layout l = buildLayout(shuffled);
            ++permutations;
            for (std::size_t i = 0; i < ref.monitors().size(); ++i) {
                check(l.monitors()[i].name == ref.monitors()[i].name, "emission order is identical");
                nearly(l.monitors()[i].mm.x, ref.monitors()[i].mm.x, "mm x is identical");
                nearly(l.monitors()[i].mm.y, ref.monitors()[i].mm.y, "mm y is identical");
            }
        } while (std::next_permutation(idx.begin(), idx.end()));
        check(permutations == 24, "all 24 orderings tried");
    }

    section("the root is leftmost, then topmost, then by name");
    {
        // Declared out of order, and DP-10 is the topmost — the leftmost still
        // wins, which is what keeps pointer feel on the same panel as before.
        const Layout l = buildLayout({mon("DP-10", Rect{2560, -240, 1080, 1920}, 530, 300, 3), mon("DP-9", Rect{0, 0, 2560, 1440}, 600, 340)});
        check(l.monitors().front().name == "DP-9", "the leftmost panel is the root");
        originIs(l, "DP-9", 0, 0, "and it sits at the origin");
    }

    section("things that cannot be derived say so");
    {
        // An island Hyprland placed touching nothing.
        BuildDiagnostics d;
        const Layout     l = buildLayout({square("A", 0, 0), square("B", 5000, 5000)}, {}, &d);
        check(d.warnings.size() == 1, "the island is reported");
        originIs(l, "B", 2500, 2500, "and placed by converting through the root");

        // A relation naming a monitor that is not here.
        MonitorDesc   b = square("B", 1000, 0);
        PlacementSpec p;
        p.edge      = Edge::RightOf;
        p.anchor    = "NOPE";
        b.placement = p;

        BuildDiagnostics d2;
        buildLayout({square("A", 0, 0), b}, {}, &d2);
        check(d2.warnings.size() == 1, "an unresolvable anchor is reported");
        check(d2.warnings[0].find("NOPE") != std::string::npos, "and the warning names it");

        // Two monitors anchored to each other: someone has to be the fixed
        // point, so one relation is necessarily dropped. It still resolves —
        // but silently dropping a user's instruction is not acceptable.
        MonitorDesc   x = square("X", 0, 0);
        MonitorDesc   y = square("Y", 1000, 0);
        PlacementSpec px, py;
        px.edge     = Edge::RightOf;
        px.anchor   = "Y";
        py.edge     = Edge::RightOf;
        py.anchor   = "X";
        x.placement = px;
        y.placement = py;

        BuildDiagnostics d3;
        const Layout     cyc = buildLayout({x, y}, {}, &d3);
        check(cyc.monitors().size() == 2, "a cycle still produces a layout");
        check(cyc.monitors().front().name == "X", "the leftmost becomes the fixed point");
        check(!d3.warnings.empty(), "and the dropped relation is reported");

        // The innocent version: one monitor anchored, one not. The unanchored
        // one must become the root even though it is not leftmost, or the only
        // instruction given would be the one thrown away.
        MonitorDesc   left = square("LEFT", 0, 0);
        PlacementSpec pl;
        pl.edge        = Edge::LeftOf;
        pl.anchor      = "RIGHT";
        left.placement = pl;

        BuildDiagnostics d4;
        const Layout     l4 = buildLayout({left, square("RIGHT", 1000, 0)}, {}, &d4);
        check(l4.monitors().front().name == "RIGHT", "the unanchored monitor is the root");
        originIs(l4, "LEFT", -500, 0, "and the stated relation is honoured");
        check(d4.warnings.empty(), "with nothing to complain about");
    }

    section("diagnostics describe how each monitor got there");
    {
        BuildDiagnostics d;
        buildLayout({mon("DP-9", Rect{0, 0, 2560, 1440}, 600, 340), mon("DP-10", Rect{2560, -240, 1080, 1920}, 530, 300, 3)}, {}, &d);
        check(d.placements.size() == 2, "one note per monitor");
        check(d.placements[0].how == "root", "the root says so");
        check(d.placements[1].anchor == "DP-9", "the child names its anchor");
        check(d.placements[1].how.find("right-of") != std::string::npos, "and the relation");
        check(d.placements[1].how.find("centre") != std::string::npos, "and the alignment");
        check(d.warnings.empty(), "a derivable desk produces no warnings");
    }

    section("scale is invisible to the core");
    {
        // Hyprland hands us m_size, which is already divided by the scale. A
        // scale-2 panel is therefore just a panel with half the logical size
        // and the same physical size — i.e. a different density, which is the
        // thing this plugin already handles.
        const Layout l = buildLayout({mon("A", Rect{0, 0, 1280, 720}, 600, 340), mon("B", Rect{1280, 0, 2560, 1440}, 600, 340)});

        const auto* a = find(l, "A");
        const auto* b = find(l, "B");
        check(a && b, "both scaled panels are present");
        if (a && b) {
            nearly(a->pxPerMMx(), 1280.0 / 600.0, "the scale-2 panel is half as dense");
            nearly(b->pxPerMMx(), 2560.0 / 600.0, "the scale-1 panel is not");
            nearly(b->mm.x, 600.0, "they are still flush on the desk");
            nearly(a->mm.y, b->mm.y, "and share a horizon, being the same physical height");
        }

        // Equal physical travel must cross both at the same mm rate, which is
        // the entire point and is exactly what a scale change threatens.
        CursorState st;
        st.setLayout(&l);
        st.setMMPerUnit(1.0);
        st.reconcile(Vec2{100, 360});
        const double startMM = st.positionMM().x;
        for (int i = 0; i < 800; ++i)
            st.applyRelative(Vec2{1.0, 0.0});
        nearly(st.positionMM().x, startMM + 800.0, "800mm of travel is 800mm of desk, across a scale boundary", 1e-9);

        // Fractional scale rounds the logical size, so the derived density is
        // off by under half a pixel across the whole panel. Pin that it stays
        // that small rather than pretending it is zero.
        const Layout frac = buildLayout({mon("A", Rect{0, 0, 2646, 1440}, 600, 340)});
        const auto*  f    = find(frac, "A");
        check(f != nullptr, "a fractionally-scaled panel builds");
        if (f)
            check(std::fabs(f->pxPerMMx() - 3440.0 / 1.3 / 600.0) < 0.5 / 600.0, "rounding error stays under half a pixel of panel width");
    }

    section("continuity across a vertical seam");
    {
        // The horizontal case has been covered since the beginning; a stack has
        // exactly the same guarantee to make, and until now no way to make it.
        const Layout l = buildLayout({mon("TOP", Rect{0, 0, 2560, 1440}, 600, 340), mon("BOT", Rect{0, 1440, 1920, 1080}, 520, 290)});

        const auto* top = find(l, "TOP");
        const auto* bot = find(l, "BOT");
        check(top && bot, "both panels present");
        if (top && bot) {
            nearly(bot->mm.y, 340.0, "the lower panel starts where the upper one ends");
            check(std::fabs(top->pxPerMMy() - bot->pxPerMMy()) > 0.4, "the densities genuinely differ, so the seam is a real test");
        }

        CursorState st;
        st.setLayout(&l);
        st.setMMPerUnit(1.0);
        st.reconcile(Vec2{300, 700});

        const double startX = st.positionMM().x;
        for (int i = 0; i < 400; ++i)
            st.applyRelative(Vec2{0.0, 1.0});
        check(st.positionMM().y > 340.0, "crossed onto the lower panel");
        nearly(st.positionMM().x, startX, "crossing a vertical seam does not drift sideways", 1e-9);

        for (int i = 0; i < 400; ++i)
            st.applyRelative(Vec2{0.0, -1.0});
        nearly(st.positionMM().y, 700.0 / (1440.0 / 340.0), "and coming back lands exactly where it started", 1e-9);
    }

    section("flush in logical, flush in mm");
    {
        // The property that makes the whole thing hold together: wherever
        // Hyprland has two panels touching, the desk has them touching too, so
        // there is no dead space for the cursor to fall into.
        const std::vector<std::vector<MonitorDesc>> desks = {
            {square("A", 0, 0), square("B", 1000, 0)},
            {square("A", 0, 0), square("B", 0, 1000)},
            {square("A", 0, 0), square("B", 1000, 0), square("C", 0, 1000), square("D", 1000, 1000)},
            {mon("A", Rect{0, 0, 2560, 1440}, 600, 340), mon("B", Rect{2560, -240, 1080, 1920}, 530, 300, 3)},
            {mon("A", Rect{0, 0, 1280, 720}, 600, 340), mon("B", Rect{1280, 0, 2560, 1440}, 600, 340)},
        };

        for (const auto& desk : desks) {
            const Layout l = buildLayout(desk);
            check(!l.firstMMOverlap().has_value(), "no overlap");

            for (std::size_t i = 0; i < desk.size(); ++i) {
                for (std::size_t j = i + 1; j < desk.size(); ++j) {
                    const Rect& li = desk[i].logical;
                    const Rect& lj = desk[j].logical;

                    const bool flushX = li.right() == lj.x || lj.right() == li.x;
                    const bool flushY = li.bottom() == lj.y || lj.bottom() == li.y;
                    if (!flushX && !flushY)
                        continue;

                    const auto* mi = find(l, desk[i].name);
                    const auto* mj = find(l, desk[j].name);
                    if (!mi || !mj)
                        continue;

                    if (flushX)
                        check(std::fabs(mi->mm.right() - mj->mm.x) < 1e-9 || std::fabs(mj->mm.right() - mi->mm.x) < 1e-9, "logically flush in x -> flush in mm");
                    if (flushY)
                        check(std::fabs(mi->mm.bottom() - mj->mm.y) < 1e-9 || std::fabs(mj->mm.bottom() - mi->mm.y) < 1e-9, "logically flush in y -> flush in mm");
                }
            }
        }
    }

    section("degenerate inputs");
    {
        check(buildLayout({}).empty(), "no monitors is an empty layout");

        const Layout one = buildLayout({square("ONLY", 400, 400)});
        check(one.monitors().size() == 1, "a single monitor builds");
        originIs(one, "ONLY", 0, 0, "and is its own root at the origin");

        // A panel with no physical size cannot be projected through; plugin.cpp
        // refuses it long before here, but the builder must not produce
        // infinities if one ever arrives.
        const Layout zero = buildLayout({mon("A", Rect{0, 0, 1000, 1000}, 0, 0), square("B", 1000, 0)});
        for (const auto& m : zero.monitors()) {
            check(std::isfinite(m.mm.x) && std::isfinite(m.mm.y), "origins stay finite");
            check(std::isfinite(m.mm.w) && std::isfinite(m.mm.h), "sizes stay finite");
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
