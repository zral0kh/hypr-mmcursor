# The core and the tests build with nothing but a compiler. The plugin needs
# Hyprland's headers. Keep those two worlds separate — you want to be able to
# iterate on the geometry without a compositor anywhere in sight.

CORE_SRC := src/geometry.cpp src/layout_build.cpp src/cursor_state.cpp
CXXFLAGS_COMMON := -std=c++23 -Wall -Wextra -Wconversion -O2

.PHONY: test plugin check-toolchain check-version load unload clean

# ---------------------------------------------------------------------------
# The part you should be running constantly.
# ---------------------------------------------------------------------------
TEST_CXXFLAGS := $(CXXFLAGS_COMMON) -g -fsanitize=address,undefined

# test_geometry  — the pieces compute what they claim.
# test_model     — the properties the plugin exists to provide, plus a
#                  differential comparison against a stock-Hyprland reference
#                  model.
# test_placement — the desk layout derived from the active Hyprland
#                  arrangement, and every override of it.
# test_apply     — the hook's decision logic against a simulated compositor.
# Run all four; they fail for different reasons.
test:
	$(CXX) $(TEST_CXXFLAGS) -o build/test_geometry  tests/test_geometry.cpp  $(CORE_SRC)
	$(CXX) $(TEST_CXXFLAGS) -o build/test_model     tests/test_model.cpp     $(CORE_SRC)
	$(CXX) $(TEST_CXXFLAGS) -o build/test_placement tests/test_placement.cpp $(CORE_SRC)
	$(CXX) $(TEST_CXXFLAGS) -o build/test_apply     tests/test_apply.cpp     $(CORE_SRC)
	./build/test_geometry
	./build/test_model
	./build/test_placement
	./build/test_apply

# ---------------------------------------------------------------------------
# The part that breaks every time Hyprland updates.
#
# Hyprland passes C++ objects across the plugin boundary, so there is no ABI
# guarantee: the plugin must be built by the SAME compiler that built Hyprland.
# Verified working against 0.56.0 built with GCC 16.1.1. `make check-toolchain`
# tells you if that stopped being true before you spend time on a crash.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Versioning.
#
# ./VERSION is the single source of truth; everything else derives from it, so
# there is no second number anywhere to drift out of sync.
#
# A build made from a release reports the bare version; one made from a working
# tree appends the commit, because the second-most-common real failure after
# "stale .so following a Hyprland upgrade" is "the rebuild did not actually
# happen" — and both are then answerable from the running compositor instead of
# by comparing file timestamps.
#
#     on the release tag, clean  ->  0.2.0
#     three commits on, dirty    ->  0.2.0+7be5c2f-dirty
#     tarball, no .git           ->  0.2.0
#
# `make vm-verify` ships the tree by tar, with no .git, so that last case is not
# theoretical. Never make a missing git a hard failure.
# ---------------------------------------------------------------------------
VERSION_BASE := $(shell cat VERSION 2>/dev/null || echo unknown)
GIT_EXACT    := $(shell git describe --exact-match --tags 2>/dev/null)
GIT_REV      := $(shell git describe --always --dirty --exclude '*' 2>/dev/null)
VERSION      := $(if $(GIT_EXACT),$(VERSION_BASE),$(if $(GIT_REV),$(VERSION_BASE)+$(GIT_REV),$(VERSION_BASE)))

PLUGIN_CXXFLAGS := $(CXXFLAGS_COMMON) -shared -fPIC --no-gnu-unique \
	-DMMCURSOR_VERSION='"$(VERSION)"' \
	$(shell pkg-config --cflags pixman-1 libdrm hyprland)

plugin: check-toolchain check-version
	$(CXX) $(PLUGIN_CXXFLAGS) -o build/mmcursor.so src/plugin.cpp $(CORE_SRC)
	@echo "built mmcursor $(VERSION)"

# If HEAD carries a release tag, it must agree with VERSION. Untagged commits
# are ordinary development and are left alone.
check-version:
	@t="$(GIT_EXACT)"; v="$(VERSION_BASE)"; \
	if [ -n "$$t" ] && [ "$$t" != "v$$v" ]; then \
		echo "ERROR: ./VERSION says $$v but HEAD is tagged $$t."; \
		echo "       These must match. Correct ./VERSION, or move the tag."; \
		exit 1; \
	fi

# Hyprland records its compiler in .comment. Compare it with ours.
check-toolchain:
	@hl="$$(readelf -p .comment $$(command -v Hyprland) 2>/dev/null | grep -o 'GCC: (GNU) [0-9.]*' | head -1 | sed 's/.*) //')"; \
	ours="$$($(CXX) -dumpfullversion 2>/dev/null || $(CXX) -dumpversion)"; \
	if [ -z "$$hl" ]; then \
		echo "note: could not read Hyprland's compiler; is it a clang build? proceeding."; \
	elif [ "$$hl" != "$$ours" ]; then \
		echo "ERROR: Hyprland was built with GCC $$hl, you have $(CXX) $$ours."; \
		echo "       Hyprland's plugin API passes C++ objects; mismatched compilers"; \
		echo "       crash at load rather than failing to build. Install a matching GCC."; \
		exit 1; \
	else \
		echo "toolchain ok: GCC $$ours matches Hyprland"; \
	fi

# Load into the RUNNING compositor. Only ever do this in a VM or a nested
# Hyprland; see the testing section of the README.
load: plugin
	hyprctl plugin load $(CURDIR)/build/mmcursor.so
	hyprctl mmcursor

unload:
	hyprctl plugin unload $(CURDIR)/build/mmcursor.so

# ---------------------------------------------------------------------------
# The VM. This is where the plugin is actually exercised.
#
# A container cannot host Hyprland: CHeadlessBackend::drmFD() returns -1 and the
# allocator comes from a started backend's DRM fd, so headless-only dies with
# "no allocator available". The VM supplies a DRM device and a seat.
# ---------------------------------------------------------------------------
vm-up:
	./test/vm/run.sh fetch
	./test/vm/run.sh seed
	./test/vm/run.sh start
	./test/vm/run.sh wait

# Push the tree in, build in there, load, and assert. `vm-verify` is the target
# that turns "does the seam feel right" into pass/fail.
#
# verify.sh           the cursor behaves correctly on one desk
# verify-placement.sh the desk itself is derived correctly from whatever
#                     arrangement Hyprland has active, plus every override and
#                     edge case. It rewrites hyprland.conf repeatedly and
#                     restores it on exit.
# verify-autoload.sh  the startup path: plugin loaded BY CONFIG, so it is live
#                     while monitors are still being brought up. Runs last
#                     because it restarts the compositor with its own config.
vm-verify:
	tar cf - src tests Makefile VERSION test | ./test/vm/run.sh ssh 'rm -rf ~/mmcursor && mkdir -p ~/mmcursor && cd ~/mmcursor && tar xf -'
	./test/vm/run.sh ssh 'cd ~/mmcursor && make plugin && make -C test/vpointer'
	./test/vm/run.sh ssh 'cd ~/mmcursor && ./test/vm/restart-and-load.sh'
	./test/vm/run.sh ssh 'cd ~/mmcursor && ./test/vm/verify.sh'
	./test/vm/run.sh ssh 'cd ~/mmcursor && ./test/vm/verify-placement.sh'
	./test/vm/run.sh ssh 'cd ~/mmcursor && ./test/vm/verify-autoload.sh'

vm-ssh:
	./test/vm/run.sh ssh

vm-down:
	./test/vm/run.sh stop

clean:
	rm -rf build

$(shell mkdir -p build)
