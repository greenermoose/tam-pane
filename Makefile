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
# linked statically from LIBTAM_PREFIX, and Lua 5.5 vendored in
# third_party/lua/.

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LIBTAM_PREFIX ?= $(HOME)/.local

PKG_CONFIG ?= pkg-config
PKGS = wlroots-0.20 wayland-server wayland-client xkbcommon
# Used only by the test client, never linked into the compositor.
CLIENT_PKGS = wayland-client

VERSION := $(shell tr -d '[:space:]' < VERSION)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-parameter \
	-D_POSIX_C_SOURCE=200809L -DWLR_USE_UNSTABLE \
	-DTP_VERSION='"$(VERSION)"' \
	-Iinclude -Ibuild/protocols -I$(LIBTAM_PREFIX)/include -Ithird_party/lua \
	-ffile-prefix-map=$(CURDIR)=. -MMD -MP
# Vendored Lua 5.5: static object set, hidden visibility, no dlopen (-rdynamic
# is never used), so lua_* symbols never collide with libinput's liblua 5.4.
LUA_CFLAGS = -O2 -g -std=c99 -Wall -Wextra -fvisibility=hidden \
	-DLUA_USE_POSIX -ffile-prefix-map=$(CURDIR)=. -MMD -MP
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS))
XKB_CFLAGS := $(shell $(PKG_CONFIG) --cflags xkbcommon)
XKB_LIBS := $(shell $(PKG_CONFIG) --libs xkbcommon)
LIBTAM := $(LIBTAM_PREFIX)/lib/libtam.a

# Vendored Lua 5.5 core + safe standard libraries only (no io/os/package/debug).
LUA_SRCS = third_party/lua/lapi.c third_party/lua/lauxlib.c \
	third_party/lua/lbaselib.c third_party/lua/lcode.c \
	third_party/lua/lcorolib.c third_party/lua/lctype.c \
	third_party/lua/ldebug.c third_party/lua/ldo.c \
	third_party/lua/ldump.c third_party/lua/lfunc.c \
	third_party/lua/lgc.c third_party/lua/llex.c \
	third_party/lua/lmathlib.c third_party/lua/lmem.c \
	third_party/lua/lobject.c third_party/lua/lopcodes.c \
	third_party/lua/lparser.c third_party/lua/lstate.c \
	third_party/lua/lstring.c third_party/lua/lstrlib.c \
	third_party/lua/ltable.c third_party/lua/ltablib.c \
	third_party/lua/ltm.c third_party/lua/lundump.c \
	third_party/lua/lutf8lib.c third_party/lua/lvm.c \
	third_party/lua/lzio.c
LUA_OBJS = $(patsubst third_party/lua/%.c,build/lua/%.o,$(LUA_SRCS))

# Pure logic: no wlroots, so the unit tests build without it.
CORE_SRCS = src/tags.c src/layout/dwindle.c
LUA_HOST_SRCS = src/lua.c src/lua_bindings.c src/settings.c src/spawn.c
# The compositor proper.
WM_SRCS = src/main.c src/server.c src/monitor.c src/layer.c \
	src/client.c src/focus.c src/bindings.c src/layout/arrange.c \
	src/input/input.c src/input/keyboard.c src/input/pointer.c

CORE_OBJS = $(patsubst src/%.c,build/%.o,$(CORE_SRCS))
LUA_HOST_OBJS = $(patsubst src/%.c,build/%.o,$(LUA_HOST_SRCS))
WM_OBJS = $(patsubst src/%.c,build/%.o,$(WM_SRCS))
TESTS = build/tests/test_tags build/tests/test_dwindle build/tests/test_lua

PROTO_HEADERS = build/protocols/xdg-shell-protocol.h \
	build/protocols/wlr-layer-shell-unstable-v1-protocol.h \
	build/protocols/viewporter-client-protocol.h \
	build/protocols/fractional-scale-v1-client-protocol.h
PROTO_SRCS = build/protocols/viewporter-protocol.c \
	build/protocols/fractional-scale-v1-protocol.c
PROTO_OBJS = $(PROTO_SRCS:.c=.o)
CLIENT_PROTO_HEADERS = build/protocols/xdg-shell-client-protocol.h \
	build/protocols/wlr-layer-shell-unstable-v1-client-protocol.h
CLIENT_PROTO_SRCS = build/protocols/xdg-shell-protocol.c \
	build/protocols/wlr-layer-shell-unstable-v1-protocol.c

all: build/tam-pane

build/tam-pane: $(CORE_OBJS) $(LUA_HOST_OBJS) $(WM_OBJS) $(PROTO_OBJS) $(LUA_OBJS) $(LIBTAM)
	$(CC) $(LDFLAGS) -o $@ $(CORE_OBJS) $(LUA_HOST_OBJS) $(WM_OBJS) $(PROTO_OBJS) $(LUA_OBJS) $(LIBTAM) $(PKG_LIBS) -lm

$(LUA_OBJS): build/lua/%.o: third_party/lua/%.c
	@mkdir -p $(@D)
	$(CC) $(LUA_CFLAGS) -c -o $@ $<

$(PROTO_OBJS): %.o: %.c | $(PROTO_HEADERS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

$(WM_OBJS): build/%.o: src/%.c | $(PROTO_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

$(CORE_OBJS): build/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(LUA_HOST_OBJS): build/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(XKB_CFLAGS) -c -o $@ $<

build/protocols/xdg-shell-protocol.h: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) server-header $< $@

build/protocols/wlr-layer-shell-unstable-v1-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) server-header $< $@

build/protocols/wlr-layer-shell-unstable-v1-client-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $@

build/protocols/wlr-layer-shell-unstable-v1-protocol.c: protocols/wlr-layer-shell-unstable-v1.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $< $@

build/protocols/xdg-shell-client-protocol.h: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $@

build/protocols/xdg-shell-protocol.c: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $< $@

build/protocols/viewporter-client-protocol.h: $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $@

build/protocols/viewporter-protocol.c: $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $< $@

build/protocols/fractional-scale-v1-client-protocol.h: $(WAYLAND_PROTOCOLS)/staging/fractional-scale/fractional-scale-v1.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $@

build/protocols/fractional-scale-v1-protocol.c: $(WAYLAND_PROTOCOLS)/staging/fractional-scale/fractional-scale-v1.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $< $@

build/tests/test_lua: tests/test_lua.c $(LUA_HOST_OBJS) $(LUA_OBJS) $(LIBTAM) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(XKB_CFLAGS) -o $@ $< $(LUA_HOST_OBJS) $(LUA_OBJS) $(LIBTAM) $(XKB_LIBS) -lm

build/tests/test_%: tests/test_%.c $(CORE_OBJS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(CORE_OBJS)

build/tests/test_input_headless: tests/test_input_headless.c $(CORE_OBJS) \
	$(LUA_HOST_OBJS) $(filter-out build/main.o,$(WM_OBJS)) $(PROTO_OBJS) \
	$(LUA_OBJS) $(LIBTAM) $(CLIENT_PROTO_HEADERS) $(CLIENT_PROTO_SRCS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Ibuild/protocols -o $@ $< \
		$(CLIENT_PROTO_SRCS) $(CORE_OBJS) $(LUA_HOST_OBJS) \
		$(filter-out build/main.o,$(WM_OBJS)) $(PROTO_OBJS) $(LUA_OBJS) \
		$(LIBTAM) $(PKG_LIBS) \
		$(shell $(PKG_CONFIG) --libs $(CLIENT_PKGS)) -lm

test: $(TESTS) build/tam-pane build/tests/test_input_headless
	@for t in $(TESTS); do $$t || exit 1; done
	@if nm -D build/tam-pane | grep -Eq ' luaL?_'; then \
		echo "ERROR: build/tam-pane exports dynamic lua_* symbols"; exit 1; \
	fi
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
