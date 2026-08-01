// Model-level tests: the properties this plugin exists to provide, stated
// directly, plus a differential comparison against stock Hyprland behaviour.
//
//   make test
//
// test_geometry.cpp checks that the pieces compute what they claim. This file
// checks the thing the user actually cares about: that equal hand movement
// produces equal physical cursor movement, and that ours differs from stock in
// exactly one respect and no others.
//
// No Hyprland, no compositor, no GPU. All of it runs in milliseconds.

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

static void exactly(double a, double b, const std::string& what) {
    ++g_checks;
    if (a != b) {
        ++g_failures;
        std::printf("  FAIL  %s  (%.17g vs %.17g)\n", what.c_str(), a, b);
    }
}

static void section(const char* name) {
    std::printf("\n%s\n", name);
}

// ---------------------------------------------------------------------------
// The desk this was written for. Same numbers as test_geometry.cpp.
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

// ---------------------------------------------------------------------------
// Tier 2's reference model: stock Hyprland, in twenty lines.
//
// The entire behaviour being compared against is "accumulate the delta in
// logical pixel space, clamp to the union of monitor rects, and carry
// coordinates across seams unchanged". There is no seam handling because from
// logical space's point of view there is no seam — that is the bug.
//
// clampLogical mirrors Layout::clampMM exactly, but over the logical rects.
// Keeping it a separate implementation rather than reusing ours is deliberate:
// a differential test whose two sides share the code under test proves nothing.
// ---------------------------------------------------------------------------
class StockModel {
  public:
    explicit StockModel(const Layout& layout) : m_layout(&layout) {}

    void  seed(const Vec2& logical) { m_pos = logical; }
    Vec2  position() const { return m_pos; }

    void applyRelative(const Vec2& delta) {
        m_pos = clampLogical({m_pos.x + delta.x, m_pos.y + delta.y});
    }

  private:
    Vec2 clampLogical(const Vec2& p) const {
        for (const auto& m : m_layout->monitors()) {
            if (m.logical.contains(p))
                return p;
        }

        Vec2   best  = m_layout->monitors().front().logical.clamp(p);
        double bestD = (best.x - p.x) * (best.x - p.x) + (best.y - p.y) * (best.y - p.y);
        for (const auto& m : m_layout->monitors()) {
            const Vec2   c = m.logical.clamp(p);
            const double d = (c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y);
            if (d < bestD) {
                bestD = d;
                best  = c;
            }
        }
        return best;
    }

    const Layout* m_layout;
    Vec2          m_pos{};
};

// A deterministic PRNG. std::mt19937 would do, but a seeded xorshift keeps the
// failure reproducible from the printed seed alone with no library version in
// the way.
class Rng {
  public:
    explicit Rng(uint64_t seed) : m_s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}

    uint64_t next() {
        m_s ^= m_s << 13;
        m_s ^= m_s >> 7;
        m_s ^= m_s << 17;
        return m_s;
    }

    // Uniform in [-mag, mag].
    double delta(double mag) {
        const double u = static_cast<double>(next() >> 11) / static_cast<double>(1ULL << 53);
        return (u * 2.0 - 1.0) * mag;
    }

  private:
    uint64_t m_s;
};

// Is p inside the closed union of mm rects? Closed, not half-open: clamping
// legitimately parks the cursor exactly on a far edge, which Rect::contains
// excludes by design.
static bool inClosedMMUnion(const Layout& layout, const Vec2& p, double eps = 1e-9) {
    for (const auto& m : layout.monitors()) {
        if (p.x >= m.mm.x - eps && p.x <= m.mm.right() + eps && p.y >= m.mm.y - eps && p.y <= m.mm.bottom() + eps)
            return true;
    }
    return false;
}

int main() {
    const Layout layout = buildLayout(realDesk());

    // Densities along the seam axis. These two numbers are the whole problem.
    const double DP9_PX_PER_MM_Y  = 1440.0 / 340.0; // 4.2353
    const double DP10_PX_PER_MM_Y = 1920.0 / 530.0; // 3.6226

    // -----------------------------------------------------------------------
    section("Tier 4 — the headline property: equal mm travel, equal physical travel");
    // -----------------------------------------------------------------------
    //
    // This is the guarantee the entire plugin exists to provide, and before this
    // test it was asserted nowhere. 10mm of hand movement must move the cursor
    // 10mm of glass on every panel, which means a different number of logical
    // pixels on each.
    {
        CursorState st;
        st.setLayout(&layout);
        st.setMMPerUnit(1.0); // one input unit == one mm, keeps it legible

        // On DP-9, mid-panel.
        st.reconcile(*layout.toLogical(Vec2{300.0, 170.0}));
        const double beforeDP9 = layout.toLogical(st.positionMM())->y;
        st.applyRelative(Vec2{0.0, 10.0});
        const double afterDP9 = layout.toLogical(st.positionMM())->y;

        nearly(afterDP9 - beforeDP9, 10.0 * DP9_PX_PER_MM_Y, "10mm on DP-9 moves 10 x 4.2353 logical px", 1e-6);

        // On DP-10, mid-panel. mm x 750 is inside [600, 900).
        st.reconcile(*layout.toLogical(Vec2{750.0, 170.0}));
        const double beforeDP10 = layout.toLogical(st.positionMM())->y;
        st.applyRelative(Vec2{0.0, 10.0});
        const double afterDP10 = layout.toLogical(st.positionMM())->y;

        nearly(afterDP10 - beforeDP10, 10.0 * DP10_PX_PER_MM_Y, "10mm on DP-10 moves 10 x 3.6226 logical px", 1e-6);

        // Different pixel counts...
        check(std::fabs((afterDP9 - beforeDP9) - (afterDP10 - beforeDP10)) > 5.0, "the two panels move a visibly different number of pixels");

        // ...for the same physical distance. That is the point.
        nearly(st.positionMM().y - 170.0, 10.0, "physical travel on DP-10 is 10mm", 1e-9);

        // And the ratio of pixel travel is exactly the ratio of densities.
        nearly((afterDP9 - beforeDP9) / (afterDP10 - beforeDP10), DP9_PX_PER_MM_Y / DP10_PX_PER_MM_Y, "pixel-travel ratio equals the density ratio", 1e-9);
    }

    // -----------------------------------------------------------------------
    section("Tier 4b — the same property when the mismatch is a scale, not a panel");
    // -----------------------------------------------------------------------
    //
    // Two identical panels, one at scale 2. Hyprland folds the scale into
    // m_size before we ever see it, so from here this is just another density
    // mismatch — but that is a claim, and this is what makes it a tested one.
    // A HiDPI laptop next to an external monitor is the common shape of this.
    {
        const Layout scaled = buildLayout({[] {
                                               MonitorDesc d;
                                               d.name         = "HIDPI";
                                               d.logical      = Rect{0, 0, 1280, 720}; // 2560x1440 at scale 2
                                               d.edidMMWidth  = 600;
                                               d.edidMMHeight = 340;
                                               return d;
                                           }(),
                                           [] {
                                               MonitorDesc d;
                                               d.name         = "PLAIN";
                                               d.logical      = Rect{1280, 0, 2560, 1440};
                                               d.edidMMWidth  = 600;
                                               d.edidMMHeight = 340;
                                               return d;
                                           }()});

        nearly(scaled.monitors()[1].mm.x, 600.0, "identical panels are flush regardless of scale");
        nearly(scaled.monitors()[0].mm.y, scaled.monitors()[1].mm.y, "and share a horizon");

        CursorState st;
        st.setLayout(&scaled);
        st.setMMPerUnit(1.0);

        st.reconcile(*scaled.toLogical(Vec2{300.0, 170.0}));
        const double beforeHi = scaled.toLogical(st.positionMM())->y;
        st.applyRelative(Vec2{0.0, 10.0});
        nearly(scaled.toLogical(st.positionMM())->y - beforeHi, 10.0 * (720.0 / 340.0), "10mm on the scale-2 panel moves half as many logical px", 1e-6);

        st.reconcile(*scaled.toLogical(Vec2{900.0, 170.0}));
        const double beforePlain = scaled.toLogical(st.positionMM())->y;
        st.applyRelative(Vec2{0.0, 10.0});
        nearly(scaled.toLogical(st.positionMM())->y - beforePlain, 10.0 * (1440.0 / 340.0), "10mm on the scale-1 panel moves twice as many", 1e-6);

        // Stock carries logical y across unchanged, so the scale mismatch
        // becomes a physical jump exactly the way a panel mismatch does.
        StockModel stock(scaled);
        stock.seed(Vec2{1279.0, 719.0}); // bottom-right of the scaled panel
        stock.applyRelative(Vec2{2.0, 0.0});
        const auto crossed = scaled.toMM(stock.position());
        check(crossed.has_value(), "stock lands on the other panel");
        if (crossed)
            check(std::fabs(crossed->y - 339.5) > 100.0, "and it lands over 100mm away from where it left");
    }

    // -----------------------------------------------------------------------
    section("Tier 2 — differential against stock: the seam discontinuity");
    // -----------------------------------------------------------------------
    //
    // Identical input stream through both models. Both trajectories are then
    // converted to physical mm and diffed. The assertion is not "ours is
    // better" but something sharper and falsifiable: stock's physical position
    // is DISCONTINUOUS at the seam and ours is CONTINUOUS, under a purely
    // horizontal input stream that never touches an edge.
    {
        CursorState ours;
        ours.setLayout(&layout);
        ours.setMMPerUnit(1.0); // 1 unit == 1 mm

        StockModel stock(layout);

        // Both start at the same physical point: the top edge of DP-9. This is
        // where the error is largest, and it is the exact case in the README.
        const Vec2 startMM{300.0, 0.0};
        ours.reconcile(*layout.toLogical(startMM));
        stock.seed(*layout.toLogical(startMM));

        nearly(ours.positionMM().y, 0.0, "ours starts at physical y = 0");
        nearly(layout.toMM(stock.position())->y, 0.0, "stock starts at the same physical y = 0");

        // Stock consumes logical px; ours consumes mm. To keep the input stream
        // physically identical, each step is 1mm for us and the DP-9-equivalent
        // number of logical px for stock. On DP-9 they are the same movement,
        // which is the whole premise: the two models agree until a seam.
        const double DP9_PX_PER_MM_X = 2560.0 / 600.0;

        double ourMaxPhysicalYStep   = 0.0;
        double stockMaxPhysicalYStep = 0.0;

        double ourPrevY   = ours.positionMM().y;
        double stockPrevY = layout.toMM(stock.position())->y;

        bool ourCrossed   = false;
        bool stockCrossed = false;

        for (int i = 0; i < 500; ++i) {
            ours.applyRelative(Vec2{1.0, 0.0});
            stock.applyRelative(Vec2{DP9_PX_PER_MM_X, 0.0});

            const double ourY = ours.positionMM().y;
            const auto   sMM  = layout.toMM(stock.position());
            check(sMM.has_value(), "stock stays on some monitor");
            if (!sMM)
                break;

            ourMaxPhysicalYStep   = std::fmax(ourMaxPhysicalYStep, std::fabs(ourY - ourPrevY));
            stockMaxPhysicalYStep = std::fmax(stockMaxPhysicalYStep, std::fabs(sMM->y - stockPrevY));

            ourPrevY   = ourY;
            stockPrevY = sMM->y;

            if (ours.positionMM().x > 600.0)
                ourCrossed = true;
            if (stock.position().x >= 2560.0)
                stockCrossed = true;
        }

        check(ourCrossed, "our model crossed the seam");
        check(stockCrossed, "stock model crossed the seam");

        // The input had no vertical component at all, so any physical vertical
        // movement is an artefact of the model.
        nearly(ourMaxPhysicalYStep, 0.0, "ours never moves vertically: continuous across the seam", 1e-12);
        check(stockMaxPhysicalYStep > 25.0, "stock jumps > 25mm vertically at the seam: discontinuous");
        nearly(stockMaxPhysicalYStep, 28.75, "…and the jump is the documented 28.75mm", 1e-2);

        // End state, stated as physical error rather than as a step.
        nearly(ours.positionMM().y, 0.0, "ours ends at the physical height it started", 1e-12);
        check(std::fabs(layout.toMM(stock.position())->y) > 25.0, "stock ends ~28.75mm from where it started, having only moved sideways");
    }

    // -----------------------------------------------------------------------
    section("Tier 3 — no-op equivalence at uniform density");
    // -----------------------------------------------------------------------
    //
    // If every panel has the same density there is nothing to correct, and the
    // two models must agree. This is cheap and it catches whole classes of sign
    // and axis-ordering error that example-based tests walk straight past: get
    // the projection backwards and the headline test still passes on one
    // monitor, but this one fails immediately.
    //
    // Constructed at exactly 1 px/mm so mm and logical coincide numerically and
    // the comparison can be bit-exact rather than epsilon-based.
    {
        MonitorDesc a;
        a.name         = "A";
        a.logical      = Rect{0, 0, 1000, 1000};
        a.edidMMWidth  = 1000;
        a.edidMMHeight = 1000;

        MonitorDesc b;
        b.name         = "B";
        b.logical      = Rect{1000, 0, 1000, 1000};
        b.edidMMWidth  = 1000;
        b.edidMMHeight = 1000;

        const Layout uniform = buildLayout({a, b});
        nearly(uniform.monitors()[0].pxPerMMx(), 1.0, "unit density by construction");
        nearly(uniform.monitors()[1].pxPerMMx(), 1.0, "unit density on both");

        CursorState ours;
        ours.setLayout(&uniform);
        ours.setMMPerUnit(defaultMMPerUnit(uniform));
        nearly(ours.mmPerUnit(), 1.0, "default sensitivity is 1mm per unit at unit density");

        StockModel stock(uniform);

        ours.reconcile(Vec2{500, 500});
        stock.seed(Vec2{500, 500});

        Rng rng(0xC0FFEE);
        for (int i = 0; i < 20000; ++i) {
            const Vec2 d{rng.delta(7.0), rng.delta(7.0)};
            const auto ourLogical = ours.applyRelative(d);
            stock.applyRelative(d);

            check(ourLogical.has_value(), "projection succeeds");
            if (!ourLogical)
                break;

            // Bit-identical, not merely close.
            exactly(ourLogical->x, stock.position().x, "uniform density: logical x identical to stock");
            exactly(ourLogical->y, stock.position().y, "uniform density: logical y identical to stock");
        }
    }

    // -----------------------------------------------------------------------
    section("Tier 3b — uniform density that is not 1:1");
    // -----------------------------------------------------------------------
    //
    // Same property at 2 px/mm. mm and logical no longer coincide, so the two
    // models associate their arithmetic differently and bit-equality is no
    // longer available — but they must still agree to floating-point noise.
    // Asserting exactness here would be asserting a coincidence, not a property.
    {
        MonitorDesc a;
        a.name         = "A";
        a.logical      = Rect{0, 0, 1000, 1000};
        a.edidMMWidth  = 500;
        a.edidMMHeight = 500;

        MonitorDesc b;
        b.name         = "B";
        b.logical      = Rect{1000, 0, 1000, 1000};
        b.edidMMWidth  = 500;
        b.edidMMHeight = 500;

        const Layout uniform = buildLayout({a, b});
        CursorState  ours;
        ours.setLayout(&uniform);
        ours.setMMPerUnit(defaultMMPerUnit(uniform));
        nearly(ours.mmPerUnit(), 0.5, "1 unit == 1 logical px at 2 px/mm");

        StockModel stock(uniform);
        ours.reconcile(Vec2{500, 500});
        stock.seed(Vec2{500, 500});

        Rng    rng(0xBEEF);
        double worst = 0.0;
        for (int i = 0; i < 20000; ++i) {
            const Vec2 d{rng.delta(7.0), rng.delta(7.0)};
            const auto ourLogical = ours.applyRelative(d);
            stock.applyRelative(d);
            if (!ourLogical)
                break;
            worst = std::fmax(worst, std::fabs(ourLogical->x - stock.position().x));
            worst = std::fmax(worst, std::fabs(ourLogical->y - stock.position().y));
        }
        check(worst < 1e-9, "uniform density at 2 px/mm: agrees with stock to 1e-9 over 20k steps");
    }

    // -----------------------------------------------------------------------
    section("Tier 1 — properties under random delta streams");
    // -----------------------------------------------------------------------
    {
        // Property: the accumulator never leaves the closed union of mm rects,
        // no matter what it is fed.
        for (uint64_t seed : {1ULL, 42ULL, 0xDEADBEEFULL, 0x5EEDULL}) {
            CursorState st;
            st.setLayout(&layout);
            st.setMMPerUnit(0.25);
            st.reconcile(Vec2{1280, 720});

            Rng  rng(seed);
            bool escaped = false;
            for (int i = 0; i < 50000; ++i) {
                st.applyRelative(Vec2{rng.delta(40.0), rng.delta(40.0)});
                if (!inClosedMMUnion(layout, st.positionMM())) {
                    escaped = true;
                    break;
                }
            }
            check(!escaped, "accumulator stays inside the mm union (seed " + std::to_string(seed) + ")");
        }

        // Property: clampMM is idempotent everywhere, including well outside.
        {
            Rng  rng(0xABCDEF);
            bool broke = false;
            for (int i = 0; i < 50000; ++i) {
                const Vec2 p{rng.delta(4000.0), rng.delta(4000.0)};
                const Vec2 once  = layout.clampMM(p);
                const Vec2 twice = layout.clampMM(once);
                if (once.x != twice.x || once.y != twice.y) {
                    broke = true;
                    break;
                }
            }
            check(!broke, "clampMM is idempotent over 50k random points, exactly");
        }

        // Property: a path and its exact reverse return to the origin, PROVIDED
        // no clamp fired. Clamping destroys information legitimately, so the
        // excursion is bounded to stay well clear of every edge — which makes
        // the precondition true by construction rather than by hoping.
        {
            const Vec2 startMM{300.0, 170.0}; // DP-9 centre, 300mm from any edge
            CursorState st;
            st.setLayout(&layout);
            st.setMMPerUnit(1.0);
            st.reconcile(*layout.toLogical(startMM));

            Rng               rng(0x1234);
            std::vector<Vec2> path;
            path.reserve(2000);

            // Each step at most 0.05mm, 2000 steps: total excursion <= 100mm in
            // either axis, so from the centre of DP-9 no clamp can fire.
            for (int i = 0; i < 2000; ++i) {
                const Vec2 d{rng.delta(0.05), rng.delta(0.05)};
                path.push_back(d);
                st.applyRelative(d);
            }

            check(inClosedMMUnion(layout, st.positionMM()), "excursion stayed in bounds");
            check(std::fabs(st.positionMM().x - startMM.x) < 100.0 && std::fabs(st.positionMM().y - startMM.y) < 100.0, "excursion stayed clear of edges, so no clamp fired");

            for (auto it = path.rbegin(); it != path.rend(); ++it)
                st.applyRelative(Vec2{-it->x, -it->y});

            nearly(st.positionMM().x, startMM.x, "exact reverse path returns to origin in x", 1e-9);
            nearly(st.positionMM().y, startMM.y, "exact reverse path returns to origin in y", 1e-9);
        }
    }

    // -----------------------------------------------------------------------
    section("degenerate cases");
    // -----------------------------------------------------------------------
    {
        // Single monitor. The row builder, the clamp and the projection must all
        // still work with nothing to cross to.
        MonitorDesc only;
        only.name         = "SOLO";
        only.logical      = Rect{0, 0, 1920, 1080};
        only.edidMMWidth  = 520;
        only.edidMMHeight = 290;

        const Layout single = buildLayout({only});
        check(single.monitors().size() == 1, "single-monitor layout builds");
        nearly(single.monitors()[0].mm.x, 0.0, "single monitor sits at the mm origin");

        CursorState st;
        st.setLayout(&single);
        st.setMMPerUnit(1.0);
        st.reconcile(Vec2{960, 540});
        for (int i = 0; i < 5000; ++i)
            st.applyRelative(Vec2{1.0, 1.0});
        check(inClosedMMUnion(single, st.positionMM()), "single monitor clamps rather than escaping");
        nearly(st.positionMM().x, 520.0, "pinned to the right edge");
        nearly(st.positionMM().y, 290.0, "pinned to the bottom edge");
    }
    {
        // An empty layout must be inert, not crash and not fabricate.
        const Layout empty;
        CursorState  st;
        st.setLayout(&empty);
        check(!st.applyRelative(Vec2{5, 5}).has_value(), "empty layout yields no position");
        check(!st.reconcile(Vec2{0, 0}), "empty layout cannot reconcile");
        check(!st.valid(), "…and stays invalid");
    }
    {
        // Zero delta must not move anything.
        CursorState st;
        st.setLayout(&layout);
        st.setMMPerUnit(1.0);
        st.reconcile(Vec2{1280, 720});
        const Vec2 before = st.positionMM();
        st.applyRelative(Vec2{0.0, 0.0});
        exactly(st.positionMM().x, before.x, "zero delta does not move x");
        exactly(st.positionMM().y, before.y, "zero delta does not move y");
    }
    {
        // Non-finite deltas from a misbehaving device. A single NaN reaching the
        // accumulator would poison it for the life of the session: NaN survives
        // every addition and every comparison in clampMM returns false, so the
        // cursor would never recover. The guard is in CursorState, so it is
        // tested here rather than living unexercised in plugin.cpp.
        CursorState st;
        st.setLayout(&layout);
        st.setMMPerUnit(1.0);
        st.reconcile(Vec2{1280, 720});
        const Vec2 before = st.positionMM();

        const double NAN_ = std::nan("");
        const double INF_ = std::numeric_limits<double>::infinity();

        st.applyRelative(Vec2{NAN_, 0.0});
        st.applyRelative(Vec2{0.0, NAN_});
        st.applyRelative(Vec2{INF_, -INF_});
        st.applyRelative(Vec2{NAN_, NAN_});

        check(std::isfinite(st.positionMM().x) && std::isfinite(st.positionMM().y), "non-finite deltas do not poison the accumulator");
        exactly(st.positionMM().x, before.x, "non-finite delta treated as zero in x");
        exactly(st.positionMM().y, before.y, "non-finite delta treated as zero in y");

        // And the accumulator still works afterwards.
        st.applyRelative(Vec2{10.0, 0.0});
        nearly(st.positionMM().x, before.x + 10.0, "accumulator still healthy after a NaN");
    }
    {
        // Absurd but finite deltas must clamp, not overflow into nonsense.
        CursorState st;
        st.setLayout(&layout);
        st.setMMPerUnit(1.0);
        st.reconcile(Vec2{1280, 720});
        st.applyRelative(Vec2{1e18, -1e18});
        check(inClosedMMUnion(layout, st.positionMM()), "an absurd delta clamps into the union");
        check(std::isfinite(st.positionMM().x) && std::isfinite(st.positionMM().y), "…and stays finite");
    }
    {
        // buildLayout must be idempotent: same input, same output. The builder
        // sorts by logical x and mutates through pointers, which is exactly the
        // shape of code that quietly stops being a pure function.
        const Layout a = buildLayout(realDesk());
        const Layout b = buildLayout(realDesk());
        check(a.monitors().size() == b.monitors().size(), "buildLayout is idempotent in size");
        for (std::size_t i = 0; i < a.monitors().size(); ++i) {
            check(a.monitors()[i].name == b.monitors()[i].name, "buildLayout is idempotent in ordering");
            exactly(a.monitors()[i].mm.x, b.monitors()[i].mm.x, "buildLayout is idempotent in mm x");
            exactly(a.monitors()[i].mm.y, b.monitors()[i].mm.y, "buildLayout is idempotent in mm y");
            exactly(a.monitors()[i].mm.w, b.monitors()[i].mm.w, "buildLayout is idempotent in mm w");
            exactly(a.monitors()[i].mm.h, b.monitors()[i].mm.h, "buildLayout is idempotent in mm h");
        }

        // Input order must not matter either — the builder sorts by logical x.
        auto reversed = realDesk();
        std::swap(reversed[0], reversed[1]);
        const Layout c = buildLayout(reversed);
        for (std::size_t i = 0; i < a.monitors().size(); ++i) {
            check(a.monitors()[i].name == c.monitors()[i].name, "declaration order does not affect the row");
            exactly(a.monitors()[i].mm.x, c.monitors()[i].mm.x, "declaration order does not affect mm x");
        }
    }
    {
        // Overlapping mm rects are a configuration error, not something to
        // silently resolve by declaration order.
        check(!buildLayout(realDesk()).firstMMOverlap().has_value(), "the real desk has no overlap");

        // Edge-adjacent is the normal case and must NOT report an overlap.
        MonitorDesc a;
        a.name         = "A";
        a.logical      = Rect{0, 0, 1000, 1000};
        a.edidMMWidth  = 500;
        a.edidMMHeight = 500;
        MonitorDesc b = a;
        b.name        = "B";
        b.logical     = Rect{1000, 0, 1000, 1000};
        check(!buildLayout({a, b}).firstMMOverlap().has_value(), "edge-adjacent panels are not an overlap");

        // Two explicit origins on top of each other are.
        MonitorDesc   c = a;
        MonitorDesc   d = a;
        d.name          = "D";
        PlacementSpec pc;
        pc.absoluteMM = Vec2{0, 0};
        c.placement   = pc;
        PlacementSpec pd;
        pd.absoluteMM    = Vec2{250, 250};
        d.placement      = pd;
        const auto clash = buildLayout({c, d}).firstMMOverlap();
        check(clash.has_value(), "overlapping explicit origins are detected");
        if (clash)
            check((clash->first == "A" && clash->second == "D"), "the overlapping pair is named");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
