#!/usr/bin/env bash
# Restart Hyprland in the VM with the test geometry and load the plugin.
# Run INSIDE the VM. Kept separate from verify.sh so you can re-verify against an
# already-running compositor without a restart.
set -uo pipefail

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export LIBSEAT_BACKEND=seatd
cd "$(dirname "$0")/../.."

pkill -x Hyprland 2>/dev/null; sleep 3
rm -f ~/.cache/hyprland/hyprlandCrashReport*.txt

setsid Hyprland -c test/vm/hyprland.conf > /tmp/mmcursor-hyprland.log 2>&1 &
sleep 12

pgrep -x Hyprland >/dev/null || { echo "compositor failed to start:"; tail -25 /tmp/mmcursor-hyprland.log; exit 1; }

export HYPRLAND_INSTANCE_SIGNATURE=$(ls -t "$XDG_RUNTIME_DIR"/hypr/ | head -1)
hyprctl plugin load "$PWD/build/mmcursor.so"
sleep 3

pgrep -x Hyprland >/dev/null || {
    echo "COMPOSITOR DIED ON PLUGIN LOAD"
    CR=$(ls -t ~/.cache/hyprland/hyprlandCrashReport*.txt 2>/dev/null | head -1)
    # Resolve our own frames to file:line — the nearest-symbol guesses in the
    # report itself point at unrelated Hyprland functions and will mislead you.
    [ -n "$CR" ] && grep -E 'mmcursor\.so' "$CR" && \
        for a in $(grep -oE 'mmcursor\.so\(\+0x[0-9a-f]+\)' "$CR" | grep -oE '0x[0-9a-f]+'); do
            echo "--- $a"; addr2line -Cfie build/mmcursor.so "$a"
        done
    exit 1
}

hyprctl plugin list | grep -q mmcursor || { echo "plugin did not load (check notifications / log)"; exit 1; }
echo "plugin loaded."
hyprctl mmcursor
