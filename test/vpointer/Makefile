# Built separately from the plugin: this is a Wayland CLIENT, and it has no
# business linking against anything Hyprland.
PROTO := wlr-virtual-pointer-unstable-v1
# Hyprland ships the protocol XML; fall back to a local copy if it moved.
XML   := $(firstword $(wildcard /usr/share/hyprland/protocols/$(PROTO).xml \
                                /usr/share/wlr-protocols/unstable/$(PROTO).xml \
                                ./$(PROTO).xml))

.PHONY: all clean
all: vpointer

$(PROTO)-client-protocol.h: $(XML)
	@test -n "$(XML)" || { echo "cannot find $(PROTO).xml; drop a copy in this dir"; exit 1; }
	wayland-scanner client-header $(XML) $@

$(PROTO)-protocol.c: $(XML)
	wayland-scanner private-code $(XML) $@

vpointer: vpointer.c $(PROTO)-protocol.c $(PROTO)-client-protocol.h
	$(CC) -O2 -Wall -Wextra -o $@ vpointer.c $(PROTO)-protocol.c $(shell pkg-config --cflags --libs wayland-client)

clean:
	rm -f vpointer $(PROTO)-protocol.c $(PROTO)-client-protocol.h
