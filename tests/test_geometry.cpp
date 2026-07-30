// Standalone tests. No Hyprland, no compositor, no GPU.
//
//   make test
//
// Everything that can be logically wrong about this plugin is reachable from
// here. Do not go looking for coordinate bugs inside a running compositor when
// they can be found in a 20ms unit test.

#include "../src/cursor_state.hpp"
#include "../src/geometry.hpp"
#include "../src/layout_build.hpp"

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

// ---------------------------------------------------------------------------
// The actual desk this was written for.
//
//   DP-9  Lenovo P27h-30, 2560x1440, EDID 600x340mm, scale 1, transform 0
//   DP-10 Acer CB242Y,    1920x1080, EDID 530x300mm, scale 1, transform 3
//
// DP-10 is rotated into portrait and sits to the right, physically centred on
// the same horizon as DP-9.
// ---------------------------------------------------------------------------
static std::vector<MonitorDesc> realDesk() {
    MonitorDesc dp9;
    dp9.name         = "DP-9";
    dp9.logical      = Rect{0, 0, 2560, 1440};
    dp9.edidMMWidth  = 600;
    dp9.edidMMHeight = 340;
    dp9.transform    = 0;

    MonitorDesc dp10;
    dp10.name         = "DP-10";
    dp10.logical      = Rect{2560, -240, 1080, 1920}; // auto-center-right
    dp10.edidMMWidth  = 530;
    dp10.edidMMHeight = 300;
    dp10.transform    = 3;

    return {dp9, dp10};
}

int main() {
    section("transform handling");
    {
        check(!transformSwapsAxes(0), "transform 0 does not swap");
        check(transformSwapsAxes(1), "transform 1 swaps");
        check(!transformSwapsAxes(2), "transform 2 does not swap");
        check(transformSwapsAxes(3), "transform 3 swaps");
        check(transformSwapsAxes(7), "transform 7 (flipped 270) swaps");

        MonitorDesc d;
        d.edidMMWidth  = 530;
        d.edidMMHeight = 300;
        d.transform    = 3;
        const Vec2 s   = physicalSizeMM(d);
        nearly(s.x, 300, "rotated panel is 300mm wide on the desk");
        nearly(s.y, 530, "rotated panel is 530mm tall on the desk");
    }

    section("layout construction");
    Layout layout = buildLayout(realDesk());
    {
        check(layout.monitors().size() == 2, "two monitors in layout");
        const auto& a = layout.monitors()[0];
        const auto& b = layout.monitors()[1];

        nearly(a.mm.x, 0, "DP-9 mm origin x");
        nearly(a.mm.y, 0, "DP-9 mm origin y");
        nearly(a.mm.w, 600, "DP-9 mm width");
        nearly(a.mm.h, 340, "DP-9 mm height");

        nearly(b.mm.x, 600, "DP-10 sits immediately right of DP-9");
        nearly(b.mm.w, 300, "DP-10 mm width (rotated)");
        nearly(b.mm.h, 530, "DP-10 mm height (rotated)");
        nearly(b.mm.y, -95, "DP-10 centred on DP-9's horizon: (340-530)/2");

        nearly(a.mm.center().y, b.mm.center().y, "physical centres share a horizon");

        // The densities that cause the whole problem.
        nearly(a.pxPerMMy(), 1440.0 / 340.0, "DP-9 vertical density");
        nearly(b.pxPerMMy(), 1920.0 / 530.0, "DP-10 vertical density");
        check(std::fabs(a.pxPerMMy() - b.pxPerMMy()) > 0.5, "densities genuinely differ");
    }

    section("projection round-trips");
    {
        const std::vector<Vec2> probes = {
            {1.0, 1.0}, {300, 170}, {599, 339}, {601, -94}, {750, 0}, {750, 400}, {899, 434},
        };
        for (const auto& p : probes) {
            const auto log = layout.toLogical(p);
            check(log.has_value(), "probe projects to logical");
            if (!log)
                continue;
            const auto back = layout.toMM(*log);
            check(back.has_value(), "logical projects back to mm");
            if (!back)
                continue;
            nearly(back->x, p.x, "round-trip preserves mm x", 1e-9);
            nearly(back->y, p.y, "round-trip preserves mm y", 1e-9);
        }
    }

    section("clamp is idempotent");
    {
        const std::vector<Vec2> outside = {
            {-500, -500}, {2000, 2000}, {650, -400}, {650, 600}, {450, -200},
        };
        for (const auto& p : outside) {
            const Vec2 once  = layout.clampMM(p);
            const Vec2 twice = layout.clampMM(once);
            nearly(twice.x, once.x, "clamp idempotent in x");
            nearly(twice.y, once.y, "clamp idempotent in y");
        }
    }

    section("the bug this exists to fix");
    {
        // Cursor at the very top edge of DP-9, then crossing to DP-10.
        const Vec2 topOfDP9MM{599.0, 0.0};
        const auto logBefore = layout.toLogical(topOfDP9MM);
        nearly(logBefore->y, 0.0, "top edge of DP-9 is logical y=0");

        // Cross the seam. mm y is untouched, by construction.
        const Vec2 justAcrossMM{601.0, 0.0};
        const auto logAfter = layout.toLogical(justAcrossMM);

        // Physically correct landing spot on DP-10.
        const double expected = -240.0 + (0.0 - (-95.0)) * (1920.0 / 530.0);
        nearly(logAfter->y, expected, "crossing lands at the physically correct logical y");
        nearly(logAfter->y, 104.1509, "…which is ~104px down DP-10, not 0", 1e-3);

        // What stock Hyprland does: carry logical y across unchanged.
        const auto naiveMM = layout.toMM(Vec2{2561.0, 0.0});
        check(naiveMM.has_value(), "naive crossing point is on DP-10");
        const double physicalError = std::fabs(naiveMM->y - topOfDP9MM.y);
        nearly(physicalError, 28.75, "stock behaviour is ~28.75mm off at the top edge", 1e-2);
        check(physicalError > 25.0, "the jump is large enough to be worth fixing");
    }

    section("no hysteresis under motion");
    {
        CursorState st;
        st.setLayout(&layout);
        st.setMMPerUnit(1.0); // deltas are mm, keeps the arithmetic legible

        st.reconcile(Vec2{1280, 720}); // centre of DP-9
        const Vec2 start = st.positionMM();
        nearly(start.x, 300.0, "seeded at DP-9 centre x");
        nearly(start.y, 170.0, "seeded at DP-9 centre y");

        // Out into DP-10 and back, in small steps, never touching an edge.
        for (int i = 0; i < 400; ++i)
            st.applyRelative(Vec2{1.0, 0.0});
        check(st.positionMM().x > 600.0, "ended up on DP-10");

        for (int i = 0; i < 400; ++i)
            st.applyRelative(Vec2{-1.0, 0.0});

        nearly(st.positionMM().x, start.x, "returned to exactly the same mm x", 1e-9);
        nearly(st.positionMM().y, start.y, "returned to exactly the same mm y", 1e-9);
    }

    section("clamping does not leak position");
    {
        CursorState st;
        st.setLayout(&layout);
        st.setMMPerUnit(1.0);
        st.reconcile(Vec2{1280, 720});

        // Slam into the left edge repeatedly, then walk back a known distance.
        for (int i = 0; i < 1000; ++i)
            st.applyRelative(Vec2{-1.0, 0.0});
        nearly(st.positionMM().x, 0.0, "pinned to the left edge");

        for (int i = 0; i < 100; ++i)
            st.applyRelative(Vec2{1.0, 0.0});
        nearly(st.positionMM().x, 100.0, "walks back exactly 100mm, no accumulated overshoot");
    }

    section("reconcile after an absolute move");
    {
        CursorState st;
        st.setLayout(&layout);
        st.setMMPerUnit(1.0);
        st.reconcile(Vec2{1280, 720});

        // Something else warps the cursor onto DP-10 (a dispatcher, a game
        // releasing pointer lock, a tablet). We must adopt it, not fight it.
        const bool ok = st.reconcile(Vec2{3100, 960});
        check(ok, "reconcile accepts a valid logical position");

        const auto expectedMM = layout.toMM(Vec2{3100, 960});
        nearly(st.positionMM().x, expectedMM->x, "adopted mm x");
        nearly(st.positionMM().y, expectedMM->y, "adopted mm y");

        // A position on no monitor must invalidate rather than fabricate.
        const bool bad = st.reconcile(Vec2{-9999, -9999});
        check(!bad, "reconcile rejects a position on no monitor");
        check(!st.valid(), "state marked invalid rather than guessing");
    }

    section("gap handling");
    {
        BuildOptions withBezels;
        withBezels.gapMM = 20.0;
        Layout gapped    = buildLayout(realDesk(), withBezels);
        nearly(gapped.monitors()[1].mm.x, 620.0, "20mm bezel gap inserted");

        // A point inside the gap belongs to no monitor but still projects,
        // by clamping to the nearest edge.
        check(gapped.monitorForMM(Vec2{610, 170}) == nullptr, "gap belongs to no monitor");
        check(gapped.toLogical(Vec2{610, 170}).has_value(), "gap still projects somewhere sane");
    }

    section("EDID override");
    {
        auto descs = realDesk();
        descs[1].overrideMMWidth  = 531.0;
        descs[1].overrideMMHeight = 298.0;
        Layout overridden         = buildLayout(descs);
        nearly(overridden.monitors()[1].mm.w, 298.0, "override respected, transform still applied");
        nearly(overridden.monitors()[1].mm.h, 531.0, "override respected in the other axis");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
