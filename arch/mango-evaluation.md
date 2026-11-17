# MangoWM evaluation

- **Read:** `~/Code/mango` at commit `5ba74a95`, 2026-11-17.
- **Purpose:** understand how MangoWM works and what it depends on, then
  decide what tam-pane takes and what it must not repeat. The ideas list
  lives in `ideas/mangowm.md`; this file is the architecture.

## Dependencies

| Library | What uses it | Verdict for tam-pane |
| --- | --- | --- |
| wlroots 0.20 | everything | keep for now; replaced in P6 (0.7.x) |
| wayland-server | everything | keep for now; replaced in P6 (0.6.x) |
| libinput | `src/input/*`, `src/config/*` (device options) | P1 uses wlroots' libinput backend without linking libinput directly; replaced in P6 (0.5.x) |
| xkbcommon | `src/input/keyboard.c` | keep for now; replaced in P6 (0.5.x) |
| scenefx 0.5 | 26 files, including `manage/client.c`, `monitor.c`, `layer.c` | dropped; our own effects module in P5 |
| pango and cairo | `src/draw/text-node.c` (title bars) | dropped; titles without pango in P5 |
| cJSON | `include/mango/ipc/ipc.h`, `src/ipc/ipc.c` | dropped; line format in P3 |
| pcre2 | `src/common/util.c` (window rules) | dropped; Lua patterns in P2 |
| xcb, xcb-icccm | `src/manage/xwayland_primary.c`, `client.c` | deferred to P3 (0.2.5), as a temporary dependency |
| libdrm, pixman | output setup, regions | only through wlroots |
| meson, ninja | the build | dropped; plain Make |

## How it starts and runs

- `main()` in `src/main.c` parses arguments and calls `setup()`.
  - `setup()` (around 470 lines from line 464) creates the backend,
    renderer, allocator and scene.
  - It then creates about 50 Wayland globals in one function: compositor,
    shells, data devices, capture, gamma, idle, session lock, input
    method, foreign toplevel, and more.
- `run()` adds the socket, starts the backend, sets the activation
  environment for systemd and D-Bus, spawns the startup command, then
  `wl_display_run()`.
- `cleanup()` removes the listeners first (`cleanup_listeners`), then
  destroys objects. tam-pane follows the same order.

## Scene layers

- `include/mango/common/server.h` defines 17 scene layers:
  - the usual ones: `LyrBg`, `LyrBottom`, `LyrTile`, `LyrFloat`, `LyrTop`,
    `LyrFullscreen`, `LyrOverlay`;
  - one for blur (`LyrBlur`) and one for fade-out animations
    (`LyrFadeOut`);
  - a full parallel set for the special workspace (`LyrSpecial*`).
- tam-pane starts with seven layers, from background to overlay. Effects
  and the scratchpad add layers in their own phases only if they need
  them.

## Client lifecycle

- New toplevel, then its first commit (a configure is sent), then map:
  rules are applied, a monitor and tags are chosen, and the window is
  arranged and focused.
- Unmap removes the window from the layout and refocuses; destroy frees
  it.
- `struct Client` (`include/mango/manage/client.h`) has 155 member lines.
  Window management, XWayland, animation, opacity, blur, overview,
  scratchpad, swallowing and per-window effect switches all live in one
  struct.

## Tags

- Each `Monitor` has `uint32_t tagset[2]` and `seltags`, as in dwm. Each
  `Client` has `uint32_t tags`.
- `VISIBLEON(c, m)` tests `c->tags & m->tagset[m->seltags]`, with
  exceptions for global and overlay windows.
- `struct Pertag` keeps per-tag layout state: the dwindle root, nmaster,
  mfact, scroller state and layout choice. There are `tag_num_MAX + 1`
  slots, and slot 0 is for multi-tag views.

## Layouts and the dwindle tree

- `arrange()` in `src/layout/arrange.c` (line 1418) counts visible
  windows and dispatches to the monitor's current layout function.
- `dwindle(m)` in `src/layout/dwindle.c`:
  1. Collects the visible tiled clients.
  2. Removes tree leaves that are no longer among them.
  3. Inserts newcomers next to the focused client
     (`dwindle_insert_with_config`).
  4. Assigns boxes recursively (`dwindle_assign`).
- Split orientation is chosen when a split is first assigned (`w >= h`),
  unless it is locked.
- Optional "manual split" arithmetic keeps a same-direction run in equal
  shares when windows come and go.

## Configuration, bindings, IPC

- `src/config/` holds its own parser for `.conf` and TOML.
  - The options table `option_defs.c` is 3,019 lines.
  - `watcher.c` and `reapply.c` give hot reload.
- `src/dispatch/bind.c` (2,802 lines) holds every bindable action.
- `src/ipc/ipc.c` (1,425 lines) serves a Unix socket and builds JSON
  replies with cJSON. `mmsg` is the command-line client.

## What works well

- **Tags with a layout tree per tag.** Switching tags restores each tag's
  exact layout. Taken (`src/tags.c`, `tp_monitor.trees`).
- **The dwindle tree itself.** It is small and handles removal correctly.
  Taken and rewritten (`src/layout/dwindle.c`).
- **Teardown order: listeners first.** It prevents wlroots' destroy
  assertions. Followed (`tp_server_finish`).
- **The input split** into keyboard, pointer, touch, tablet and trackpad
  files. Followed (`src/input/`).
- **A session target for systemd user services.** Planned (P4).

## What not to repeat

- **Global state everywhere.**
  - `extern struct MangoServer server` (`server.h:256`) and
    `extern Config config` (`parse.h:598`) are used about 4,000 times.
  - Even the layout algorithm reads them, so nothing can be tested
    alone.
- **One struct for every concern.** `struct Client` mixes window
  management with effects, animation and overview state.
- **Effects inside core code.** scenefx calls sit in client, monitor and
  layer management.
- **Very large files.** `client.c` is 5,027 lines, `option_defs.c` 3,019,
  `bind.c` 2,802.
- **A library for every small job.** cJSON for IPC text, pcre2 for rule
  matching, pango and cairo for title text.
