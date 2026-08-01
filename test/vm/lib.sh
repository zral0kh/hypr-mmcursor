#!/usr/bin/env bash
# Shared helpers for the in-VM verification scripts. Source it, do not run it.
#
# Everything here is a thin wrapper over hyprctl. Two things are load-bearing:
#
#   settle()  — our reconcile is a PULL, so an absolute warp is not adopted
#               until the next relative event. One null event is enough, and
#               without it every reading after a warp is one step stale. That is
#               the mechanism working, not a race.
#
#   near()    — every expected value passed to this should be predicted by a
#               unit test or derivable from the config, never read off a
#               previous run. A test that asserts what the code currently does
#               cannot fail for the right reason.

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export HYPRLAND_INSTANCE_SIGNATURE=${HYPRLAND_INSTANCE_SIGNATURE:-$(ls -t "$XDG_RUNTIME_DIR"/hypr/ | head -1)}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | head -1)}

PASS=0
FAIL=0

# --- reading the plugin's state --------------------------------------------
mmx() { hyprctl mmcursor | awk -F'[:,]' '/^mm position/{gsub(/ /,"",$2); print $2}'; }
mmy() { hyprctl mmcursor | awk -F'[:,]' '/^mm position/{gsub(/ /,"",$3); print $3}'; }
mmper() { hyprctl mmcursor | awk '/^mm per input unit/{print $5}'; }
curx() { hyprctl cursorpos | cut -d, -f1 | tr -d ' '; }
cury() { hyprctl cursorpos | cut -d, -f2 | tr -d ' '; }

# One field of a monitor's mm rect from the dump: mm_field NAME 1..4 -> x y w h
mm_field() {
    hyprctl mmcursor | awk -v n="$1" -v i="$2" '
        $1 == n { s = $0
                  sub(/.*mm \[/, "", s); sub(/\].*/, "", s); gsub(/x/, " ", s)
                  split(s, a, " "); print a[i] }'
}
mm_x() { mm_field "$1" 1; }
mm_y() { mm_field "$1" 2; }
mm_w() { mm_field "$1" 3; }
mm_h() { mm_field "$1" 4; }

# How the builder says it placed this monitor.
prov() {
    hyprctl mmcursor | awk -v n="$1" '$1 == n { i = index($0, "<- "); if (i) print substr($0, i + 3) }'
}

# Order monitors are emitted in; the first is the root.
mm_root() { hyprctl mmcursor | awk '/^monitors:/{f=1; next} f && NF {print $1; exit}'; }

mm_warnings() { hyprctl mmcursor | awk '/^warnings:/{f=1; next} f && NF {print}'; }
mm_inert() { hyprctl mmcursor | grep -q 'layout: EMPTY'; }

# --- assertions -------------------------------------------------------------
near() { # value expected tolerance label
    if python3 -c "import sys; sys.exit(0 if abs($1-($2))<=$3 else 1)" 2>/dev/null; then
        printf "  PASS  %-58s %s ~= %s\n" "$4" "$1" "$2"
        PASS=$((PASS + 1))
    else
        printf "  FAIL  %-58s %s != %s (tol %s)\n" "$4" "$1" "$2" "$3"
        FAIL=$((FAIL + 1))
    fi
}

ok() { # condition-already-evaluated label
    if [ "$1" = "0" ]; then
        printf "  PASS  %s\n" "$2"
        PASS=$((PASS + 1))
    else
        printf "  FAIL  %s\n" "$2"
        FAIL=$((FAIL + 1))
    fi
}

same() { # actual expected label
    if [ "$1" = "$2" ]; then
        printf "  PASS  %-58s %s\n" "$3" "$1"
        PASS=$((PASS + 1))
    else
        printf "  FAIL  %-58s got '%s', want '%s'\n" "$3" "$1" "$2"
        FAIL=$((FAIL + 1))
    fi
}

contains() { # haystack needle label
    case "$1" in
        *"$2"*)
            printf "  PASS  %-58s %s\n" "$3" "$2"
            PASS=$((PASS + 1))
            ;;
        *)
            printf "  FAIL  %-58s '%s' does not contain '%s'\n" "$3" "$1" "$2"
            FAIL=$((FAIL + 1))
            ;;
    esac
}

# --- driving the pointer ----------------------------------------------------
VP="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../vpointer/vpointer"

settle() { "$VP" 0 0 1 >/dev/null 2>&1; sleep 0.4; }
warp() { hyprctl dispatch movecursor "$1" "$2" >/dev/null; sleep 0.3; settle; }
move() { "$VP" "$@" >/dev/null 2>&1; sleep 0.8; }

summary() {
    echo
    echo "$PASS passed, $FAIL failed"
    return $((FAIL > 0))
}
