#!/usr/bin/env bash
# Verify the plugin behaves when it is loaded BY CONFIG, i.e. present while
# Hyprland is still bringing monitors up.
#
# Run this INSIDE the VM:
#   ~/mmcursor/test/vm/verify-autoload.sh
#
# Why this exists as a separate script: every other test here loads the plugin
# by hand, after the compositor has settled. `restart-and-load.sh` even sleeps 12
# seconds first. So the startup path — the one every real user takes, via
# `plugin =` in hyprland.conf — had no coverage at all, and a bug shipped in it.
#
# That bug: Hyprland applies monitor rules one at a time, so mid-startup two
# monitors can share a logical rect. Overlapping rects are adjacent on no axis,
# nothing can attach to anything, every monitor falls back, and the fallback
# converted a zero offset — stacking them. The plugin then refused its own
# layout, disabled itself, and told the user to check a config keyword they had
# never written. Moments later layoutChanged rebuilt correctly.
#
# Which is why this asserts the REFUSAL COUNTER and not the geometry. The end
# state was always right; only the transient was wrong, and a test that looks at
# the settled layout sees nothing at all.
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 2
ROOT="$PWD"
CONF="$ROOT/test/vm/hyprland-autoload.conf"

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export LIBSEAT_BACKEND=seatd

[ -f "$ROOT/build/mmcursor.so" ] || { echo "build the plugin first: make plugin"; exit 2; }

# The baseline desk, plus the one line that makes this test what it is. Note the
# absolute path: `plugin =` does not expand ~, and the resulting failure does not
# show up in `hyprctl configerrors`.
sed 's|^plugin {|plugin = '"$ROOT"'/build/mmcursor.so\n\nplugin {|' "$ROOT/test/vm/hyprland.conf" > "$CONF"
grep -q '^plugin = ' "$CONF" || { echo "could not build the autoload config"; exit 2; }

echo "== restarting Hyprland with the plugin autoloaded =="
pkill -x Hyprland 2>/dev/null
sleep 3
rm -f ~/.cache/hyprland/hyprlandCrashReport*.txt

setsid Hyprland -c "$CONF" > /tmp/mmcursor-autoload.log 2>&1 &
sleep 12

if ! pgrep -x Hyprland > /dev/null; then
    echo "COMPOSITOR FAILED TO START WITH THE PLUGIN AUTOLOADED"
    tail -25 /tmp/mmcursor-autoload.log
    exit 1
fi

# shellcheck source=lib.sh
. "$ROOT/test/vm/lib.sh"

echo
echo "== the plugin came up on its own =="
hyprctl plugin list | grep -q mmcursor
ok $? "autoloaded from hyprland.conf"

mm_inert
ok $([ $? -eq 0 ] && echo 1 || echo 0) "it is live, not inert"

echo
echo "== and it never disabled itself on the way there =="
TALLY=$(hyprctl mmcursor | grep '^rebuilds:')
echo "  $TALLY"
REFUSED=$(echo "$TALLY" | sed 's/.*refused: *//')
near "$REFUSED" 0 0 "zero refusals during startup"
same "$(mm_warnings)" "" "and no placement warnings"

echo
echo "== the settled layout is still the right one =="
near "$(mm_x Virtual-2)" 600 0.01 "Virtual-2 flush right"
near "$(mm_y Virtual-2)" -95 0.01 "Virtual-2 centred on the horizon"
contains "$(prov Virtual-2)" "right-of" "derived, not fallen back to"

echo
echo "== and the cursor actually works after an autoloaded start =="
warp 300 0
Y0=$(mmy)
move 2 0 1500 800
near "$(mmy)" "$Y0" 0.001 "no physical vertical drift across the seam"

rm -f "$CONF"
summary
exit $?
