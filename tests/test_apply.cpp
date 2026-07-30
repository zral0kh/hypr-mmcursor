// Tests for the motion-application step: the part that used to live inside
// plugin.cpp where nothing could reach it.
//
//   make test
//
// The compositor is simulated here, closely enough to be adversarial:
//
//   * Its cursor position is authoritative and only it can write it.
//   * It clamps whatever we hand it, the way CPointerManager::warpTo does via
//     closestValid() — so we cannot assume we got the position we asked for.
//   * Third parties (dispatchers, games releasing pointer lock, tablets) warp it
//     behind our back with no notification.
//
// The single most valuable thing asserted here is the one PLAN.md flagged as a
// trap: that our own writes do NOT get mistaken for external moves. Get that
// wrong and nothing crashes, nothing loops, and the mm accumulator quietly
// round-trips through lossy logical space on the fast path — drift arriving from
// the one place you were most confident it couldn't.

#include "../src/apply.hpp"
#include "../src/cursor_state.hpp"
#include "../src/geometry.hpp"
#include "../src/layout_build.hpp"

#include <cmath>
#include <cstdint>
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

static std::vector<MonitorDesc> realDesk() {
    MonitorDesc dp9;
    dp9.name         = "DP-9";
    dp9.logical      = Rect{0, 0, 2560, 1440};
    dp9.edidMMWidth  = 600;
    dp9.edidMMHeight = 340;
    dp9.transform    = 0;

    MonitorDesc dp10;
    dp10.name         = "DP-10";
    dp10.logical      = Rect{2560, -240, 1080, 1920};
    dp10.edidMMWidth  = 530;
    dp10.edidMMHeight = 300;
    dp10.transform    = 3;

    return {dp9, dp10};
}

// ---------------------------------------------------------------------------
// A stand-in for CPointerManager.
//
// move(delta) mirrors PointerManager.cpp:831 — add the delta, guard NaN, then
// warpTo — and warpTo mirrors :818, clamping through a closestValid analogue.
// `padding` models cursor:hotspot_padding, whose whole purpose here is to make
// the clamp perturb positions we hand it so the readback discipline is actually
// under test rather than trivially satisfied.
// ---------------------------------------------------------------------------
class FakeCompositor {
  public:
    FakeCompositor(const Layout& layout, double padding = 0.0) : m_layout(&layout), m_padding(padding) {}

    Vec2 position() const { return m_pos; }

    // Counts how many times the compositor's own move() ran. Should be exactly
    // once per event: our hook replaces the delta, it does not add a second move.
    int  moves() const { return m_moves; }

    void move(const Vec2& delta) {
        ++m_moves;
        const double dx = std::isnan(delta.x) ? 0.0 : delta.x;
        const double dy = std::isnan(delta.y) ? 0.0 : delta.y;
        warpTo({m_pos.x + dx, m_pos.y + dy});
    }

    // Everything that is not relative motion: dispatchers, tablets, client
    // warps, pointer-lock handoff. Bypasses our hook entirely, exactly as in
    // the real thing.
    void externalWarp(const Vec2& logical) { warpTo(logical); }

  private:
    void warpTo(const Vec2& logical) { m_pos = closestValid(logical); }

    // Stands in for CPointerManager::closestValid. Returns the point unchanged
    // when the padded box fits inside a SINGLE monitor rect — note single, not
    // the union, which is why points near a seam get perturbed.
    Vec2 closestValid(const Vec2& p) const {
        for (const auto& m : m_layout->monitors()) {
            if (p.x - m_padding >= m.logical.x && p.x + m_padding <= m.logical.right() && p.y - m_padding >= m.logical.y && p.y + m_padding <= m.logical.bottom())
                return p;
        }

        Vec2   best  = m_layout->monitors().front().logical.clamp(p);
        double bestD = 1e300;
        for (const auto& m : m_layout->monitors()) {
            Rect         inset{m.logical.x + m_padding, m.logical.y + m_padding, m.logical.w - 2 * m_padding, m.logical.h - 2 * m_padding};
            const Vec2   c = inset.clamp(p);
            const double d = (c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y);
            if (d < bestD) {
                bestD = d;
                best  = c;
            }
        }
        return best;
    }

    const Layout* m_layout;
    double        m_padding;
    Vec2          m_pos{};
    int           m_moves = 0;
};

// The plugin's hook, reproduced exactly: read position, plan, hand the plan to
// the compositor, read back. If this drifts from hkPointerMove in plugin.cpp the
// tests stop meaning anything, so keep the two in step.
struct Driver {
    CursorState&    cursor;
    FakeCompositor& comp;
    Vec2            lastSeen{};

    MotionPlan event(const Vec2& delta) {
        const Vec2 current = comp.position();
        const auto plan    = planMotion(cursor, current, lastSeen, delta);

        if (plan.correctedDelta)
            comp.move(*plan.correctedDelta);
        else
            comp.move(delta);

        lastSeen = comp.position(); // readback, never the request
        return plan;
    }
};

class Rng {
  public:
    explicit Rng(uint64_t seed) : m_s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next() {
        m_s ^= m_s << 13;
        m_s ^= m_s >> 7;
        m_s ^= m_s << 17;
        return m_s;
    }
    double delta(double mag) {
        const double u = static_cast<double>(next() >> 11) / static_cast<double>(1ULL << 53);
        return (u * 2.0 - 1.0) * mag;
    }

  private:
    uint64_t m_s;
};

int main() {
    const Layout layout = buildLayout(realDesk());

    // -----------------------------------------------------------------------
    section("our own writes are not mistaken for external moves");
    // -----------------------------------------------------------------------
    //
    // THE trap from PLAN.md. Our write lands via the same path a third-party
    // warp would, so a naive implementation reconciles against its own output on
    // every single event: mm -> logical -> mm, on the fast path, through the
    // lossy space. It does not crash and it does not loop; it just drifts.
    //
    // The pull model makes this structural rather than a flag to remember: the
    // readback equals the current position on the next event, so no external
    // move is reported. This test fails if the readback is ever replaced by the
    // requested target, or if the comparison is dropped.
    {
        CursorState    st;
        FakeCompositor comp(layout);
        st.setLayout(&layout);
        st.setMMPerUnit(1.0);

        comp.externalWarp(Vec2{1280, 720});
        Driver drv{st, comp, {}};

        // First event must adopt: we have no state yet.
        const auto first = drv.event(Vec2{1.0, 0.0});
        check(first.adoptedExternal, "the first event adopts the compositor position");

        int adoptions = 0;
        for (int i = 0; i < 5000; ++i) {
            if (drv.event(Vec2{0.37, -0.11}).adoptedExternal)
                ++adoptions;
        }
        check(adoptions == 0, "no further adoptions across 5000 events: our writes are recognised as ours");
        check(comp.moves() == 5001, "exactly one compositor move per event, no double motion");
    }

    // -----------------------------------------------------------------------
    section("…even when the compositor perturbs what we asked for");
    // -----------------------------------------------------------------------
    //
    // With hotspot_padding non-zero the clamp moves the cursor away from the
    // position we requested near every edge. Comparing against the request
    // rather than the readback would report a phantom external move on every
    // event there, re-reconciling constantly. This is the case that makes the
    // readback load-bearing rather than stylistic.
    {
        CursorState    st;
        FakeCompositor comp(layout, 10.0); // 10px padding: perturbs aggressively
        st.setLayout(&layout);
        st.setMMPerUnit(1.0);

        comp.externalWarp(Vec2{1280, 720});
        Driver drv{st, comp, {}};
        drv.event(Vec2{0, 0}); // seed

        // Drive hard into the top-left corner, where the clamp is always active.
        int adoptions = 0;
        for (int i = 0; i < 3000; ++i) {
            if (drv.event(Vec2{-2.0, -2.0}).adoptedExternal)
                ++adoptions;
        }
        check(adoptions == 0, "parked against a clamped edge, still no phantom external moves");
    }

    // -----------------------------------------------------------------------
    section("external moves ARE detected, exactly once each");
    // -----------------------------------------------------------------------
    {
        CursorState    st;
        FakeCompositor comp(layout);
        st.setLayout(&layout);
        st.setMMPerUnit(1.0);

        comp.externalWarp(Vec2{1280, 720});
        Driver drv{st, comp, {}};
        drv.event(Vec2{0, 0});

        for (int round = 0; round < 20; ++round) {
            // Something warps the cursor onto DP-10 behind our back.
            comp.externalWarp(Vec2{3100, 960});

            const auto after = drv.event(Vec2{0.0, 0.0});
            check(after.adoptedExternal, "the warp is noticed on the very next event");

            // And our mm state now agrees with where the cursor actually is.
            const auto expectedMM = layout.toMM(Vec2{3100, 960});
            check(expectedMM.has_value(), "warp target is on a monitor");
            if (expectedMM) {
                nearly(st.positionMM().x, expectedMM->x, "mm adopted the warp in x", 1e-9);
                nearly(st.positionMM().y, expectedMM->y, "mm adopted the warp in y", 1e-9);
            }

            // The next event must NOT adopt again.
            check(!drv.event(Vec2{0.5, 0.5}).adoptedExternal, "the same warp is not adopted twice");
        }
    }

    // -----------------------------------------------------------------------
    section("the corrected delta actually lands on the intended target");
    // -----------------------------------------------------------------------
    //
    // We hand the compositor (target - current) so that its own
    // newPos = current + delta arithmetic reproduces target exactly. If the sign
    // or the operand order were wrong the cursor would move twice as far, or
    // backwards, and every other test here would still pass.
    {
        CursorState    st;
        FakeCompositor comp(layout);
        st.setLayout(&layout);
        st.setMMPerUnit(1.0);

        comp.externalWarp(Vec2{1280, 720});
        Driver drv{st, comp, {}};
        drv.event(Vec2{0, 0});

        Rng rng(0x51DE);
        for (int i = 0; i < 5000; ++i) {
            const Vec2 d{rng.delta(3.0), rng.delta(3.0)};
            drv.event(d);

            // Where does the mm accumulator say we should be?
            const auto want = layout.toLogical(st.positionMM());
            check(want.has_value(), "mm projects to logical");
            if (!want)
                break;
            nearly(comp.position().x, want->x, "compositor cursor is where mm says, in x", 1e-9);
            nearly(comp.position().y, want->y, "compositor cursor is where mm says, in y", 1e-9);
        }
    }

    // -----------------------------------------------------------------------
    section("physical continuity across the seam, driven through the hook");
    // -----------------------------------------------------------------------
    //
    // The headline property again, but this time end to end: through planMotion,
    // through a compositor that clamps, reading the real cursor position rather
    // than the accumulator. Purely horizontal input must produce zero vertical
    // physical movement even as the cursor crosses onto a different-density
    // panel.
    {
        CursorState    st;
        FakeCompositor comp(layout);
        st.setLayout(&layout);
        st.setMMPerUnit(1.0); // 1 unit == 1 mm

        comp.externalWarp(*layout.toLogical(Vec2{300.0, 0.0})); // top edge of DP-9
        Driver drv{st, comp, {}};
        drv.event(Vec2{0, 0});

        const double startPhysY = layout.toMM(comp.position())->y;
        nearly(startPhysY, 0.0, "starts at physical y = 0", 1e-9);

        double worstPhysYDrift = 0.0;
        bool   crossed         = false;

        for (int i = 0; i < 500; ++i) {
            drv.event(Vec2{1.0, 0.0}); // 1mm right, no vertical component
            const auto physical = layout.toMM(comp.position());
            check(physical.has_value(), "cursor stays on a monitor");
            if (!physical)
                break;
            worstPhysYDrift = std::fmax(worstPhysYDrift, std::fabs(physical->y - startPhysY));
            if (comp.position().x >= 2560.0)
                crossed = true;
        }

        check(crossed, "the cursor crossed onto DP-10");
        nearly(worstPhysYDrift, 0.0, "zero physical vertical drift across the seam, measured at the cursor", 1e-9);

        // Sanity: the logical y really did change, i.e. the correction did work
        // rather than nothing having happened.
        check(std::fabs(comp.position().y - 0.0) > 50.0, "logical y changed substantially — the correction is doing something");
        nearly(comp.position().y, 104.1509, "…landing ~104px down DP-10, as the geometry tests predict", 1e-3);
    }

    // -----------------------------------------------------------------------
    section("degenerate: no layout, no interference");
    // -----------------------------------------------------------------------
    {
        const Layout   empty;
        CursorState    st;
        FakeCompositor comp(layout); // real layout so the fake can clamp
        st.setLayout(&empty);

        comp.externalWarp(Vec2{1280, 720});
        Driver drv{st, comp, {}};

        const auto plan = drv.event(Vec2{7.0, -3.0});
        check(!plan.correctedDelta.has_value(), "no layout: we hand back nothing and stock runs");
        nearly(comp.position().x, 1287.0, "…and the original delta moved the cursor normally in x");
        nearly(comp.position().y, 717.0, "…and in y");
    }
    {
        // Cursor sitting somewhere our layout does not cover. We must decline
        // rather than teleport it to a plausible-looking place.
        MonitorDesc lonely;
        lonely.name         = "SOLO";
        lonely.logical      = Rect{0, 0, 800, 600};
        lonely.edidMMWidth  = 300;
        lonely.edidMMHeight = 225;
        const Layout small  = buildLayout({lonely});

        CursorState    st;
        st.setLayout(&small);
        st.setMMPerUnit(1.0);

        FakeCompositor comp(layout); // cursor lives in the BIG layout
        comp.externalWarp(Vec2{3100, 960}); // on DP-10, unknown to `small`
        Driver drv{st, comp, {}};

        const auto plan = drv.event(Vec2{1.0, 1.0});
        check(plan.adoptedExternal, "an unknown position is treated as an external move");
        check(!plan.correctedDelta.has_value(), "…and declined rather than mapped");
        check(!st.valid(), "state stays invalid rather than inventing an mm position");
        nearly(comp.position().x, 3101.0, "the cursor moved by the original delta, untouched");
    }

    // -----------------------------------------------------------------------
    section("hotspot_padding reintroduces edge hysteresis — bounded, and known");
    // -----------------------------------------------------------------------
    //
    // A finding, not a passing formality. cursor:hotspot_padding holds the cursor
    // N logical px inside the layout. Our mm clamp knows nothing about it and
    // clamps to the true panel edge, so mm ends up describing a position the
    // cursor is not permitted to occupy: mm says logical 0, the cursor sits at
    // logical N. Walking back off the edge then produces up to N px of dead
    // travel — precisely the hysteresis this project exists to delete.
    //
    // The default is 0 (Hyprland's own ConfigValues.cpp: cursor:hotspot_padding,
    // default 0, range 0-20), so out of the box this cannot bite. The plugin
    // warns when it is non-zero. Modelling Hyprland's padding geometry inside our
    // core was rejected deliberately: it would couple the tested core to a
    // compositor quirk, and Hyprland's own padding logic tests containment
    // against a SINGLE monitor rect, so it already creates dead zones at internal
    // seams that have nothing to do with us.
    //
    // What is asserted is therefore the true, bounded property: the discrepancy
    // never exceeds the padding, and at padding 0 it is exactly zero.
    {
        for (double pad : {0.0, 1.0, 4.0, 20.0}) {
            CursorState    st;
            FakeCompositor comp(layout, pad);
            st.setLayout(&layout);
            st.setMMPerUnit(1.0);

            comp.externalWarp(Vec2{1280, 720});
            Driver drv{st, comp, {}};
            drv.event(Vec2{0, 0});

            // Park hard against the left edge, where the effect is maximal.
            for (int i = 0; i < 2000; ++i)
                drv.event(Vec2{-30.0, 0.0});

            const auto want = layout.toLogical(st.positionMM());
            check(want.has_value(), "mm still projects");
            if (!want)
                continue;

            const double gap = std::fabs(comp.position().x - want->x);
            check(gap <= pad + 1e-9, "edge discrepancy is bounded by hotspot_padding (" + std::to_string(static_cast<int>(pad)) + "px)");
            if (pad == 0.0)
                nearly(gap, 0.0, "at the default padding of 0 there is no discrepancy at all", 1e-9);
        }
    }

    // -----------------------------------------------------------------------
    section("random session: warps, motion and clamping interleaved");
    // -----------------------------------------------------------------------
    //
    // The invariant that must survive everything: after any event, the
    // compositor's cursor is where the mm accumulator says it should be — unless
    // we declined the event, in which case we must have declined honestly by
    // marking ourselves invalid.
    //
    // Run at padding 0, the default and the only setting under which exact
    // tracking is achievable — see the section above for why, and for the bounded
    // guarantee that replaces it when padding is non-zero.
    {
        for (uint64_t seed : {7ULL, 99ULL, 0xFEEDULL}) {
            CursorState    st;
            FakeCompositor comp(layout, 0.0);
            st.setLayout(&layout);
            st.setMMPerUnit(0.25);

            comp.externalWarp(Vec2{1280, 720});
            Driver drv{st, comp, {}};

            Rng  rng(seed);
            bool broke = false;

            for (int i = 0; i < 20000; ++i) {
                // One event in fifty, something else moves the cursor.
                if ((rng.next() % 50) == 0) {
                    const bool ontoDP10 = (rng.next() % 2) == 0;
                    comp.externalWarp(ontoDP10 ? Vec2{2600 + rng.delta(400), 100 + rng.delta(800)} : Vec2{1280 + rng.delta(1000), 720 + rng.delta(600)});
                }

                const auto plan = drv.event(Vec2{rng.delta(30.0), rng.delta(30.0)});

                if (!plan.correctedDelta) {
                    if (st.valid()) { // declined but claimed to be fine: contradiction
                        broke = true;
                        break;
                    }
                    continue;
                }

                const auto want = layout.toLogical(st.positionMM());
                if (!want || std::fabs(comp.position().x - want->x) > 1e-6 || std::fabs(comp.position().y - want->y) > 1e-6) {
                    broke = true;
                    break;
                }
            }
            check(!broke, "cursor tracks the mm accumulator through 20k mixed events (seed " + std::to_string(seed) + ")");
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
