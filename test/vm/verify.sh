#!/usr/bin/env bash
# Scripted verification of mmcursor inside a running compositor.
#
# Run this INSIDE the VM, with the plugin already loaded:
#   ~/mmcursor/test/vm/verify.sh
#
# Every expected number here is predicted by the unit tests, not read off a
# previous run. The point is that "does the seam feel right" becomes pass/fail
# you can re-run after every Hyprland update — which is exactly when this breaks.
set -uo pipefail

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export HYPRLAND_INSTANCE_SIGNATURE=$(ls -t "$XDG_RUNTIME_DIR"/hypr/ | head -1)
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | head -1)}

VP="$(dirname "$0")/../vpointer/vpointer"
[ -x "$VP" ] || { echo "build test/vpointer first"; exit 2; }

PASS=0; FAIL=0
mmx() { hyprctl mmcursor | awk -F'[:,]' '/^mm position/{gsub(/ /,"",$2); print $2}'; }
mmy() { hyprctl mmcursor | awk -F'[:,]' '/^mm position/{gsub(/ /,"",$3); print $3}'; }
curx() { hyprctl cursorpos | cut -d, -f1 | tr -d ' '; }
cury() { hyprctl cursorpos | cut -d, -f2 | tr -d ' '; }
mmper() { hyprctl mmcursor | awk '/^mm per input unit/{print $5}'; }

near() { # value expected tolerance label
    if python3 -c "import sys; sys.exit(0 if abs($1-($2))<=$3 else 1)" 2>/dev/null; then
        printf "  PASS  %-58s %s ~= %s\n" "$4" "$1" "$2"; PASS=$((PASS+1))
    else
        printf "  FAIL  %-58s %s != %s (tol %s)\n" "$4" "$1" "$2" "$3"; FAIL=$((FAIL+1))
    fi
}
# Seed the lazy reconcile: our reconcile is a pull, so it happens on the next
# relative event, not at warp time. One null event is enough.
settle() { "$VP" 0 0 1 >/dev/null 2>&1; sleep 0.4; }
warp() { hyprctl dispatch movecursor "$1" "$2" >/dev/null; sleep 0.3; settle; }

MMPU=$(mmper)
echo "mm per input unit: $MMPU"
echo
echo "== 1. seam continuity: horizontal-only input must not move the cursor vertically =="
warp 300 0                       # top edge of Virtual-1, where stock is worst
Y0=$(mmy)
"$VP" 2 0 1500 800 >/dev/null; sleep 1
near "$(mmy)" "$Y0" 0.001 "physical y unchanged across the seam"
near "$(curx)" 3193 2 "ended on Virtual-2 (logical x)"
# Predicted by tests/test_geometry.cpp: -240 + (0-(-95)) * (1920/530)
near "$(cury)" 104.1509 1.0 "logical y moved to the physically level point"

echo
echo "== 2. no hysteresis: an exact reverse path returns to the same physical point =="
warp 1280 720
X0=$(mmx); Y1=$(mmy)
"$VP" 2 0 1200 800 >/dev/null; sleep 1
CROSSED=$(mmx)
"$VP" -2 0 1200 800 >/dev/null; sleep 1
near "$CROSSED" "$X0+1200*2*$MMPU" 0.01 "outbound travel matches delta x mm-per-unit"
near "$(mmx)" "$X0" 0.01 "returned to the same mm x"
near "$(mmy)" "$Y1" 0.001 "returned to the same mm y"

echo
echo "== 3. equal mm travel produces equal PHYSICAL travel on each panel =="
# 200 events of 1px each. Physical distance is identical on both panels by
# construction; the logical pixel count must differ by the density ratio.
warp 1280 720
A0=$(cury); M0=$(mmy); "$VP" 0 1 200 800 >/dev/null; sleep 0.8
DY_V1=$(python3 -c "print($(cury)-$A0)"); DMM_V1=$(python3 -c "print($(mmy)-$M0)")
warp 3100 720
B0=$(cury); M1=$(mmy); "$VP" 0 1 200 800 >/dev/null; sleep 0.8
DY_V2=$(python3 -c "print($(cury)-$B0)"); DMM_V2=$(python3 -c "print($(mmy)-$M1)")
echo "  Virtual-1: $DY_V1 logical px for $DMM_V1 mm"
echo "  Virtual-2: $DY_V2 logical px for $DMM_V2 mm"
near "$DMM_V1" "$DMM_V2" 0.02 "same physical distance on both panels"
near "$DY_V1" "200*$MMPU*(1440/340)" 1.5 "Virtual-1 px travel = mm x 4.2353"
near "$DY_V2" "200*$MMPU*(1920/530)" 1.5 "Virtual-2 px travel = mm x 3.6226"
near "$(python3 -c "print($DY_V1/$DY_V2)")" "(1440/340)/(1920/530)" 0.02 "px ratio equals density ratio"

echo
echo "== 4. external absolute warps are adopted, not fought =="
warp 3100 960
# mm must now agree with where the compositor actually put the cursor.
near "$(mmx)" "600+(3100-2560)/(1080/300)" 0.05 "mm x adopted the warp"
near "$(mmy)" "-95+(960-(-240))/(1920/530)" 0.05 "mm y adopted the warp"

echo
echo "$PASS passed, $FAIL failed"
exit $((FAIL > 0))
