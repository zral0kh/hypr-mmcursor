// layout_build.hpp — turning what Hyprland has active, plus what the user tells
// us, into a physical desk layout.
//
// The user-facing config is deliberately in millimetres, because that is the
// unit the hardware already reports. EDID carries the panel's physical size,
// Hyprland surfaces it as `physical size (mm)` in `hyprctl monitors`, and it
// is the only number in this whole system that describes the real world. Asking
// someone for "the vertical offset between my two monitors, in mm" is a
// question they can answer with a tape measure. Asking for a logical pixel
// offset is a question they can only answer by recomputing it every time a
// scale factor changes.
//
// ---------------------------------------------------------------------------
// How placement is derived
// ---------------------------------------------------------------------------
//
// The desk layout is derived from the arrangement Hyprland actually has active
// — each monitor's logical rect — rather than synthesised from scratch. That
// means `auto`, `auto-center-right`, hand-written offsets and everything else
// all arrive here already resolved, and we never have to parse a monitor rule.
//
// The subtlety, and the reason this is not a coordinate conversion: a logical
// offset cannot be converted to millimetres by dividing by a density, because
// the two panels have different densities and the offset spans both. On the
// desk this was written for, DP-10 sits at logical y = -240 against DP-9's
// 1440px/340mm. Dividing gives 56.7mm. The physically true answer is 95mm.
//
// What the logical layout actually expresses is a *relation*: both centres sit
// at logical y = 720, i.e. "centred". Reproduce the relation physically and the
// answer is exact. So for each seam we ask which of three relations the logical
// arrangement states — near edges flush, far edges flush, or centres flush —
// take the closest, reproduce it exactly in mm, and convert only the leftover
// residual through the anchor's density. An exactly-stated relation therefore
// converts exactly, and anything in between degrades continuously. There is no
// tolerance knob to tune.
//
// ZERO Hyprland dependencies. Keep it that way.

#pragma once

#include "geometry.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pcs {

// wlroots / Hyprland output transform values.
//   0 normal, 1 = 90, 2 = 180, 3 = 270, 4..7 = the flipped variants.
// Odd values mod 4 rotate by a quarter turn and therefore swap which physical
// dimension of the panel runs along which desk axis.
inline bool transformSwapsAxes(int transform) {
    const int t = transform % 4;
    return t == 1 || t == 3;
}

// How a monitor lines up with its anchor across the seam.
//
// Start/End mean top/bottom at a horizontal seam and left/right at a vertical
// one, because the computation is identical with the axes swapped and naming
// them per-axis would mean writing it twice.
//
// Derive — the default — reads the relation out of the active logical layout.
// The other three force it, which is the escape hatch for when the logical
// arrangement is itself wrong.
enum class Align {
    Derive,
    Start,
    Center,
    End,
};

// Which side of its anchor a monitor sits on, when stated explicitly.
enum class Edge {
    RightOf,
    LeftOf,
    Above,
    Below,
};

// A user-stated placement, overriding what would otherwise be derived.
//
// Either absolute (an mm origin on the desk, which also makes this monitor an
// anchor for everything else) or relational (an edge and an anchor to hang off).
// Relational placement is what a human writes and it survives a resolution
// change; absolute is what an alignment GUI would write back after a drag.
struct PlacementSpec {
    std::optional<Vec2> absoluteMM;

    std::optional<Edge> edge;
    std::string         anchor;

    // Alignment across the seam. Derive still reads the relation from the
    // logical layout even when the edge is stated explicitly.
    Align align = Align::Derive;

    // Extra millimetres along the cross axis of the relation, applied after
    // alignment.
    double offsetMM = 0.0;
};

struct MonitorDesc {
    std::string name;

    // Straight from Hyprland: already scaled and transformed.
    Rect logical;

    // Straight from EDID, in the panel's NATIVE orientation. This is the
    // `physical size (mm)` field. It is wrong on a depressing number of
    // panels, hence the override below.
    double edidMMWidth  = 0.0;
    double edidMMHeight = 0.0;

    int transform = 0;

    // Manual override for the above, in native panel orientation. Set this
    // when EDID lies (0x0, obviously-wrong values, headless outputs).
    std::optional<double> overrideMMWidth;
    std::optional<double> overrideMMHeight;

    // Extra physical offset applied after placement, in mm. Positive y pushes
    // the panel down the desk, positive x to the right. Anything anchored to
    // this monitor inherits the shift, because the panel really did move.
    Vec2 offsetMM{};

    // Fully or partly explicit placement, overriding derivation.
    std::optional<PlacementSpec> placement;
};

// A bezel measurement for one specific seam, overriding BuildOptions::gapMM.
// Unordered — {a,b} and {b,a} are the same seam.
struct SeamGap {
    std::string a;
    std::string b;
    double      mm = 0.0;
};

struct BuildOptions {
    // Derive reads each seam's alignment from the active layout. Anything else
    // forces every derived seam to that alignment and drops the residual, which
    // makes `align = center` reproduce the old row builder exactly.
    Align align = Align::Derive;

    // Physical millimetres of bezel/gap to insert between adjacent panels.
    //
    // 0.0 collapses gaps: panels become edge-adjacent in mm space even though
    // real bezels exist. This is a deliberate lie that feels correct — the
    // alternative is a dead zone where the cursor is "in the bezel" and visibly
    // stalls. Set it to a real measurement if you want physical honesty.
    //
    // Collapsing at build time (rather than special-casing gaps at projection
    // time) is what keeps clampMM idempotent and the whole thing drift-free.
    double gapMM = 0.0;

    // Per-seam overrides of the above. Bezels differ between pairs.
    std::vector<SeamGap> seamGaps;
};

// How each monitor ended up where it did, and anything the builder had to guess
// at. Optional: pass nullptr if you only want the layout.
//
// This exists so a wrong layout is diagnosable from `hyprctl mmcursor` in one
// glance, instead of by moving the mouse around and forming a theory.
struct BuildDiagnostics {
    struct Placement {
        std::string name;
        std::string how;    // "root", "at 620.0,-95.0", "right-of DP-9 centre", "fallback"
        std::string anchor; // empty for roots and fallbacks
        Vec2        residualMM{};
    };

    std::vector<Placement>   placements;
    std::vector<std::string> warnings;
};

// Physical extent on the desk, after applying the transform and any override.
Vec2 physicalSizeMM(const MonitorDesc& d);

// Derives the desk layout from the monitors' active logical arrangement,
// honouring any explicit placements.
//
// Monitors come back in placement order, root first. That ordering is load
// bearing: defaultMMPerUnit() and CursorState's seed both key off
// monitors().front(), so the root is what sets pointer feel, and naming a root
// with an absolute placement is how you pin that to a chosen panel.
Layout buildLayout(std::vector<MonitorDesc> descs, const BuildOptions& opts = {}, BuildDiagnostics* diag = nullptr);

// Default pointer sensitivity, in mm of desk travel per unit of input delta.
//
// Chosen as the inverse density of the root monitor so that enabling the
// plugin is feel-neutral there and only changes behaviour elsewhere. libinput's
// accelerated deltas are in a nominal pixel-ish unit; multiplying by a constant
// preserves the shape of its acceleration curve, so this is a speed knob and
// nothing more.
double defaultMMPerUnit(const Layout& layout);

} // namespace pcs
