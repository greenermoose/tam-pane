# tam-pane: a Wayland compositor. Plain Make; no other build system.
#
#   make            build build/tam-pane
#   make test       unit, smoke, and headless input integration checks
#   make install    install to PREFIX (default $HOME/.local)
#   make uninstall  remove what install put there
#   make clean      remove build/
#
# Today's dependencies are wlroots 0.20, wayland-server and xkbcommon (see
# arch/dependencies.md for why each is here and when it goes), plus libtam,
# linked statically from LIBTAM_PREFIX.

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LIBTAM_PREFIX ?= $(HOME)/.local

PKG_CONFIG ?= pkg-config
PKGS = wlroots-0.20 wayland-server xkbcommon
# Used only by the test client, never linked into the compositor.
CLIENT_PKGS = wayland-client

VERSION := $(shell tr -d '[:space:]' < VERSION)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-parameter \
	-D_POSIX_C_SOURCE=200809L -DWLR_USE_UNSTABLE \
	-DTP_VERSION='"$(VERSION)"' \
	-Iinclude -Ibuild/protocols -I$(LIBTAM_PREFIX)/include \
	-ffile-prefix-map=$(CURDIR)=. -MMD -MP
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS))
LIBTAM := $(LIBTAM_PREFIX)/lib/libtam.a

# Pure logic: no wlroots, so the unit tests build without it.
CORE_SRCS = src/tags.c src/layout/dwindle.c
# The compositor proper.
WM_SRCS = src/main.c src/server.c src/settings.c src/monitor.c src/client.c src/focus.c \
	src/spawn.c src/bindings.c src/layout/arrange.c \
	src/input/input.c src/input/keyboard.c src/input/pointer.c

CORE_OBJS = $(patsubst src/%.c,build/%.o,$(CORE_SRCS))
WM_OBJS = $(patsubst src/%.c,build/%.o,$(WM_SRCS))
TESTS = build/tests/test_tags build/tests/test_dwindle

PROTO_HEADERS = build/protocols/xdg-shell-protocol.h \
	build/protocols/wlr-layer-shell-unstable-v1-protocol.h
CLIENT_PROTO_HEADER = build/protocols/xdg-shell-client-protocol.h
CLIENT_PROTO_CODE = build/protocols/xdg-shell-protocol.c

all: build/tam-pane

build/tam-pane: $(CORE_OBJS) $(WM_OBJS) $(LIBTAM)
	$(CC) $(LDFLAGS) -o $@ $(CORE_OBJS) $(WM_OBJS) $(LIBTAM) $(PKG_LIBS) -lm

$(WM_OBJS): build/%.o: src/%.c | $(PROTO_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

$(CORE_OBJS): build/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

build/protocols/xdg-shell-protocol.h: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) server-header $< $@

build/protocols/wlr-layer-shell-unstable-v1-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) server-header $< $@

build/protocols/xdg-shell-client-protocol.h: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $@

build/protocols/xdg-shell-protocol.c: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $< $@

build/tests/test_%: tests/test_%.c $(CORE_OBJS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(CORE_OBJS)

build/tests/test_input_headless: tests/test_input_headless.c $(CORE_OBJS) \
	$(filter-out build/main.o,$(WM_OBJS)) $(LIBTAM) \
	$(CLIENT_PROTO_HEADER) $(CLIENT_PROTO_CODE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Ibuild/protocols -o $@ $< \
		$(CLIENT_PROTO_CODE) $(CORE_OBJS) \
		$(filter-out build/main.o,$(WM_OBJS)) $(LIBTAM) $(PKG_LIBS) \
		$(shell $(PKG_CONFIG) --libs $(CLIENT_PKGS)) -lm

test: $(TESTS) build/tam-pane build/tests/test_input_headless
	@for t in $(TESTS); do $$t || exit 1; done
	@sh tests/smoke-headless.sh build/tam-pane
	@sh tests/run-input-headless.sh build/tests/test_input_headless

install: build/tam-pane
	install -Dm755 build/tam-pane $(DESTDIR)$(BINDIR)/tam-pane

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/tam-pane

clean:
	rm -rf build

.PHONY: all test install uninstall clean

-include $(shell find build -name '*.d' 2>/dev/null)
