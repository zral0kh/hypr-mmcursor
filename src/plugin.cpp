// plugin.cpp — Hyprland glue.
//
// ============================================================================
// VERIFIED AGAINST Hyprland 0.56.0
//   commit 36b2e0cfe0c6094dbc47bd42a437431315bb3087
//   ABI    36b2e0c..._aq_0.13_hu_0.14_hg_0.5_hc_0.1_hlg_0.6
//
// Every Hyprland touchpoint below was checked against the installed headers in
// /usr/include/hyprland/src and against the 0.56.0 sources. Where a fact is
// load-bearing, the source location that establishes it is cited inline, so the
// next person can re-check it in seconds instead of rediscovering it.
//
// The parts that matter — geometry.*, layout_build.*, cursor_state.* — have no
// Hyprland dependency at all and are covered by tests. Keep it that way. When
// this file breaks on a Hyprland update, and it will, the fix should be
// pretty much confined to this file.
// ============================================================================

#define WLR_USE_UNSTABLE

#include "apply.hpp"
#include "cursor_state.hpp"
#include "geometry.hpp"
#include "layout_build.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>

#include <format>
#include <optional>
#include <string_view>
#include <type_traits>
#include <string>
#include <unordered_map>
#include <vector>

// Stamped by the Makefile, which derives it from ./VERSION plus the commit, so
// a loaded plugin can say both which release it is and which source built it —
// the question you actually have after a post-update hook rebuilds it for you.
//
// Deliberately NOT a version number here. ./VERSION is the single source of
// truth, matched against the git tag by `make check-version`; a second literal
// in this file is precisely the thing that drifts from the tag and makes a
// GitHub release disagree with what the plugin reports. This fallback is only
// reachable by compiling plugin.cpp without the Makefile.
#ifndef MMCURSOR_VERSION
#define MMCURSOR_VERSION "unknown"
#endif

inline HANDLE PHANDLE = nullptr;

namespace {

// The ABI string we were compiled against, captured at init. Since the guard
// below refuses to load on a mismatch, seeing this at all means it matched the
// running compositor — its value is telling you which Hyprland that was.
std::string g_builtAgainst;

pcs::Layout      g_layout;
pcs::CursorState g_cursor;

bool             g_enabled     = true;
double           g_sensitivity = 1.0;
double           g_gapMM       = 0.0;
pcs::Align       g_align       = pcs::Align::Derive;

// How the last rebuild placed each monitor, for `hyprctl mmcursor`.
pcs::BuildDiagnostics g_diag;

// The logical position we last observed immediately after our own warp.
//
// This is the whole reconcile mechanism; see hkPointerMove(). It is a readback,
// not a prediction — never assign a value here that we merely *asked* for.
Vector2D g_lastSeen = {0, 0};

// -------------------------------------------------------------------------
// Config keyword state
// -------------------------------------------------------------------------
//
// Everything below is repopulated from scratch on every config parse. Hyprlang
// calls our keyword handlers once per matching line, and it has no notion of
// "these lines went away", so holding state across a reload would mean deleting
// a line leaves its effect in place until the compositor restarts. The
// config.preReload listener clears all four; see PLUGIN_INIT.

// Physical size overrides, keyed by monitor name. `mmcursor-monitor`.
// Everything else comes from EDID.
struct Override {
    double mmWidth  = 0.0;
    double mmHeight = 0.0;
    double offsetMM = 0.0; // legacy 4th field, folded into the y offset
};
std::unordered_map<std::string, Override> g_overrides;

// `mmcursor-place`  — absolute mm origins and explicit anchor relations.
std::unordered_map<std::string, pcs::PlacementSpec> g_placements;

// `mmcursor-offset` — 2D nudges applied after placement.
std::unordered_map<std::string, pcs::Vec2> g_offsets;

// `mmcursor-gap`    — per-seam bezels, overriding the global gap_mm.
std::vector<pcs::SeamGap> g_seamGaps;

// Live overrides from `hyprctl mmcursor place|offset`, which deliberately do
// NOT persist: they are cleared by the next config parse, so hyprland.conf
// stays the one source of truth and a tuning session cannot silently become
// your configuration.
std::unordered_map<std::string, pcs::Vec2> g_liveOrigins;
std::unordered_map<std::string, pcs::Vec2> g_liveOffsets;

void clearConfigState() {
    g_overrides.clear();
    g_placements.clear();
    g_offsets.clear();
    g_seamGaps.clear();
    g_liveOrigins.clear();
    g_liveOffsets.clear();
}

CFunctionHook* g_moveHook = nullptr;

// EventBus listeners. These are [[nodiscard]] SPs whose lifetime *is* the
// subscription — drop the pointer and you silently stop receiving events.
struct {
    CHyprSignalListener layoutChanged;
    CHyprSignalListener monitorAdded;
    CHyprSignalListener monitorRemoved;
    CHyprSignalListener configPreReload;
    CHyprSignalListener configReloaded;
} g_listeners;

// -------------------------------------------------------------------------
// Layout sync
// -------------------------------------------------------------------------
//
// Monitor field provenance, all verified in 0.56.0:
//
//   m_position      logical origin.                    output/Monitor.cpp:1211
//   m_size          logical size, ALREADY transformed and scaled:
//                     m_size = (transformedSize / m_scale).round()
//                                                      output/Monitor.cpp:1057
//                   and logicalBox() is exactly {m_position, m_size}
//                                                      output/Monitor.cpp:1766
//                   So this is the rect the cursor actually lives in. Do NOT
//                   reconstruct it from the mode and scale by hand, and do NOT
//                   use m_pixelSize (mode size) or m_transformedSize (unscaled).
//   m_transform     wl_output_transform.               output/Monitor.hpp:95
//   m_output->physicalSize
//                   EDID physical size in mm, in the panel's NATIVE
//                   orientation, {0,0} when unknown.
//                                          aquamarine/output/Output.hpp:179
//
// The enabled-monitor list moved out of CCompositor: it is now
// State::monitorState()->monitors(). allMonitors() is the superset that
// includes disabled outputs — we want monitors().
//                                                      state/MonitorState.cpp:53
void rebuildLayout() {
    std::vector<pcs::MonitorDesc> descs;

    for (const auto& m : State::monitorState()->monitors()) {
        if (!m || !m->enabled() || m->isMirror())
            continue;

        // A monitor mid-configuration can have a zero logical size. Projecting
        // through it would divide by zero.
        if (m->m_size.x <= 0.0 || m->m_size.y <= 0.0)
            continue;

        pcs::MonitorDesc d;
        d.name    = m->m_name;
        d.logical = pcs::Rect{m->m_position.x, m->m_position.y, m->m_size.x, m->m_size.y};

        d.edidMMWidth  = m->m_output ? m->m_output->physicalSize.x : 0.0;
        d.edidMMHeight = m->m_output ? m->m_output->physicalSize.y : 0.0;
        d.transform    = static_cast<int>(m->m_transform);

        if (auto it = g_overrides.find(d.name); it != g_overrides.end()) {
            if (it->second.mmWidth > 0.0)
                d.overrideMMWidth = it->second.mmWidth;
            if (it->second.mmHeight > 0.0)
                d.overrideMMHeight = it->second.mmHeight;
            d.offsetMM.y = it->second.offsetMM;
        }

        if (auto it = g_offsets.find(d.name); it != g_offsets.end())
            d.offsetMM = it->second;
        if (auto it = g_liveOffsets.find(d.name); it != g_liveOffsets.end())
            d.offsetMM = it->second;

        if (auto it = g_placements.find(d.name); it != g_placements.end())
            d.placement = it->second;
        if (auto it = g_liveOrigins.find(d.name); it != g_liveOrigins.end()) {
            pcs::PlacementSpec spec;
            spec.absoluteMM = it->second;
            d.placement     = spec;
        }

        // A panel that reports no physical size and has no override cannot
        // participate. Headless outputs always land here. Bail out entirely
        // rather than half-applying the mapping across a partial layout.
        if (d.overrideMMWidth.value_or(d.edidMMWidth) <= 0.0 || d.overrideMMHeight.value_or(d.edidMMHeight) <= 0.0) {
            HyprlandAPI::addNotification(PHANDLE, "[mmcursor] " + d.name + " has no physical size; set mmcursor-monitor for it. Disabling.",
                                         CHyprColor{1.0F, 0.7F, 0.2F, 1.0F}, 6000);
            g_layout = pcs::Layout{};
            g_cursor.setLayout(nullptr);
            g_cursor.invalidate();
            return;
        }

        descs.push_back(std::move(d));
    }

    pcs::BuildOptions opts;
    opts.align    = g_align;
    opts.gapMM    = g_gapMM;
    opts.seamGaps = g_seamGaps;

    g_diag   = pcs::BuildDiagnostics{};
    g_layout = pcs::buildLayout(std::move(descs), opts, &g_diag);

    // Best-effort placement is still placement, but it is a guess and the user
    // is the only one who can correct it. Cap the toasts — the full list is
    // always in `hyprctl mmcursor`.
    for (std::size_t i = 0; i < g_diag.warnings.size() && i < 3; ++i)
        HyprlandAPI::addNotification(PHANDLE, "[mmcursor] " + g_diag.warnings[i], CHyprColor{1.0F, 0.7F, 0.2F, 1.0F}, 6000);
    if (g_diag.warnings.size() > 3)
        HyprlandAPI::addNotification(PHANDLE, std::format("[mmcursor] {} more placement warnings; see `hyprctl mmcursor`.", g_diag.warnings.size() - 3),
                                     CHyprColor{1.0F, 0.7F, 0.2F, 1.0F}, 6000);

    // Overlapping mm rects mean two panels claim the same desk space. Which one
    // a point belongs to then comes down to declaration order, and the cursor
    // teleports between them for reasons nothing in the config explains. Refuse
    // rather than project through whichever rect happened to be listed first.
    if (const auto CLASH = g_layout.firstMMOverlap()) {
        HyprlandAPI::addNotification(PHANDLE, std::format("[mmcursor] {} and {} overlap in mm space; check mmcursor-monitor. Disabling.", CLASH->first, CLASH->second),
                                     CHyprColor{1.0F, 0.7F, 0.2F, 1.0F}, 8000);
        g_layout = pcs::Layout{};
        g_cursor.setLayout(nullptr);
        g_cursor.invalidate();
        return;
    }

    g_cursor.setLayout(&g_layout);
    g_cursor.setMMPerUnit(pcs::defaultMMPerUnit(g_layout) * g_sensitivity);

    // The layout moved under us, so whatever mm position we were holding is now
    // meaningless. Adopt the compositor's current cursor instead of trusting
    // ourselves — it is authoritative here, we are not.
    g_cursor.invalidate();

    if (!g_layout.empty()) {
        const auto POS = Pointer::mgr()->position();
        g_cursor.reconcile({POS.x, POS.y});
        g_lastSeen = POS;
    }
}

// -------------------------------------------------------------------------
// The hook — the one arrow: delta -> new global cursor position
// -------------------------------------------------------------------------
//
// Hook site: Pointer::CPointerManager::move(const Vector2D& deltaLogical)
//            pointer/PointerManager.cpp:831
//
// Why this site and not CInputManager::onMouseMoved or mouseMoveUnified:
//
//   * It receives a pure logical delta and nothing else. That is exactly the
//     arrow this plugin is allowed to touch.
//
//   * It sits DOWNSTREAM of relative-pointer dispatch. In onMouseMoved,
//                        managers/input/InputManager.cpp:154   sendRelativeMotion(delta, unaccel)
//                        managers/input/InputManager.cpp:155   Pointer::mgr()->move(DELTA)
//     the protocol send happens first. So pointer-locked clients have already
//     been handed the untouched libinput delta before we are called, and the
//     "never mutate the delta" invariant holds *by construction* — not because
//     anything here is careful. A game cannot see our correction even in
//     principle.
//
//   * The delta is ACCELERATED. onMouseMoved picks unaccel only when
//     input:force_no_accel is set (InputManager.cpp:146) and hands the result
//     down. libinput's profile is therefore already applied and we only scale
//     by a constant, which preserves the shape of its curve. mmPerUnit is a
//     speed knob and nothing more.
//
//   * It is upstream of all clamping. move() computes newPos and calls
//     warpTo(), and warpTo() is what runs closestValid()
//     (PointerManager.cpp:818-829). Hooking mouseMoveUnified instead would hand
//     us a position Hyprland had already computed AND already clamped — the
//     wrong cross-seam decision already made, and a delta to reverse-engineer
//     in order to undo work just done.
//
// Why NOT the EventBus:
//
//   Event::bus()->m_events.input.mouse.move is Cancellable<Vector2D>, which
//   looks perfect and is not. It emits MOUSECOORDSFLOORED — an absolute,
//   floored position — from inside mouseMoveUnified
//                                        managers/input/InputManager.cpp:269
//   It is the verdict, not the delta, and it is floored on top. There is no
//   event carrying relative motion, so the function hook is forced. It is the
//   single thing in this repo most likely to break on a Hyprland update; that
//   is why it is four lines and why they are all here.
//
// How we apply the result: we do NOT warp directly. We rewrite the delta to
// (target - current) and call the original. The original then computes
// oldPos + (target - oldPos) == target and proceeds normally, so we inherit its
// NaN guard, its input-capture handling (PointerManager.cpp:835-839), warpTo,
// clamping, damage and focus for free. Calling warpTo() ourselves would skip
// all of that.
//
// Note this parameter is NOT the event's delta — that one is long gone. This is
// Hyprland's internal "advance the cursor by this much" instruction, and
// rewriting it is precisely equivalent to setting a position.

using origPointerMove = void (*)(void*, const Vector2D&);

void callOriginal(void* thisptr, const Vector2D& delta) {
    (*(origPointerMove)g_moveHook->m_original)(thisptr, delta);
}

void hkPointerMove(void* thisptr, const Vector2D& deltaLogical) {
    if (!g_enabled || g_layout.empty()) {
        callOriginal(thisptr, deltaLogical);
        return;
    }

    const Vector2D CURRENT = Pointer::mgr()->position();

    // ---- Reconcile and correct -------------------------------------------
    //
    // All of the decision-making lives in pcs::planMotion, which is pure and
    // tested (tests/test_apply.cpp simulates whole sessions, including the
    // compositor's own clamping and third-party warps). What is left here is
    // reading two observable values, calling it, and writing the result back.
    // Resist the urge to inline "just this one condition" — that is how the
    // logic ended up untestable the first time.
    //
    // Why a pull rather than a push (a
    // reentrancy flag plus a hook on every absolute-motion path): m_pointerPos
    // is written in exactly three places in 0.56.0 —
    //     warpTo()        PointerManager.cpp:821
    //     warpAbsolute()  PointerManager.cpp:915-918, 937
    // — and every one is observable through position(). Comparing against a
    // readback therefore catches dispatchers, tablet and touch, pointer-lock
    // and confinement handoff, client warps via the pointer-warp protocol,
    // workspace and window focus warps, and anything added in a future
    // release, without naming any of them and without a flag to forget.
    //
    // Non-finite deltas are handled inside CursorState::applyRelative, upstream
    // of the original's own NaN guard (PointerManager.cpp:833). Do not add a
    // second guard here: there would then be two, and only one would be tested.
    const auto PLAN = pcs::planMotion(g_cursor, {CURRENT.x, CURRENT.y}, {g_lastSeen.x, g_lastSeen.y}, {deltaLogical.x, deltaLogical.y});

    if (PLAN.correctedDelta)
        callOriginal(thisptr, Vector2D{PLAN.correctedDelta->x, PLAN.correctedDelta->y});
    else
        callOriginal(thisptr, deltaLogical); // no usable state; stock behaviour beats a guess

    // Read back what actually landed, rather than assuming we got what we asked
    // for. warpTo() runs the position through closestValid()
    // (PointerManager.cpp:721-796), which perturbs points whose hotspot box
    // straddles a seam. Storing the request here instead would report a phantom
    // external move on every event near an edge, and re-reconcile constantly.
    g_lastSeen = Pointer::mgr()->position();
}

// -------------------------------------------------------------------------
// Config
// -------------------------------------------------------------------------
//
//   plugin {
//       mmcursor {
//           enabled     = true
//           sensitivity = 1.0
//           gap_mm      = 0.0
//
//           # derive (default) reads each seam's alignment out of the layout
//           # Hyprland actually has active. top/center/bottom force every seam
//           # instead, for when that layout is itself wrong.
//           align       = derive
//
//           # name, physical width mm, physical height mm [, vertical offset mm]
//           # width/height are in the panel's NATIVE orientation; rotation is
//           # applied for you. 0 means "trust EDID".
//           mmcursor-monitor = DP-9,  600, 340
//           mmcursor-monitor = DP-10, 530, 300
//
//           # Placement is derived from the active layout, so none of the rest
//           # is normally needed. Each one overrides that derivation.
//           mmcursor-place  = DP-10, at, 620, -95      # absolute mm origin
//           mmcursor-place  = DP-11, below, DP-9, left, 12
//           mmcursor-gap    = DP-9, DP-10, 22          # this seam's bezel, mm
//           mmcursor-offset = DP-10, 0, -4             # 2D nudge, mm
//       }
//   }
//
// None of this requires rebuilding the plugin: Hyprland watches hyprland.conf
// itself, and the config.reloaded listener below re-reads and rebuilds. Saving
// the file is enough.

std::vector<std::string> splitFields(const char* value) {
    std::string              v{value ? value : ""};
    std::vector<std::string> parts;
    size_t                   pos = 0;
    while (true) {
        const size_t comma = v.find(',', pos);
        std::string  tok   = v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t b     = tok.find_first_not_of(" \t");
        const size_t e     = tok.find_last_not_of(" \t");
        parts.push_back(b == std::string::npos ? "" : tok.substr(b, e - b + 1));
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return parts;
}

// Alignment words. top/bottom read naturally at a horizontal seam and
// left/right at a vertical one; both spellings map to the same axis-agnostic
// Start/End, so either is accepted wherever an alignment is expected.
std::optional<pcs::Align> parseAlign(const std::string& s) {
    if (s == "derive")
        return pcs::Align::Derive;
    if (s == "top" || s == "left")
        return pcs::Align::Start;
    if (s == "center" || s == "centre")
        return pcs::Align::Center;
    if (s == "bottom" || s == "right")
        return pcs::Align::End;
    return std::nullopt;
}

std::optional<pcs::Edge> parseEdge(const std::string& s) {
    if (s == "right-of")
        return pcs::Edge::RightOf;
    if (s == "left-of")
        return pcs::Edge::LeftOf;
    if (s == "above")
        return pcs::Edge::Above;
    if (s == "below")
        return pcs::Edge::Below;
    return std::nullopt;
}

Hyprlang::CParseResult onMonitorKeyword(const char* /*command*/, const char* value) {
    Hyprlang::CParseResult result;

    const std::vector<std::string> parts = splitFields(value);

    if (parts.size() < 3) {
        result.setError("mmcursor-monitor needs: name, mm_width, mm_height [, offset_mm]");
        return result;
    }

    Override o;
    try {
        o.mmWidth  = std::stod(parts[1]);
        o.mmHeight = std::stod(parts[2]);
        if (parts.size() >= 4 && !parts[3].empty())
            o.offsetMM = std::stod(parts[3]);
    } catch (...) {
        result.setError("mmcursor-monitor: could not parse a number");
        return result;
    }

    g_overrides[parts[0]] = o;
    return result;
}

//   mmcursor-place = NAME, at, X_MM, Y_MM
//   mmcursor-place = NAME, right-of|left-of|above|below, ANCHOR [, align] [, offset_mm]
Hyprlang::CParseResult onPlaceKeyword(const char* /*command*/, const char* value) {
    Hyprlang::CParseResult result;

    const std::vector<std::string> parts = splitFields(value);
    if (parts.size() < 3) {
        result.setError("mmcursor-place needs: NAME, at, x_mm, y_mm  |  NAME, right-of|left-of|above|below, ANCHOR [, align] [, offset_mm]");
        return result;
    }

    pcs::PlacementSpec spec;

    if (parts[1] == "at") {
        if (parts.size() < 4) {
            result.setError("mmcursor-place ... at needs both x_mm and y_mm");
            return result;
        }
        try {
            spec.absoluteMM = pcs::Vec2{std::stod(parts[2]), std::stod(parts[3])};
        } catch (...) {
            result.setError("mmcursor-place: could not parse the mm origin");
            return result;
        }
    } else {
        const auto EDGE = parseEdge(parts[1]);
        if (!EDGE) {
            result.setError("mmcursor-place: expected `at`, `right-of`, `left-of`, `above` or `below`");
            return result;
        }
        if (parts[2].empty()) {
            result.setError("mmcursor-place: a relation needs an anchor monitor");
            return result;
        }
        spec.edge   = *EDGE;
        spec.anchor = parts[2];

        if (parts.size() >= 4 && !parts[3].empty()) {
            const auto ALIGN = parseAlign(parts[3]);
            if (!ALIGN) {
                result.setError("mmcursor-place: alignment must be derive, top/left, center or bottom/right");
                return result;
            }
            spec.align = *ALIGN;
        }
        if (parts.size() >= 5 && !parts[4].empty()) {
            try {
                spec.offsetMM = std::stod(parts[4]);
            } catch (...) {
                result.setError("mmcursor-place: could not parse the offset");
                return result;
            }
        }
    }

    // Placing a monitor on itself would make it its own anchor and it would
    // never be placeable — caught here rather than as a mystery fallback later.
    if (spec.anchor == parts[0]) {
        result.setError("mmcursor-place: a monitor cannot be its own anchor");
        return result;
    }

    g_placements[parts[0]] = spec;
    return result;
}

//   mmcursor-gap = A, B, MM
Hyprlang::CParseResult onGapKeyword(const char* /*command*/, const char* value) {
    Hyprlang::CParseResult result;

    const std::vector<std::string> parts = splitFields(value);
    if (parts.size() < 3) {
        result.setError("mmcursor-gap needs: monitor_a, monitor_b, mm");
        return result;
    }

    pcs::SeamGap g;
    g.a = parts[0];
    g.b = parts[1];
    try {
        g.mm = std::stod(parts[2]);
    } catch (...) {
        result.setError("mmcursor-gap: could not parse a number");
        return result;
    }
    if (g.mm < 0.0) {
        result.setError("mmcursor-gap: a bezel cannot be negative");
        return result;
    }

    std::erase_if(g_seamGaps, [&](const pcs::SeamGap& e) { return (e.a == g.a && e.b == g.b) || (e.a == g.b && e.b == g.a); });
    g_seamGaps.push_back(g);
    return result;
}

//   mmcursor-offset = NAME, DX_MM, DY_MM
Hyprlang::CParseResult onOffsetKeyword(const char* /*command*/, const char* value) {
    Hyprlang::CParseResult result;

    const std::vector<std::string> parts = splitFields(value);
    if (parts.size() < 3) {
        result.setError("mmcursor-offset needs: name, dx_mm, dy_mm");
        return result;
    }

    try {
        g_offsets[parts[0]] = pcs::Vec2{std::stod(parts[1]), std::stod(parts[2])};
    } catch (...) {
        result.setError("mmcursor-offset: could not parse a number");
        return result;
    }
    return result;
}

// getConfigValue is gated only on the config being the legacy hyprland.conf
// kind, not on plugin-init state (plugins/PluginAPI.cpp:212-225), so it is safe
// to call from a reload callback. It returns nullptr under a Lua config.
template <typename T>
bool readConfig(const char* name, T& out) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    auto* const VAL = HyprlandAPI::getConfigValue(PHANDLE, name);
#pragma GCC diagnostic pop
    if (!VAL)
        return false;

    void* const* const SLOT = VAL->getDataStaticPtr();
    if (!SLOT)
        return false;

    // Hyprlang is asymmetric here and it is not documented anywhere obvious:
    // for a STRING the slot holds the char pointer ITSELF (one dereference),
    // while for INT/FLOAT it holds a pointer to the number (two). Hyprland's own
    // code puts the two forms on adjacent lines, which is the clearest statement
    // of it that exists:
    //
    //     *rc<Hyprlang::STRING const*>(VAL)     debug/HyprCtl.cpp:1687
    //     **rc<Config::STRING* const*>(VAL)     debug/HyprCtl.cpp:1689
    //
    // Using two dereferences for a STRING reads the first 8 bytes of the string's
    // characters as a pointer. It does not fail here — it fails later, inside
    // std::string's constructor, with a backtrace that points nowhere near the
    // cause. This cost a compositor crash to find; leave the branch alone.
    if constexpr (std::is_same_v<T, Hyprlang::STRING>) {
        out = *reinterpret_cast<Hyprlang::STRING const*>(SLOT);
        return out != nullptr;
    } else {
        auto* const* const PTR = reinterpret_cast<T* const*>(SLOT);
        if (!*PTR)
            return false;
        out = **PTR;
        return true;
    }
}

void reloadConfigValues() {
    Hyprlang::INT enabled = 1;
    if (readConfig("plugin:mmcursor:enabled", enabled))
        g_enabled = enabled != 0;

    Hyprlang::FLOAT sensitivity = 1.0F;
    if (readConfig("plugin:mmcursor:sensitivity", sensitivity))
        g_sensitivity = sensitivity > 0.0F ? static_cast<double>(sensitivity) : 1.0;

    Hyprlang::FLOAT gap = 0.0F;
    if (readConfig("plugin:mmcursor:gap_mm", gap))
        g_gapMM = gap >= 0.0F ? static_cast<double>(gap) : 0.0;

    // Not our value, but it silently defeats part of what we do, so it is worth
    // saying out loud. hotspot_padding holds the cursor N logical px inside the
    // layout, while our mm clamp stops at the true panel edge. mm then describes
    // a position the cursor may not occupy, and walking back off an edge wastes
    // up to N px of travel — the exact hysteresis this plugin removes everywhere
    // else. Default is 0, so this normally never fires.
    // tests/test_apply.cpp pins the bounded version of this behaviour.
    Hyprlang::INT padding = 0;
    if (readConfig("cursor:hotspot_padding", padding) && padding != 0) {
        HyprlandAPI::addNotification(
            PHANDLE, std::format("[mmcursor] cursor:hotspot_padding is {}; this reintroduces up to {}px of dead travel at screen edges. Set it to 0.", padding, padding),
            CHyprColor{1.0F, 0.7F, 0.2F, 1.0F}, 7000);
    }

    Hyprlang::STRING align = nullptr;
    if (readConfig("plugin:mmcursor:align", align) && align)
        g_align = parseAlign(std::string{align}).value_or(pcs::Align::Derive);
}

// -------------------------------------------------------------------------
// Debug surface
// -------------------------------------------------------------------------
//
//   hyprctl mmcursor
//
// A hyprctl command rather than a dispatcher, because a dispatcher can only
// return success/error (SDispatchResult, SharedDefs.hpp:52) whereas this hands
// text back to the terminal.
// One line, so a pasted dump identifies itself. Not a separate subcommand: a
// version you have to ask for separately is a version nobody includes in the
// bug report.
std::string versionLine() {
    return std::format("mmcursor {}  (built against {})\n", MMCURSOR_VERSION, g_builtAgainst.empty() ? "?" : g_builtAgainst);
}

std::string debugDump() {
    std::string out;

    out += versionLine();
    out += std::format("mmcursor: {}\n", g_enabled ? "enabled" : "disabled");
    out += std::format("mm per input unit: {:.6f}  (sensitivity {:.3f})\n", g_cursor.mmPerUnit(), g_sensitivity);
    const char* ALIGN_NAME = g_align == pcs::Align::Start ? "top/left" : (g_align == pcs::Align::End ? "bottom/right" : (g_align == pcs::Align::Center ? "center" : "derive"));
    out += std::format("gap: {:.2f} mm    align: {}\n", g_gapMM, ALIGN_NAME);
    if (!g_seamGaps.empty()) {
        for (const auto& g : g_seamGaps)
            out += std::format("  seam {} <-> {}: {:.2f} mm\n", g.a, g.b, g.mm);
    }

    if (g_layout.empty()) {
        out += "\nlayout: EMPTY — plugin is inert.\n";
        return out;
    }

    out += std::format("\nmm position: {}", g_cursor.valid() ? std::format("{:.2f}, {:.2f}", g_cursor.positionMM().x, g_cursor.positionMM().y) : "INVALID");

    const auto POS = Pointer::mgr()->position();
    out += std::format("\ncompositor cursor: {:.2f}, {:.2f}", POS.x, POS.y);
    out += std::format("\nlast readback: {:.2f}, {:.2f}{}\n", g_lastSeen.x, g_lastSeen.y, POS != g_lastSeen ? "   (external move pending reconcile)" : "");

    out += "\nmonitors:\n";
    for (const auto& m : g_layout.monitors()) {
        out += std::format("  {:<10} mm [{:7.2f} {:7.2f}  {:7.2f}x{:7.2f}]  logical [{:6.0f} {:6.0f}  {:5.0f}x{:5.0f}]  {:.4f} x {:.4f} px/mm", m.name, m.mm.x, m.mm.y,
                           m.mm.w, m.mm.h, m.logical.x, m.logical.y, m.logical.w, m.logical.h, m.pxPerMMx(), m.pxPerMMy());

        // How this monitor got where it is. Without this a wrong layout is a
        // theory you form by moving the mouse around; with it, it is a line.
        for (const auto& p : g_diag.placements) {
            if (p.name != m.name)
                continue;
            out += "  <- " + p.how;
            if (!p.anchor.empty())
                out += " " + p.anchor;
            if (p.residualMM.x != 0.0 || p.residualMM.y != 0.0)
                out += std::format(" residual {:+.2f},{:+.2f}mm", p.residualMM.x, p.residualMM.y);
            break;
        }
        out += "\n";
    }

    if (!g_diag.warnings.empty()) {
        out += "\nwarnings:\n";
        for (const auto& w : g_diag.warnings)
            out += "  " + w + "\n";
    }

    if (!g_liveOrigins.empty() || !g_liveOffsets.empty())
        out += "\nlive overrides are active (not persisted; the next config reload drops them)\n";

    return out;
}

// `hyprctl mmcursor [reload | place NAME X Y | offset NAME DX DY]`
//
// reload exists because `hyprctl keyword` applies a single value without
// re-parsing the file, and so may never emit config.reloaded. place/offset
// exist because positioning a monitor is a tape-measure job: nudge, look,
// nudge. They deliberately do not persist — hyprland.conf stays the source of
// truth and the next reload drops them.
std::string ctlCommand(eHyprCtlOutputFormat /*format*/, std::string args) {
    // With exact=false the dispatcher may hand us the whole request, command
    // word and all. Drop a leading "mmcursor" so both forms parse the same.
    std::vector<std::string> tok;
    for (size_t i = 0; i < args.size();) {
        const size_t b = args.find_first_not_of(" \t", i);
        if (b == std::string::npos)
            break;
        const size_t e = args.find_first_of(" \t", b);
        tok.push_back(args.substr(b, e == std::string::npos ? std::string::npos : e - b));
        i = e == std::string::npos ? args.size() : e;
    }
    if (!tok.empty() && tok.front() == "mmcursor")
        tok.erase(tok.begin());

    if (tok.empty())
        return debugDump();

    // For scripting. `hyprctl plugin list` is the idiomatic place to read a
    // plugin's version — this exists so a post-update hook can check what it
    // rebuilt without parsing that.
    if (tok[0] == "version")
        return versionLine();

    if (tok[0] == "reload") {
        reloadConfigValues();
        rebuildLayout();
        return "mmcursor: config re-read and layout rebuilt\n";
    }

    const bool isPlace  = tok[0] == "place";
    const bool isOffset = tok[0] == "offset";
    if (isPlace || isOffset) {
        if (tok.size() < 4)
            return std::format("usage: hyprctl mmcursor {} NAME X_MM Y_MM\n", tok[0]);
        double x = 0.0, y = 0.0;
        try {
            x = std::stod(tok[2]);
            y = std::stod(tok[3]);
        } catch (...) { return "mmcursor: could not parse a number\n"; }

        if (isPlace)
            g_liveOrigins[tok[1]] = pcs::Vec2{x, y};
        else
            g_liveOffsets[tok[1]] = pcs::Vec2{x, y};

        rebuildLayout();
        return debugDump();
    }

    return "usage: hyprctl mmcursor [version | reload | place NAME X_MM Y_MM | offset NAME DX_MM DY_MM]\n";
}

SP<SHyprCtlCommand> g_ctlCommand;

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // ABI guard. Hyprland itself only checks PLUGIN_API_VERSION, a string that
    // has been "0.1" for years — it does NOT compare hashes, so this is the
    // plugin's job and it is the difference between "refuses to load" and
    // "corrupts memory in the input path".
    //
    // Compare the two ABI *strings*, not commit hashes:
    //   __hyprland_api_get_hash()         resolved from the Hyprland binary at
    //                                     dlopen — the server's ABI string
    //   __hyprland_api_get_client_hash()  inline in PluginAPI.hpp, baked in from
    //                                     the headers WE compiled against
    // Both look like <commit>_aq_0.14_hu_0.14_hg_0.5_hc_0.1_hlg_0.6, so this
    // catches a bumped aquamarine or hyprutils as well as a bumped Hyprland.
    //
    // Do not compare against getHyprlandVersion().hash: that returns a bare
    // GIT_COMMIT_HASH, which can never equal the full ABI string, so the guard
    // fires on every load and the plugin never runs. (It did exactly that.)
    const std::string_view SERVER_ABI = __hyprland_api_get_hash();
    const std::string_view CLIENT_ABI = __hyprland_api_get_client_hash();
    g_builtAgainst                    = std::string{CLIENT_ABI};
    if (SERVER_ABI != CLIENT_ABI) {
        HyprlandAPI::addNotification(PHANDLE, std::format("[mmcursor] ABI mismatch; refusing to load.\n  compositor: {}\n  plugin:     {}", SERVER_ABI, CLIENT_ABI),
                                     CHyprColor{1.0F, 0.2F, 0.2F, 1.0F}, 10000);
        throw std::runtime_error("[mmcursor] ABI mismatch");
    }

    // addConfigValue/addConfigKeyword are only permitted while
    // m_allowConfigVars is true, and that flag is set exclusively around the
    // call to pluginInit (plugins/PluginSystem.cpp:115-128). So these MUST
    // happen here and can never be deferred.
    //
    // These four are [[deprecated]] in favour of addConfigValueV2. We use them
    // deliberately. V2 covers config *values* but there is no V2 equivalent for
    // config *keywords*, and `mmcursor-monitor` has to be a keyword because it
    // is repeated once per monitor. Mixing the two would leave the values
    // working under a Lua config while the physical-size overrides silently did
    // not — a half-configured layout that mis-maps monitors without saying so,
    // which is the one failure mode this plugin must never have. One config
    // system, and it fails loudly (below) if unavailable.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:mmcursor:enabled", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:mmcursor:sensitivity", Hyprlang::FLOAT{1.0F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:mmcursor:gap_mm", Hyprlang::FLOAT{0.0F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:mmcursor:align", Hyprlang::STRING{"derive"});
    HyprlandAPI::addConfigKeyword(PHANDLE, "mmcursor-monitor", onMonitorKeyword, Hyprlang::SHandlerOptions{});
    HyprlandAPI::addConfigKeyword(PHANDLE, "mmcursor-place", onPlaceKeyword, Hyprlang::SHandlerOptions{});
    HyprlandAPI::addConfigKeyword(PHANDLE, "mmcursor-gap", onGapKeyword, Hyprlang::SHandlerOptions{});
    HyprlandAPI::addConfigKeyword(PHANDLE, "mmcursor-offset", onOffsetKeyword, Hyprlang::SHandlerOptions{});

    // Both the value and keyword APIs bail out unless the config is the legacy
    // hyprland.conf kind (plugins/PluginAPI.cpp:180, :199), and getConfigValue
    // returns nullptr in that case (:215). So this doubles as a config-type
    // probe: no value back means a Lua config, which means our overrides would
    // never be readable. Refuse rather than run with an EDID-only layout the
    // user cannot correct.
    if (!HyprlandAPI::getConfigValue(PHANDLE, "plugin:mmcursor:enabled")) {
        HyprlandAPI::addNotification(PHANDLE, "[mmcursor] requires a legacy hyprland.conf config (Lua configs are unsupported); refusing to load.",
                                     CHyprColor{1.0F, 0.2F, 0.2F, 1.0F}, 8000);
        throw std::runtime_error("[mmcursor] unsupported config backend");
    }
#pragma GCC diagnostic pop

    // HyprlandAPI::registerCallbackDynamic is a no-op that returns nullptr in
    // 0.56.0 — its body is commented out (plugins/PluginAPI.cpp:36-45). The
    // EventBus is the only working path. Listeners are [[nodiscard]] and the
    // subscription dies with the returned pointer, so these must be stored.
    g_listeners.layoutChanged  = Event::bus()->m_events.monitor.layoutChanged.listen([] { rebuildLayout(); });
    g_listeners.monitorAdded   = Event::bus()->m_events.monitor.added.listen([](PHLMONITOR) { rebuildLayout(); });
    g_listeners.monitorRemoved = Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR) { rebuildLayout(); });
    // Hyprlang calls a keyword handler once per matching line and has no way to
    // tell us a line was deleted, so accumulated keyword state has to be thrown
    // away before each parse — otherwise removing an `mmcursor-place` line
    // leaves it in force until the compositor restarts. preReload is the only
    // hook that fires early enough to do that.
    g_listeners.configPreReload = Event::bus()->m_events.config.preReload.listen([] { clearConfigState(); });

    g_listeners.configReloaded = Event::bus()->m_events.config.reloaded.listen([] {
        reloadConfigValues();
        rebuildLayout();
    });

    // exact=false so `hyprctl mmcursor reload` and friends reach us rather than
    // failing to match the bare command name.
    g_ctlCommand = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{"mmcursor", false, ctlCommand});

    // The function hook. x86_64 only, and the most fragile thing here by a wide
    // margin — see the block comment above hkPointerMove for why there is no
    // alternative.
    //
    // We search for the MANGLED name, not the demangled one, because that is
    // what findFunctionsByName actually matches on: it shells out to
    // `nm -D -j` and tests `line.contains(name)` against the mangled symbol,
    // resolving the address with dlsym (plugins/PluginAPI.cpp:392-406). Passing
    // "move" would scan 134 unrelated symbols and make us depend on the
    // demangler's exact output formatting for disambiguation. The full mangled
    // name matches exactly one line and nothing else.
    //
    // Note this makes binutils (`nm`) a runtime dependency of loading any
    // Hyprland plugin that hooks. It is present on any normal Arch install.
    static constexpr const char* MOVE_SYMBOL = "_ZN7Pointer15CPointerManager4moveERKN9Hyprutils4Math8Vector2DE";

    static const auto METHODS = HyprlandAPI::findFunctionsByName(PHANDLE, MOVE_SYMBOL);

    if (METHODS.size() != 1) {
        HyprlandAPI::addNotification(PHANDLE, std::format("[mmcursor] expected exactly 1 match for CPointerManager::move, found {}; refusing to load.", METHODS.size()),
                                     CHyprColor{1.0F, 0.2F, 0.2F, 1.0F}, 8000);
        throw std::runtime_error("[mmcursor] motion hook symbol not found");
    }

    g_moveHook = HyprlandAPI::createFunctionHook(PHANDLE, METHODS[0].address, (void*)&hkPointerMove);
    if (!g_moveHook || !g_moveHook->hook()) {
        HyprlandAPI::addNotification(PHANDLE, "[mmcursor] failed to install the motion hook; refusing to load.", CHyprColor{1.0F, 0.2F, 0.2F, 1.0F}, 8000);
        throw std::runtime_error("[mmcursor] motion hook install failed");
    }

    // The config has not been parsed with our values in it yet — PluginSystem
    // queues Config::mgr()->reload() right after pluginInit returns
    // (plugins/PluginSystem.cpp:135), which fires configReloaded and gets us
    // real values plus a rebuild. Read whatever defaults exist now so we are
    // not inert in the gap.
    reloadConfigValues();
    rebuildLayout();

    // This is what `hyprctl plugin list` prints, and it is the idiomatic place
    // for a plugin's version — hence no separate --version flag.
    return {"mmcursor", "Cursor motion in physical millimetres across mismatched-density monitors", "you", MMCURSOR_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Drop the subscriptions explicitly. Hyprland tears down hooks, config
    // values and hyprctl commands for us, but these listeners live in our own
    // globals and would otherwise be destroyed after our code is unmapped.
    g_listeners = {};

    g_cursor.setLayout(nullptr);
    g_cursor.invalidate();
    g_layout = pcs::Layout{};
    g_diag   = pcs::BuildDiagnostics{};
    clearConfigState();
}
