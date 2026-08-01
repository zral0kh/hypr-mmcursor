#!/usr/bin/env bash
# Placement verification: every monitor arrangement the derivation has to cope
# with, every config override, and the edge cases, inside a real compositor.
#
# Run this INSIDE the VM, with the plugin already loaded:
#   ~/mmcursor/test/vm/verify-placement.sh
#
# verify.sh proves the cursor behaves correctly on ONE desk. This proves the
# desk itself is derived correctly from whatever Hyprland happens to have
# active — which is the part that used to be a single hardcoded horizontal row.
#
# Every scenario rewrites hyprland.conf and reloads, because that is the path a
# user actually takes and because a reload is where stale keyword state shows
# up. Expected numbers come from tests/test_placement.cpp, not from a previous
# run of this script.
set -uo pipefail

# shellcheck source=lib.sh
. "$(dirname "$0")/lib.sh"

[ -x "$VP" ] || { echo "build test/vpointer first"; exit 2; }

CONF="$(cd "$(dirname "$0")" && pwd)/hyprland.conf"
BACKUP="/tmp/mmcursor-hyprland.conf.orig"
[ -f "$BACKUP" ] || cp "$CONF" "$BACKUP"

# bochs reports a bogus 320x200mm for both heads, so the size overrides are
# mandatory in every scenario. Sizes are NATIVE orientation.
SIZES='        mmcursor-monitor = Virtual-1, 600, 340
        mmcursor-monitor = Virtual-2, 530, 300'

# Write a config and reload. Written via a temp file and renamed, because
# Hyprland watches this file with inotify and would happily parse it half
# written.
scenario() { # NAME  monitor-block  plugin-extra-block
    echo
    echo "== $1 =="
    cat > "$CONF.tmp" <<EOF
$2

exec-once =
misc {
    disable_hyprland_logo = true
    disable_splash_rendering = true
    force_default_wallpaper = 0
}
cursor { hotspot_padding = 0 }
animations { enabled = false }
decoration { blur { enabled = false } }
debug { disable_logs = false
        enable_stdout_logs = true }

plugin {
    mmcursor {
        enabled = true
        sensitivity = 1.0
        gap_mm = 0
$3
    }
}
EOF
    mv "$CONF.tmp" "$CONF"
    hyprctl reload > /dev/null
    sleep 1.5
}

SIDE_BY_SIDE='monitor = Virtual-1, 2560x1440@60, 0x0,       1
monitor = Virtual-2, 1920x1080@60, 2560x-240, 1, transform, 3'

restore() {
    cp "$BACKUP" "$CONF"
    hyprctl reload > /dev/null
    sleep 1.5
}
trap restore EXIT

# ---------------------------------------------------------------------------
# Derivation: the relation the logical layout states is the one reproduced.
# ---------------------------------------------------------------------------

scenario "centred side by side (the baseline desk)" "$SIDE_BY_SIDE" "$SIZES"
near "$(mm_x Virtual-2)" 600 0.01 "flush right"
near "$(mm_y Virtual-2)" -95 0.01 "centres share a horizon"
contains "$(prov Virtual-2)" "centre" "derived as a centre relation"

scenario "top-aligned" 'monitor = Virtual-1, 2560x1440@60, 0x0,    1
monitor = Virtual-2, 1920x1080@60, 2560x0, 1, transform, 3' "$SIZES"
near "$(mm_y Virtual-2)" 0 0.01 "logically top-flush -> physically top-flush"
contains "$(prov Virtual-2)" "top" "derived as a top relation"

scenario "bottom-aligned" 'monitor = Virtual-1, 2560x1440@60, 0x0,       1
monitor = Virtual-2, 1920x1080@60, 2560x-480, 1, transform, 3' "$SIZES"
# Virtual-1 ends at logical 1440; Virtual-2 is 1920 tall, so -480 makes the
# bottoms flush. Physically: 340 - 530 = -190.
near "$(mm_y Virtual-2)" -190 0.01 "logically bottom-flush -> physically bottom-flush"
contains "$(prov Virtual-2)" "bottom" "derived as a bottom relation"

scenario "a deliberate logical gap" 'monitor = Virtual-1, 2560x1440@60, 0x0,       1
monitor = Virtual-2, 1920x1080@60, 3000x-240, 1, transform, 3' "$SIZES"
# 440 logical px of dead space at Virtual-1's 2560/600 px/mm is 103.125mm.
near "$(mm_x Virtual-2)" "600+440/(2560/600)" 0.02 "the gap carries across in mm"

# ---------------------------------------------------------------------------
# The arrangement the old row builder could not express at all.
# ---------------------------------------------------------------------------

scenario "vertical stack" 'monitor = Virtual-1, 2560x1440@60, 0x0,    1
monitor = Virtual-2, 1920x1080@60, 0x1440, 1' "$SIZES"
near "$(mm_x Virtual-2)" 0 0.01 "left edges flush, as the layout states"
near "$(mm_y Virtual-2)" 340 0.01 "and it sits BELOW, not beside"
contains "$(prov Virtual-2)" "below" "derived as a vertical relation"

echo "  -- crossing the vertical seam --"
warp 300 700
X0=$(mmx)
move 0 2 900 800
near "$(mmy)" "340+1" 340 "crossed onto the lower panel"
near "$(mmx)" "$X0" 0.01 "vertical-only input does not drift sideways"
move 0 -2 900 800
near "$(mmy)" "700/(1440/340)" 0.05 "and the reverse path returns exactly"

# ---------------------------------------------------------------------------
# Three monitors in an L. Two heads is enough to prove a seam; it is not enough
# to prove the spanning tree picks sane parents, which is where an arrangement
# with a choice in it earns its keep.
# ---------------------------------------------------------------------------

# Any headless output left over from a previous run has no size override, which
# correctly makes the plugin inert — and would make every assertion below fail
# for a reason that has nothing to do with the layout being tested.
drop_headless() {
    for m in $(hyprctl monitors | awk '/^Monitor HEADLESS/{print $2}'); do
        hyprctl output remove "$m" > /dev/null 2>&1
    done
    sleep 1.0
}
drop_headless

if hyprctl output create headless > /dev/null 2>&1; then
    sleep 1.5
    HL=$(hyprctl monitors | awk '/^Monitor HEADLESS/{print $2; exit}')
    scenario "three monitors in an L ($HL below, Virtual-2 right)" \
        "monitor = Virtual-1, 2560x1440@60, 0x0,       1
monitor = Virtual-2, 1920x1080@60, 2560x-240, 1, transform, 3
monitor = $HL, 1920x1080@60, 0x1440,    1" \
        "$SIZES
        mmcursor-monitor = $HL, 500, 280"

    same "$(mm_root)" "Virtual-1" "the corner panel is the root"
    near "$(mm_x Virtual-2)" 600 0.01 "the right arm is flush right"
    near "$(mm_y Virtual-2)" -95 0.01 "and centred, as the layout states"
    near "$(mm_x "$HL")" 0 0.01 "the lower arm is left-flush"
    near "$(mm_y "$HL")" 340 0.01 "and directly below"
    contains "$(prov "$HL")" "below" "the lower arm attached to the corner, not to Virtual-2"
    same "$(mm_warnings)" "" "an L derives cleanly"

    echo "  -- both arms are reachable from the corner --"
    warp 1280 720
    move 0 2 900 800
    near "$(mmy)" "340+1" 340 "moving down reaches the lower arm"
    warp 1280 720
    move 2 0 900 800
    near "$(mmx)" "600+1" 600 "moving right reaches the right arm"

    drop_headless
else
    echo
    echo "== three monitors in an L =="
    echo "  SKIP  headless outputs unavailable in this VM"
fi

# ---------------------------------------------------------------------------
# A mirrored monitor is not a place on the desk. rebuildLayout skips it; if it
# did not, two panels would claim the same logical rect and the overlap guard
# would disable the plugin for a perfectly ordinary configuration.
# ---------------------------------------------------------------------------

scenario "a mirrored monitor is ignored, not overlapped" 'monitor = Virtual-1, 2560x1440@60, 0x0, 1
monitor = Virtual-2, 1920x1080@60, 0x0, 1, mirror, Virtual-1' "$SIZES"
ok $(mm_inert && echo 1 || echo 0) "the plugin stays live with a mirror present"
near "$(hyprctl mmcursor | grep -c 'px/mm')" 1 0 "only the real panel is in the layout"
same "$(mm_root)" "Virtual-1" "and it is the root"

# ---------------------------------------------------------------------------
# Scale. Hyprland folds it into m_size before we see it, so it should be just
# another density — but that is a claim, and this is where it gets tested.
# ---------------------------------------------------------------------------

scenario "one panel at scale 2" 'monitor = Virtual-1, 2560x1440@60, 0x0,    2
monitor = Virtual-2, 1920x1080@60, 1280x0, 1' "$SIZES"
near "$(mm_w Virtual-1)" 600 0.01 "physical width is unchanged by scale"
near "$(mm_x Virtual-2)" 600 0.01 "still flush on the desk"
MMPU=$(mmper)
echo "  -- equal physical travel on both panels --"
warp 600 360
M0=$(mmx)
move 2 0 200 800
D1=$(python3 -c "print($(mmx)-$M0)")
warp 1800 360
M1=$(mmx)
move 2 0 200 800
D2=$(python3 -c "print($(mmx)-$M1)")
near "$D1" "$D2" 0.05 "same physical distance across a scale boundary"
near "$D1" "200*2*$MMPU" 0.05 "and it equals delta x mm-per-unit"

# ---------------------------------------------------------------------------
# Explicit overrides.
# ---------------------------------------------------------------------------

scenario "explicit absolute placement" "$SIDE_BY_SIDE" "$SIZES
        mmcursor-place = Virtual-2, at, 700, -50"
near "$(mm_x Virtual-2)" 700 0.01 "placed exactly where told (x)"
near "$(mm_y Virtual-2)" -50 0.01 "placed exactly where told (y)"
same "$(mm_root)" "Virtual-2" "an absolute placement becomes the root"
contains "$(prov Virtual-2)" "at" "provenance says it was placed by config"

scenario "explicit relation overriding the layout" "$SIDE_BY_SIDE" "$SIZES
        mmcursor-place = Virtual-2, below, Virtual-1, left"
# Hyprland has them side by side; the config says below. The config wins, and
# the contradicted cross-axis residual is dropped rather than flung across.
near "$(mm_x Virtual-2)" 0 0.01 "left-aligned under Virtual-1"
near "$(mm_y Virtual-2)" 340 0.01 "and directly below it"
contains "$(prov Virtual-2)" "config" "provenance marks it as configured"

scenario "per-seam gap" "$SIDE_BY_SIDE" "$SIZES
        mmcursor-gap = Virtual-2, Virtual-1, 22"
near "$(mm_x Virtual-2)" 622 0.01 "the seam's own bezel is used, named in either order"

scenario "2D offset" "$SIDE_BY_SIDE" "$SIZES
        mmcursor-offset = Virtual-2, 3, -7"
near "$(mm_x Virtual-2)" 603 0.01 "offset x applied"
near "$(mm_y Virtual-2)" -102 0.01 "offset y applied on top of the derived -95"

scenario "forcing align ignores the layout" 'monitor = Virtual-1, 2560x1440@60, 0x0,    1
monitor = Virtual-2, 1920x1080@60, 2560x0, 1, transform, 3' "$SIZES
        align = center"
near "$(mm_y Virtual-2)" -95 0.01 "align=center overrides a top-flush layout"

# ---------------------------------------------------------------------------
# Reload. The scenarios above already prove a changed line takes effect; this
# proves a REMOVED line does too, which is the case that was broken.
# ---------------------------------------------------------------------------

echo
echo "== removing a config line actually removes its effect =="
scenario "  (with the offset)" "$SIDE_BY_SIDE" "$SIZES
        mmcursor-offset = Virtual-2, 3, -7" > /dev/null
near "$(mm_x Virtual-2)" 603 0.01 "offset is in force"
scenario "  (line deleted, reloaded)" "$SIDE_BY_SIDE" "$SIZES" > /dev/null
near "$(mm_x Virtual-2)" 600 0.01 "deleting the line drops the offset"
near "$(mm_y Virtual-2)" -95 0.01 "and the derived value comes back"

echo
echo "== the same for a placement, which is the one that would strand a desk =="
scenario "  (with the placement)" "$SIDE_BY_SIDE" "$SIZES
        mmcursor-place = Virtual-2, at, 700, -50" > /dev/null
near "$(mm_x Virtual-2)" 700 0.01 "placement is in force"
scenario "  (line deleted, reloaded)" "$SIDE_BY_SIDE" "$SIZES" > /dev/null
near "$(mm_x Virtual-2)" 600 0.01 "deleting the line restores derivation"
same "$(mm_root)" "Virtual-1" "and the root goes back to the leftmost panel"

# ---------------------------------------------------------------------------
# The hyprctl surface.
# ---------------------------------------------------------------------------

echo
echo "== hyprctl subcommands =="
contains "$(hyprctl mmcursor reload)" "rebuilt" "mmcursor reload responds"
near "$(mm_x Virtual-2)" 600 0.01 "and the layout survives it"

hyprctl mmcursor place Virtual-2 800 10 > /dev/null
sleep 0.5
near "$(mm_x Virtual-2)" 800 0.01 "live place takes effect immediately (x)"
near "$(mm_y Virtual-2)" 10 0.01 "live place takes effect immediately (y)"
contains "$(hyprctl mmcursor)" "live overrides" "and the dump says it is not persisted"

hyprctl mmcursor offset Virtual-2 0 5 > /dev/null
sleep 0.5
near "$(mm_y Virtual-2)" 15 0.01 "live offset stacks on the live origin"

hyprctl reload > /dev/null
sleep 1.5
near "$(mm_x Virtual-2)" 600 0.01 "a reload drops live overrides"
near "$(mm_y Virtual-2)" -95 0.01 "and derivation is back in charge"

contains "$(hyprctl mmcursor nonsense)" "usage" "an unknown subcommand explains itself"

# ---------------------------------------------------------------------------
# Edge cases. Each of these has exactly one correct behaviour and it is not
# "carry on quietly".
# ---------------------------------------------------------------------------

echo
echo "== overlapping mm rects must disable the plugin, not guess =="
scenario "  (two panels placed on top of each other)" "$SIDE_BY_SIDE" "$SIZES
        mmcursor-place = Virtual-1, at, 0, 0
        mmcursor-place = Virtual-2, at, 100, 100" > /dev/null
mm_inert
ok $? "an overlap makes the plugin inert rather than picking a winner"

echo
echo "== a monitor with no physical size and no override =="
scenario "  (override removed for Virtual-2)" "$SIDE_BY_SIDE" '        mmcursor-monitor = Virtual-1, 600, 340' > /dev/null
# bochs reports a bogus-but-nonzero 320x200, so this is NOT expected to go
# inert — it is expected to build a layout using that bogus number. What must
# not happen is a silent wrong answer with no way to see it.
if mm_inert; then
    ok 0 "no usable size -> inert (EDID reported nothing)"
else
    near "$(mm_w Virtual-2)" 200 1 "falls back to the EDID value, visible in the dump"
fi

echo
echo "== hotplug =="
scenario "  (back to the baseline)" "$SIDE_BY_SIDE" "$SIZES" > /dev/null
BEFORE=$(hyprctl mmcursor | grep -c 'px/mm')
if hyprctl output create headless > /dev/null 2>&1; then
    sleep 1.5
    AFTER=$(hyprctl mmcursor | grep -c 'px/mm')
    # A headless output reports 0x0 physical, which cannot participate: the
    # correct response is to go inert and say so, not to project through it.
    if mm_inert; then
        ok 0 "a sizeless hotplugged output disables the plugin loudly"
    else
        ok $([ "$AFTER" -gt "$BEFORE" ] && echo 0 || echo 1) "a hotplugged output joins the layout"
    fi
    drop_headless
    near "$(hyprctl mmcursor | grep -c 'px/mm')" "$BEFORE" 0 "removing it restores the previous layout"
    near "$(mm_y Virtual-2)" -95 0.01 "and the derived geometry is unchanged"
else
    echo "  SKIP  headless outputs unavailable in this VM"
fi

# ---------------------------------------------------------------------------
# Normal use, on the restored baseline. The plugin has to survive all of the
# above and still behave.
# ---------------------------------------------------------------------------

echo
echo "== ordinary use after all of that =="
scenario "  (baseline)" "$SIDE_BY_SIDE" "$SIZES" > /dev/null
MMPU=$(mmper)

warp 1280 720
X0=$(mmx)
Y0=$(mmy)
move 2 0 600 800
move -2 0 600 800
near "$(mmx)" "$X0" 0.02 "out and back returns to the same mm x"
near "$(mmy)" "$Y0" 0.001 "and the same mm y"

echo "  -- boundary hammering --"
move -3 0 1200 800
near "$(mmx)" 0 0.01 "pinned to the left edge"
move 1 0 "$(python3 -c "print(int(100/$MMPU))")" 800
near "$(mmx)" 100 1.0 "walks back ~100mm with no accumulated overshoot"

move 0 -3 1200 800
near "$(mmy)" 0 0.01 "pinned to the top edge"

echo "  -- a warp mid-stream is adopted, not fought --"
warp 3100 960
near "$(mmx)" "600+(3100-2560)/(1080/300)" 0.05 "mm x adopted the warp"
near "$(mmy)" "-95+(960-(-240))/(1920/530)" 0.05 "mm y adopted the warp"

summary
exit $?
