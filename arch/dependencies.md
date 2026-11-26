# Dependency ledger

Every library tam-pane uses is listed here, with why it is needed and which
version removes it. The goal is libc, the kernel's headers, libtam, code
in this repository, and Mesa for the GPU (`arch/principles.md`). Update
this file in the same change as any dependency change.

## Direct dependencies (0.0.1)

`readelf -d build/tam-pane` lists exactly these as NEEDED.

- **libc, libm**
  - **Why:** standard C.
  - **Planned exit:** none; they are the goal.
- **libtam** (static, `$(LIBTAM_PREFIX)/lib/libtam.a`)
  - **Why:** logging (`tam_log_*`). More of it will be used as it grows.
  - **Planned exit:** none; it is the goal.
- **wlroots 0.20**
  - **Why:** backend, renderer, scene graph, and the protocol
    implementations.
  - **Planned exit:** P6, 0.7.x (libtam KMS and session backend, plus
    Mesa).
- **wayland-server**
  - **Why:** the Wayland wire protocol and the event loop.
  - **Planned exit:** P6, 0.6.x (libtam wire protocol).
- **xkbcommon**
  - **Why:** keymaps and key symbols.
  - **Planned exit:** P6, 0.5.x (libtam keymaps).

## Build-time only

- **make, a C17 compiler, pkg-config.** Plain Make; no other build system.
- **wayland-scanner** (from wayland) generates protocol headers.
  - **Planned exit:** P6, 0.6.x, when our own scanner generates them.
- **wayland-protocols** supplies the XML files.
  - **Planned exit:** none needed. Any file we need can be carried in
    `protocols/`, as layer-shell already is.

## Pulled in through wlroots (not linked by tam-pane)

Arch's wlroots 0.20 package brings these at run time (seen with `ldd`):

- **Graphics:** libdrm, pixman, libEGL, libGLESv2, libgbm, libvulkan,
  liblcms2, libdisplay-info, libliftoff.
- **Devices and seat:** libudev, libseat, libinput (with libevdev, mtdev,
  libwacom and, through libinput's plugins, liblua 5.4).
- **X11 and the client library:** libxcb and several xcb extensions, and
  wayland-client. Arch builds wlroots with XWayland and its X11 and
  Wayland backends.
- **Other libraries:** libsystemd, glib and gudev (through libwacom),
  pcre2 (through glib), libffi, libexpat.

All of these go when wlroots goes (P6, 0.7.x). Mesa's libEGL, libGLESv2
and libgbm stay.

## Dropped from MangoWM's list

- **scenefx:** effects, replaced by our own module in P5.
- **pango and cairo:** title text.
- **cJSON:** IPC text, replaced by a line format in P3.
- **pcre2:** rule matching, replaced by Lua patterns in P2.
- **xcb and xcb-icccm:** XWayland. Not linked in default builds
  (`XWAYLAND=0`). Opt-in only via `make XWAYLAND=1` (0.2.6), temporary
  until all X11 apps are replaced by native Wayland apps.
- **meson and ninja.**

## Vendored in tree (no external dependency)

- **Lua 5.5.1** (`third_party/lua/`, added in 0.1.0): compiled as a static
  object set with `-fvisibility=hidden` and `-DLUA_USE_POSIX` (no
  `-rdynamic`, no `lua_*` symbols exported to avoid clashing with
  `libinput`'s `liblua 5.4`). Unsafe libraries (`io`, `os`, `package`,
  `debug`) are not compiled or linked. One marked patch in `lstrlib.c`
  (0.1.8) lets the watchdog stop the pattern matcher (`UPSTREAM.md`).

## Optional build-time dependencies (`XWAYLAND=1`, 0.2.6)

- **xcb, xcb-icccm**
  - **Why:** opt-in XWayland support (`make XWAYLAND=1`, `-DTP_XWAYLAND=1`).
    Default builds (`XWAYLAND=0`) do not link them (`readelf -d` shows no
    `libxcb` NEEDED).
  - **Planned exit:** removed once no X11 apps remain on Tamlinux.
