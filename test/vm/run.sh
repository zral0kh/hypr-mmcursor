#!/usr/bin/env bash
# Bring up an Arch VM that can actually run Hyprland, and load mmcursor into it.
#
# Why a VM at all: a container cannot host Hyprland. CHeadlessBackend::drmFD()
# returns -1 and aquamarine takes its allocator from a started backend's DRM fd,
# so headless-only dies with "no allocator available". Hyprland needs either a
# real GPU seat or a parent compositor. The VM provides the seat.
#
# Only ONE working virtio-gpu head is needed: once the DRM backend is up and the
# allocator exists, `hyprctl output create headless` works, so the second
# (mismatched-density) monitor is a headless output.
#
#   ./test/vm/run.sh fetch    download the cloud image
#   ./test/vm/run.sh seed     build the cloud-init seed ISO
#   ./test/vm/run.sh start    boot the VM (backgrounded)
#   ./test/vm/run.sh wait     block until SSH answers and cloud-init has finished
#   ./test/vm/run.sh ssh      shell in
#   ./test/vm/run.sh stop     shut it down
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VM="$ROOT/build/vm"
IMG="$VM/arch.qcow2"
SEED="$VM/seed.iso"
KEY="$VM/id_ed25519"
PORT=2222
VNC=9
URL="https://fastly.mirror.pkgbuild.com/images/latest/Arch-Linux-x86_64-cloudimg.qcow2"

mkdir -p "$VM"

# A dedicated keypair, generated into build/ (gitignored). Deliberately not the
# user's own key: nothing personal should end up in a throwaway VM or the repo.
ensure_key() {
    [ -f "$KEY" ] || ssh-keygen -q -t ed25519 -N '' -C mmcursor-vm -f "$KEY"
}

SSHOPTS=(-i "$KEY" -p "$PORT"
         -o StrictHostKeyChecking=no
         -o UserKnownHostsFile=/dev/null
         -o LogLevel=ERROR)

case "${1:-}" in
fetch)
    if [ -f "$IMG" ]; then echo "image already present: $IMG"; exit 0; fi
    echo "downloading $(basename "$URL") (~555MB)…"
    curl -L --progress-bar -o "$IMG.tmp" "$URL"
    mv "$IMG.tmp" "$IMG"
    # cloud-init's growpart expands btrfs on first boot.
    qemu-img resize "$IMG" +20G
    echo "image ready: $IMG"
    ;;

seed)
    ensure_key
    sed "s|@SSHKEY@|$(cat "$KEY.pub")|" "$ROOT/test/vm/user-data.in" > "$VM/user-data"
    cloud-localds "$SEED" "$VM/user-data"
    echo "seed ready: $SEED"
    ;;

start)
    [ -f "$IMG" ]  || { echo "no image; run: $0 fetch" >&2; exit 1; }
    [ -f "$SEED" ] || { echo "no seed;  run: $0 seed"  >&2; exit 1; }
    if [ -f "$VM/pid" ] && kill -0 "$(cat "$VM/pid")" 2>/dev/null; then
        echo "already running (pid $(cat "$VM/pid"))"; exit 0
    fi
    # -vnc rather than -display none: costs nothing, gives virtio-gpu a real
    # display sink so the head comes up CONNECTED, and lets you look at the
    # cursor when a number surprises you.
    # Display device. What matters is only that the guest ends up with a DRM
    # device exposing a CONNECTED connector, because that is where aquamarine
    # gets its allocator from.
    #
    # virtio-gpu is the best-supported choice for Wayland compositors, but on
    # Arch it lives in separate packages (qemu-hw-display-virtio-gpu*) that
    # plain qemu-base does not pull in. bochs-display needs no extra package and
    # binds to the in-tree bochs DRM driver, so it is the default here.
    # Override with: MMCURSOR_VM_VGA=virtio-gpu-pci ./test/vm/run.sh start
    VGADEV="${MMCURSOR_VM_VGA:-bochs-display}"
    echo "display device: $VGADEV"

    qemu-system-x86_64 \
        -enable-kvm -m 4G -smp 4 \
        -drive file="$IMG",if=virtio \
        -cdrom "$SEED" \
        -device "$VGADEV" \
        -nic user,hostfwd=tcp::$PORT-:22 \
        -vnc :$VNC \
        -pidfile "$VM/pid" \
        -daemonize \
        -serial file:"$VM/serial.log"
    echo "started. vnc :$VNC (port $((5900+VNC))), ssh port $PORT, pid $(cat "$VM/pid")"
    ;;

wait)
    ensure_key
    echo -n "waiting for sshd"
    for i in $(seq 1 150); do
        if ssh "${SSHOPTS[@]}" -o ConnectTimeout=3 dev@localhost true 2>/dev/null; then
            echo " — up"
            echo -n "waiting for cloud-init"
            ssh "${SSHOPTS[@]}" dev@localhost 'sudo cloud-init status --wait >/dev/null 2>&1 || true'
            echo " — done"
            ssh "${SSHOPTS[@]}" dev@localhost 'echo; echo "hyprland: $(pacman -Q hyprland 2>/dev/null || echo MISSING)"; echo "seatd:    $(systemctl is-active seatd)"; echo; echo "DRM connectors:"; for s in /sys/class/drm/*/status; do echo "  $(basename $(dirname $s)): $(cat $s)"; done'
            exit 0
        fi
        echo -n .
        sleep 4
    done
    echo " — TIMED OUT"; tail -30 "$VM/serial.log" 2>/dev/null; exit 1
    ;;

ssh)   ensure_key; shift; exec ssh "${SSHOPTS[@]}" dev@localhost "$@" ;;
scp)   ensure_key; shift; exec scp -i "$KEY" -P "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$@" ;;

stop)
    if [ -f "$VM/pid" ] && kill -0 "$(cat "$VM/pid")" 2>/dev/null; then
        ssh "${SSHOPTS[@]}" -o ConnectTimeout=3 dev@localhost 'sudo poweroff' 2>/dev/null || kill "$(cat "$VM/pid")"
        echo "stopping…"; sleep 3
        kill -0 "$(cat "$VM/pid")" 2>/dev/null && kill "$(cat "$VM/pid")" || true
        rm -f "$VM/pid"
    fi
    echo "stopped"
    ;;

*) sed -n '1,25p' "$0" | grep '^#' | sed 's/^# \?//'; exit 1 ;;
esac
