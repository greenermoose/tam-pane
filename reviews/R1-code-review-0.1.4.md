# R1: code review of tam-pane 0.1.4

- **Date:** 2026-11-21
- **Reviewer:** Claude Code 2.1.330, claude-opus-5-5
- **Revision:** b244e85 (dev)
- **Scope:** all first-party code in `src/` and `include/` (about 6,600
  lines), with the tests read where a finding needed them. Not the
  vendored Lua 5.5 in `third_party/lua/`.

## Summary

The code is small, readable and consistently organized, and the pure
modules (tags, dwindle, the Lua host) are well tested. One finding was
reproduced as a crash-class memory error and is fixed (B1). Two more can
hang the compositor: a Lua hook feedback loop (2) and work inside Lua C
library calls (8). The Lua "sandbox" protects against hangs, not against
untrusted input: client window titles reach `/bin/sh` through `tp.spawn`
(7). Before Fred dogfoods it, tam-pane needs VT switching and a quit that
does not depend on Lua (10). Protocol gaps that ROADMAP P3 already
schedules are not findings here.

## Findings

### 1. Use-after-free during a tiled resize drag (critical)

- **Where:** `src/input/pointer.c` (`update_grab`), `src/layout/dwindle.c`
  (`tp_dwindle_remove`)
- **What:** A tiled drag holds raw split-node pointers. If another window
  leaves the tree mid-drag, the split it shared is freed, and the next
  motion reads and writes freed memory.
- **Evidence:** the new headless case in `tests/test_input_headless.c`,
  under AddressSanitizer: `heap-use-after-free` in
  `tp_dwindle_drag_update`.
- **Fix:** the tree's generation counter changes whenever it frees a node,
  and the drag records the tree and generation it began on; on a
  mismatch the drag begins again from the pointer.
- **Status:** became B1, fixed in 3b808fd (0.1.5), verified by Fred

### 2. A Lua hook feedback loop hangs the compositor (critical)

- **Where:** `src/events.c` (`drain_events_idle`)
- **What:** The drain re-adds itself as an idle source. libwayland runs
  idle sources until the list is empty, so a hook whose action emits
  the same event never returns to epoll. Examples: a `window.focus` hook
  that calls `tp.dsp.focus('next')()`, or a `tag.change` hook that
  changes tags. Each hook call stays within budget, so the watchdog
  never fires, and frames, input and SIGTERM all stop.
- **Evidence:** reading the code against libwayland's
  `wl_event_loop_dispatch_idle`. Not yet reproduced.
- **Fix:** drain from a zero-delay timer or eventfd so epoll runs between
  batches, and stop a cascade after a set number of generations.
- **Status:** became B2, fixed in 0735f35 (0.1.6), verified by Fred

### 3. `tp.spawn` runs while the config loads (high)

- **Where:** `src/lua.c` (`l_tp_spawn`), `src/main.c`
- **What:** An autostart line such as `tp.spawn("waybar")` in `init.lua`
  runs before `WAYLAND_DISPLAY` is set; nested, it opens on the host. It
  runs again on every reload, and it still runs when the config later
  fails, so the rollback is not atomic.
- **Fix:** refuse spawn while staging; add a startup hook or
  `tp.exec_once` that runs after the server starts.
- **Status:** became B3, fixed in 51b0425 (0.1.7), verified by Fred

### 4. Fullscreen state is wrong across map and remap (medium)

- **Where:** `src/client.c` (`on_map`, `on_unmap`)
- **What:** `on_map` ignores `requested.fullscreen`, so programs that start
  fullscreen do not. `on_unmap` keeps `fullscreen`, so a remapped window
  is reparented to the tile or float layer while still marked
  fullscreen.
- **Status:** fixed in ff5dc75 (0.1.12), verified by Fred: `on_map`
  applies `requested.fullscreen`, and `on_unmap` clears the flag. No
  bug record; the headless case covers both halves, and Fred's demo
  the first.

### 5. xdg popups are never unconstrained (medium)

- **Where:** `src/client.c` (`popup_commit`)
- **What:** Menus near a screen edge open off-screen. Layer popups already
  call `wlr_xdg_popup_unconstrain_from_box`.
- **Status:** fixed in 41ef9ad (0.1.13): `popup_commit` finds the root
  toplevel and unconstrains against its monitor in that toplevel's
  surface coordinates. Fred's check of 0.1.13 found B6 instead (Files
  crashed nested). His check of 0.1.14 found the menu still off-screen:
  the fix missed `xdg_popup.reposition`, so the finding became B7,
  fixed in c5ec224 (0.1.15), verified by Fred.

### 6. A floating move across monitors skips the bookkeeping (low)

- **Where:** `src/input/pointer.c` (`update_grab`, move path)
- **What:** It rewrites `c->mon` and `c->tags` without arranging either
  monitor, emitting `tag.change` or refreshing focus. This is a symptom
  of 13.
- **Status:** fixed in 0.1.10 (part of 13): the drag now re-lays out
  both monitors. On a second look the gap had no visible effect, so it
  has no bug record. The "emitting `tag.change`" above was wrong:
  `tag.change` reports a monitor's view, not a window's tags.

### 7. Window titles can reach the shell through `tp.spawn` (high)

- **Where:** `src/lua.c`, `src/spawn.c`, `arch/lua-api.md`
- **What:** The only spawn form is `/bin/sh -c`. Hooks receive
  client-controlled `title` and `app_id`, and the natural pattern (used
  in the tests) concatenates them into a command. Any client can set a
  title such as `'; rm -rf ~ #`.
- **Fix:** add `tp.spawn{argv...}` with no shell, recommend it for event
  data, and say in the docs that the sandbox does not make untrusted
  strings safe.
- **Status:** became B3, fixed in 51b0425 (0.1.7), verified by Fred

### 8. The watchdog misses work inside C calls (high)

- **Where:** `src/lua.c` (`luaL_newstate`), `src/lua_rules.c`
  (`match_lua_pattern`)
- **What:** The count hook runs only between VM instructions. One
  `string.rep("x", 2^31)` allocates gigabytes, because the allocator is
  unbounded. A rule pattern with several `.*` or `.-` items can backtrack
  for seconds in C against a long client title.
- **Fix:** a counting allocator with a cap, and rule subjects truncated to
  255 bytes as the events already are.
- **Status:** became B4, fixed in 9b5f27b (0.1.8), verified by Fred

### 9. There is no privileged-protocol filter (medium)

- **Where:** `src/server.c`
- **What:** Every global is offered to every client. Any client can take
  an exclusive-keyboard OVERLAY layer surface and imitate a lock screen.
  P3 adds screencopy and foreign-toplevel.
- **Fix:** design `wl_display_set_global_filter` (later
  security-context-v1) before P3.
- **Status:** scheduled in `plans/P3-daily-parity.md` (Design work
  from review R1), before 0.2.0

### 10. No VT switching and no Lua-independent quit (high)

- **Where:** `src/input/keyboard.c`
- **What:** Nothing calls `wlr_session_change_vt`. Every binding lives in
  Lua, so a config that calls `tp.clear_binds()` and binds no quit key
  leaves no way out short of SysRq.
- **Fix:** VT switching, and possibly one emergency quit, handled in C
  before Lua sees the key.
- **Status:** became B5, fixed in 92f1b11 (0.1.9); the quit is verified
  by Fred, the VT switch waits for the P4 hardware trial

### 11. A GPU reset ends the session (medium)

- **Where:** `src/server.c`
- **What:** tam-pane does not listen to `renderer->events.lost`.
- **Fix:** recreate the renderer and allocator and re-init the outputs, as
  dwl and sway do.
- **Status:** scheduled in `plans/P4-dogfood-tamlinux-0.7.md` (Design
  work from review R1), checked on real hardware

### 12. New windows split the last leaf, not the focused window (medium)

- **Where:** `src/client.c` (`on_map`), `src/layout/arrange.c`
  (`sync_tree`)
- **What:** `on_map` puts the new window at the top of the focus stack
  before it arranges. `sync_tree` then takes the new window as "near",
  finds it is not yet in the tree and falls back to the last leaf. The
  README and the `sync_tree` comment both say a new window opens next to
  the focused one.
- **Evidence:** found while writing the B1 test: with c0 focused in
  `[c0 | c1]`, a new window split c1.
- **Status:** fixed in 9c0bf49 (0.1.11), verified by Fred: `sync_tree`
  places newcomers next to the most recently focused window that is
  already in the tree. No bug record, because the effect is placement
  only.

### 13. Client/monitor membership is changed by hand in five places (medium)

- **Where:** `on_unmap`, `tp_client_set_monitor`, `tp_client_set_tags`,
  `update_grab` and `on_new_output`
- **What:** Each site clears `focus[]` slots, forgets the client from the
  trees and reassigns `mon`, `tags` and `home_output` slightly
  differently.
- **Fix:** one `tp_client_detach` / `tp_client_attach` pair that owns the
  invariant and emits the events.
- **Status:** fixed in 0.1.10: `tp_client_attach` / `tp_client_detach`.
  `tp_client_set_tags` keeps its own slot clearing, because it changes
  tags, not monitors.

### 14. Duplicated logic (low)

- **What:** The float clamp is written twice (`tp_client_set_monitor`,
  `clamp_float_to`), and so is the default floating size (`on_map`,
  `tp_client_set_floating`). The "now in ms" code is repeated,
  `first_tag_num` duplicates `tp_tags_first`, and there are three
  near-identical Lua integer validators.
- **Status:** fixed in 845b2c2 (0.1.16), accepted by Fred. The float clamp was merged in 0.1.10
  (`tp_client_carry_float`); 0.1.16 adds `default_float_size`,
  `tp_pointer_refocus_now` and `tp_lua_check_int`, and uses
  `tp_tags_first`. The test written for the validators found B8.

### 15. Layout runs eagerly (low)

- **What:** `tp_arrange` runs several times per action. Each run does a
  `malloc`, quadratic `find`s and a pointer re-enter. A dirty flag per
  monitor with one arrange per idle or frame would save work and
  wakeups.
- **Status:** fixed in 0.2.4 (`tp_monitor.arrange_dirty`,
  `tp_server.arrange_idle`, `tp_arrange_flush`, stack-buffered `sync_tree`)

### 16. `tp_focused()` infers focus from the seat (low)

- **What:** It returns NULL while an xdg popup grab holds the keyboard, so
  window bindings do nothing while a menu is open. An explicit focused
  client would be sturdier.
- **Status:** scheduled in `plans/P3-daily-parity.md` (Design work
  from review R1), with 0.2.0

### 17. Smaller points (low)

- **What:**
  - Bindings have no gate yet, and session-lock will need one.
  - Keyboards are not grouped, so modifiers on one keyboard do not
    combine with keys on another.
  - The scene and output-layout helpers are called without NULL checks.
  - `validate_lua_pattern` misses patterns that are malformed only after
    the first item.
  - Event strings can be cut in the middle of a UTF-8 sequence.
  - The nested backend makes a blocking roundtrip.
  - `tp_monitor_cycle` passes a NULL-typed pointer to `wl_container_of`.
  - SIGHUP means reload, so closing the launching terminal reloads
    instead of exiting.
  - ON_DEMAND layer surfaces get no focus on map.
  - The Makefile does not rebuild `main.o` and `lua.o` when `VERSION`
    changes, so a bumped build can still report the old version (seen
    while building 0.1.5).
- **Status:** the Makefile point is fixed in 0.1.8; the rest are
  scheduled in `plans/P3-daily-parity.md` (Design work from review R1)

## Limits

The review did not cover the vendored Lua or the Python nested test. It
did not run tam-pane nested or on DRM. Findings 2 to 17 come from
reading the code; only 1 and 12 were run.
