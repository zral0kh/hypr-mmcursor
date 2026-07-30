// layout_build.hpp — turning what Hyprland knows plus what the user tells us
// into a physical desk layout.
//
// The user-facing config is deliberately in millimetres, because that is the
// unit the hardware already reports. EDID carries the panel's physical size,
// Hyprland surfaces it as `physical size (mm)` in `hyprctl monitors`, and it
// is the only number in this whole system that describes the real world. Asking
// someone for "the vertical offset between my two monitors, in mm" is a
// question they can answer with a tape measure. Asking for a logical pixel
// offset is a question they can only answer by recomputing it every time a
// scale factor changes.

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

enum class VAlign {
    Center, // align physical centres — the sane default for a desk row
    Top,    // align physical top edges
    Bottom, // align physical bottom edges
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

    // Extra physical offset applied after alignment, in mm. Positive y pushes
    // the panel down the desk. Use this when one monitor genuinely sits higher
    // than the other and you want the software to know.
    double offsetMM = 0.0;

    // Fully explicit placement, bypassing the row builder entirely. Needed for
    // any arrangement that is not a single horizontal row.
    std::optional<Vec2> explicitMMOrigin;
};

struct BuildOptions {
    VAlign align = VAlign::Center;

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
};

// Physical extent on the desk, after applying the transform and any override.
Vec2 physicalSizeMM(const MonitorDesc& d);

// Builds a left-to-right row in mm space, ordered by existing logical x.
// Monitors with explicitMMOrigin are placed there and excluded from the row.
Layout buildLayout(std::vector<MonitorDesc> descs, const BuildOptions& opts = {});

// Default pointer sensitivity, in mm of desk travel per unit of input delta.
//
// Chosen as the inverse density of the first monitor so that enabling the
// plugin is feel-neutral on the primary display and only changes behaviour
// elsewhere. libinput's accelerated deltas are in a nominal pixel-ish unit;
// multiplying by a constant preserves the shape of its acceleration curve, so
// this is a speed knob and nothing more.
double defaultMMPerUnit(const Layout& layout);

} // namespace pcs
