// vpointer — feed relative pointer motion into a running Hyprland.
//
//   vpointer <dx> <dy> <count> [delay_us]
//
// Exists so cross-seam behaviour is pass/fail rather than judged by feel. There
// is no packaged tool for this on Arch (wlrctl, ydotool are both absent), but
// Hyprland ships the protocol, so it is ~100 lines.
//
// It drives zwlr_virtual_pointer_v1.motion(), which lands in Hyprland as an
// IPointer::SMotionEvent -> CInputManager::onMouseMoved -> CPointerManager::move
// — i.e. exactly the path mmcursor hooks, through the real relative-pointer
// plumbing rather than a simulation of it.
//
// Deltas are wl_fixed (24.8), so fractional values are fine.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat*                          seat = NULL;
static struct zwlr_virtual_pointer_manager_v1*  mgr  = NULL;

static void reg_global(void* data, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
    (void)data; (void)ver;
    if (!strcmp(iface, wl_seat_interface.name))
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
        mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void reg_remove(void* d, struct wl_registry* r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener REG_LISTENER = { reg_global, reg_remove };

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <dx> <dy> <count> [delay_us]\n", argv[0]);
        return 2;
    }
    const double dx    = atof(argv[1]);
    const double dy    = atof(argv[2]);
    const long   count = atol(argv[3]);
    const long   delay = (argc > 4) ? atol(argv[4]) : 1500;

    struct wl_display* dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "vpointer: cannot connect to WAYLAND_DISPLAY\n"); return 1; }

    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &REG_LISTENER, NULL);
    wl_display_roundtrip(dpy);

    if (!seat || !mgr) {
        fprintf(stderr, "vpointer: compositor lacks %s\n",
                !mgr ? "zwlr_virtual_pointer_manager_v1" : "wl_seat");
        return 1;
    }

    struct zwlr_virtual_pointer_v1* vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
    if (!vp) { fprintf(stderr, "vpointer: create_virtual_pointer failed\n"); return 1; }
    wl_display_roundtrip(dpy);

    // Give the compositor a moment to attach the new pointer device; motion sent
    // before that is silently dropped and the test then "passes" having done
    // nothing.
    usleep(200000);

    for (long i = 0; i < count; ++i) {
        zwlr_virtual_pointer_v1_motion(vp, (uint32_t)(i * 8), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
        zwlr_virtual_pointer_v1_frame(vp);
        if (wl_display_flush(dpy) < 0) { fprintf(stderr, "vpointer: flush failed\n"); return 1; }
        if (delay > 0)
            usleep((useconds_t)delay);
    }

    wl_display_roundtrip(dpy);
    usleep(200000);   // let the last events be processed before we tear down

    zwlr_virtual_pointer_v1_destroy(vp);
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);

    printf("vpointer: sent %ld events of (%.4f, %.4f)\n", count, dx, dy);
    return 0;
}
