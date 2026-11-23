# Data structures (0.2.5)

This file describes the structures as they are built today. Update it in
the same commit as any change to them.

## tp_server (`include/tam-pane/server.h`)

- **What it is:** the one object that holds all compositor state. It is
  created on `main`'s stack and passed to every module. There are no
  globals.
- **It holds:**
  - the wlroots core: display, event loop, backend, renderer, allocator
    and compositor;
  - the scene, with eight layer trees (`enum tp_layer`) from background
    to `TP_LAYER_LOCK` (above `TP_LAYER_OVERLAY`, 0.2.0);
  - the output layout;
  - lists of monitors, clients, the focus stack and keyboards,
    `focused_client` (`struct tp_client *`, explicit focused window kept
    separately from the seat's keyboard surface so popup grabs do not
    lose window focus, 0.1.19), and `selmon`, the monitor that receives
    new windows;
  - the listeners, grouped by the module that owns them;
  - `grab` (`struct tp_pointer_grab`), explicit state for an active
    `Meta+Left/Right` pointer move or resize drag (no globals). A tiled
    drag (`struct tp_layout_drag`, 0.2.5) wraps engine-specific drag state
    (dwindle split nodes, master-stack `mfact`, or scroller column width)
    with a generation counter. When `tp_layout_drag_valid` fails, the drag
    begins again from the pointer, so it never touches freed state (0.1.5).
  - `layer_shell` (`struct wlr_layer_shell_v1`) and `focused_layer`
    (`struct tp_layer_surface *`) for exclusive or on-demand layer keyboard
    focus.
  - `security_context_mgr` (`struct wlr_security_context_manager_v1`),
    `shell_client` (`struct wl_client *`, cleared on disconnect via
    `shell_client_destroy`), `locker_client` (`struct wl_client *`,
    cleared on disconnect via `locker_client_destroy`, 0.2.0), and
    `capture_clients` (`struct wl_list` of authorized raw-capture
    connections, each automatically removed on disconnect, 0.2.1) for the
    global filter and per-role policies (`src/security.c`,
    `arch/protocols.md`). Privileged globals are denied to sandboxed
    clients and cross-UID connections, exclusive OVERLAY keyboard focus is
    reserved for `shell_client`, `ext_session_lock_manager_v1` is granted
    exclusively to `locker_client`, and raw capture globals are granted
    only to `capture_clients`.
  - P3 shell protocol managers (0.2.0): `foreign_toplevel_mgr` and
    `ext_foreign_toplevel_list` (`src/foreign_toplevel.c`),
    `workspace_mgr` (`src/workspace.c`), `session_lock_mgr` +
    `active_lock` + `lock_surfaces` + `lock_bg_tree` + `locked`
    (`src/session_lock.c`), `idle_notifier` + `idle_inhibit_mgr` +
    `idle_inhibitors` (`src/idle.c`), and `output_power_mgr`
    (`src/output_power.c`).
  - P3 screens and capture managers (0.2.1): `screencopy_mgr`,
    `image_copy_capture_mgr`, `output_capture_source_mgr`, and
    `toplevel_capture_source_mgr` (`src/capture.c`); `xdg_output_mgr`,
    `output_mgr` (with `output_mgmt_applying` guard), and `gamma_mgr`
    wired to `wlr_scene` (`src/output_mgmt.c`); and `cursor_shape_mgr`
    with `cursor_shape_name` (`src/input/pointer.c`).
  - P3 text, activation, clipboard, and decoration managers (0.2.2):
    `text_input_mgr`, `input_method_mgr`, and `im_relay` (`src/text_input.c`,
    deactivated and keyboard grabs bypassed while `s->locked`);
    `xdg_activation` (`src/xdg_activation.c`, ignored while `s->locked`);
    `primary_selection_mgr` (wired to `request_set_primary_selection` in
    `src/input/input.c`), `data_control_mgr`, and `ext_data_control_mgr`;
    and `xdg_decoration_mgr` (`src/decoration.c`, server-side enforced on
    tiled windows, tracked per client in `tp_client.decoration`).
  - P3 IPC server (0.2.3): `ipc` (`struct tp_ipc_server *`, `src/ipc.c`,
    wire helpers and `tam-pane msg` client in `src/ipc_proto.c`, spec in
    `arch/ipc.md`). Owns the non-blocking Unix socket at
    `$XDG_RUNTIME_DIR/tam-pane/<socket>.sock` (`TAM_PANE_SOCKET`),
    per-client read/write buffers (capped at `TP_IPC_MAX_OUTBUF` with
    slow-subscriber eviction), `SO_PEERCRED` same-UID check, and
    per-event-kind subscriber counts. `SIGHUP` shuts down cleanly like
    `SIGTERM`/`SIGINT` (R1 finding 17); reloads use `tam-pane msg reload`
    or `tp.dsp.reload()`.
  - `arrange_idle` (`struct wl_event_source *`, `src/layout/arrange.c`,
    0.2.4): single server-owned Wayland idle source armed by `tp_arrange()`
    and `tp_arrange_all()`, coalescing multiple layout mutations into one
    arrange pass per dirty monitor per idle (`tp_arrange_flush()`), with a
    single post-batch `tp_idle_update_inhibit()` and
    `tp_pointer_refocus_now()` call and teardown cleanup in
    `tp_arrange_finish()`.
- **Compositor keys:** `change_vt` switches VT for Ctrl+Alt+F-keys. It is
  set only when `session` exists (DRM), and tests may substitute their
  own. `quit_requested` is set by `tp_server_quit`, which the emergency
  quit (Ctrl+Alt+Shift+Backspace) also calls (0.1.9).
- **Listener ownership:** each module removes its own listeners in its
  `*_finish` function, before `tp_server_finish` destroys anything.
- **Settings:** `settings` (`struct tp_settings`) holds every user setting
  in one small value struct (`gap_inner`, `gap_outer`, `split_ratio`,
  `width_bias`, `preserve_split`, `smart_split`, `proportional_split`,
  `resize_step`, fixed-buffer `terminal` and `cursor_theme`, `cursor_size`,
  `repeat_rate`, `repeat_delay`). Populated from compiled-in defaults and
  overlaid atomically from `~/.config/tam-pane/init.lua` via `lua`.
- **Lua runtime:** `lua` (`struct tp_lua *`, `include/tam-pane/lua.h`,
  `src/lua.c`, `src/lua_bindings.c`, `src/lua_events.c`, `src/lua_rules.c`)
  owns the embedded Lua 5.5 `lua_State`, watchdog state (instruction budget
  and monotonic deadline, also checked by the patched pattern matcher),
  the memory count of its counting allocator (`mem_used`, capped at
  `TP_LUA_MEMORY_MAX` inside guarded calls), the spawn handlers
  (`spawn_fn` for `tp.spawn(cmd)`, `spawn_argv_fn` for
  `tp.spawn{...}`), staged and active key-binding tables
  (`active_binds` / `staged_binds`, seeded from Lua defaults on creation),
  staged and active event hook lists (`active_hooks` / `staged_hooks`),
  staged and active window-rule lists (`active_rules` / `staged_rules`,
  matching via a cached `string.find` registry reference and committing
  atomically with settings, bindings, and hooks), and the compositor action
  callback (`tp_action_run`). Stored in `LUA_REGISTRYINDEX` by
  lightuserdata key; no globals and no wlroots dependency.
- **Deferred event queue:** `events` (`struct tp_event_queue`,
  `include/tam-pane/server_events.h`, `src/events.c`) holds a heap-backed
  ring buffer, capped at 256 entries, of `struct tp_queued_event`: a plain
  value snapshot (`struct tp_event`) plus its cascade `depth`. Events are
  enqueued whenever Lua hooks or IPC subscribers exist for that event kind,
  and drained to both (`src/events.c`, 0.2.3).
  - Events the compositor raises (depth 0) are drained by a one-shot
    `idle` source after the wlroots callbacks return.
  - Events raised while hooks run (`draining`) are one round deeper. They
    are drained by the `timer` source, so epoll runs between batches, and
    past `TP_EVENT_CASCADE_MAX` (16) rounds they are dropped with a
    warning (0.1.6).
  - The `startup` event is emitted once by `tp_server_start` (0.1.7).

## tp_monitor (`include/tam-pane/monitor.h`)

- **`area`:** the whole output, in layout coordinates.
- **`usable`:** `area` minus panels' exclusive zones (`tp_layers_arrange`).
- **`layers`:** `tp_layer_surface` list bound to this monitor.
- **`focus`:** remembered window per single tag, plus shared slot 0 for
  combined views. Focus changes update only the active slot; invalid
  candidates fall back to the most recently focused visible window.
- **`view`:** the monitor's tag view (below).
- **`layouts`:** `TP_TAG_SLOTS` `struct tp_layout` slots (`include/tam-pane/layout/layout.h`,
  0.2.5), one per single tag plus slot 0 for multi-tag views. Each slot
  holds its active `enum tp_layout_kind` (`dwindle`, `master`, `scroller`,
  or `monocle`) plus the per-engine state so switching tags or layouts
  preserves split ratios, `mfact`/`nmaster`, and scroller column widths.
  `tp_monitor_layout()` returns the slot for the current view.
- **`workspaces`:** opaque `struct tp_monitor_workspaces *`
  (`src/workspace.c`, 0.2.0) holding this monitor's
  `ext_workspace_group_handle_v1` and 9 `ext_workspace_handle_v1` entries.
- **`arrange_dirty` / `arrange_count`:** per-monitor dirty flag and
  monotonic completed-arrange counter (`src/layout/arrange.c`, 0.2.4) used
  to coalesce repeated `tp_arrange()` calls onto one pass per idle.

## tp_layer_surface (`include/tam-pane/layer.h`)

- One `wlr_layer_surface_v1` (panel, bar, launcher, or notification) bound to
  a `tp_monitor`.
- **Fields:** `mon`, `layer_surface`, `scene` (`wlr_scene_layer_surface_v1`,
  with `node.data` left `NULL` so `tp_client_at` never confuses it with a
  toplevel), `popup_tree` (in `TP_LAYER_OVERLAY`, synced to the surface's
  layout position), `mapped`, current `layer`, and its five listeners.
- **Arrangement:** `tp_layers_arrange()` runs two passes (exclusive surfaces
  first, then non-exclusive) from `OVERLAY` down to `BACKGROUND` to shrink
  `tp_monitor.usable`.

## tp_client (`include/tam-pane/client.h`)

- One managed toplevel window (`TP_CLIENT_XDG` via `wlr_xdg_toplevel`, or
  `TP_CLIENT_XWAYLAND` via `wlr_xwayland_surface` when built with
  `XWAYLAND=1`, 0.2.6). Client-kind accessors (`tp_client_surface`,
  `tp_client_app_id`, `tp_client_title`, `tp_client_is_xwayland`,
  `tp_client_set_activated`) and shared `tp_client_apply_map` /
  `tp_client_apply_unmap` pipelines keep the rest of the compositor
  kind-agnostic. Override-redirect X11 surfaces live on `TP_LAYER_OVERLAY`
  as `tp_xwayland_unmanaged` outside `tp_server.clients`.
- **Fields:** its monitor; `home_output` (remembered output name when
  displaced by hot-plug disconnect, cleared when reclaimed or explicitly moved
  to another monitor); its tag mask; whether it is mapped, floating,
  fullscreen, or in the scratchpad (`scratchpad`, `scratchpad_visible`,
  0.2.5); its current and floating geometry; its scene tree; and its
  listeners.
- **Kept small on purpose.** Effects, animations and Lua state will live
  in their own modules, keyed by the client, not as more fields.
- **Two lists:**
  - `link`, in `tp_server.clients`, newest first. Layout order uses this
    list.
  - `flink`, in `tp_server.focus_stack`, most recently focused first.
- **Fullscreen across map (0.1.12):** `on_map` honours a fullscreen
  request made before the window mapped, and `on_unmap` clears the flag,
  so a remapped window follows the client's current request and is never
  left marked fullscreen in the tile or float layer.
- **Popups (0.1.13):** an xdg popup rides on its parent's scene tree. On
  its first commit it is unconstrained against its window's monitor
  (`usable`, or `area` when the window is fullscreen), in the root
  toplevel's surface coordinates, so menus near an edge flip or slide
  onto the screen. It is unconstrained again on each
  `xdg_popup.reposition`, which GTK 4 uses to move a popover (0.1.15,
  B7). Layer-surface popups do the same in `src/layer.c`.
- **Floating place (0.1.17):** every window keeps `float_geom`, the
  place Meta+V floats it to. A window that opens tiled gets half its
  monitor, centred; a window that opens floating keeps its own size if
  it is at least 64x64. Every place is centred unless a rule gives
  coordinates. `tp_client_carry_float` moves the place with the window,
  tiled or not, and a place off the window's monitor counts as a first
  float (B9).
- **Monitor membership (0.1.10):** every change of `mon` goes through
  `tp_client_attach` (`src/client.c`). It first calls `tp_client_detach`,
  which drops the window from its monitor's `focus` slots and layout
  trees, so a monitor never keeps a window that left. The callers are
  `on_map`, `tp_client_set_monitor`, a floating drag in `update_grab`,
  and hot-plug in `src/monitor.c`. `tp_client_carry_float` moves the
  floating place between usable areas, and `tp_client_clear_home`
  forgets `home_output`. `on_unmap` detaches without changing `mon`.

## Tags (`include/tam-pane/tags.h`)

- **`tp_tags_t`:** a 32-bit mask; tags 1 to 9 are bits 0 to 8.
- **`tp_tagview`:** two slots (current and previous) and the index of the
  current one.
  - `show` swaps to the other slot and sets it.
  - `back` swaps.
  - `toggle` edits the current slot.
- **Refusals in one place:** an empty view, or showing the view already
  shown, is refused.
- **Visibility:** a window is visible when `tags & current` is not zero.
- **Tree choice:** `tp_tags_slot(view)` is the tag number for a single-tag
  view, and 0 otherwise.

## The dwindle tree (`include/tam-pane/layout/dwindle.h`)

- **Nodes:** leaves hold an opaque `item` (a `tp_client` in the
  compositor; a small integer in tests). Inner nodes hold:
  - `ratio`, the first child's share, from 0.05 to 0.95;
  - `split_h`, true when the children are side by side;
  - `locked`, true when the orientation is never re-chosen;
  - `box`, the area assigned at the last layout.
- **Generation:** `gen` changes whenever the tree frees a node (remove,
  clear), so holders of node pointers can tell when they may be stale
  (0.1.5).
- **Insert `x` next to `y`:** `y`'s leaf is replaced by a new inner node,
  with children `x` and `y` (or `y` and `x`). When `y` is unknown, the
  last leaf is used.
- **Remove `x`:** `x`'s sibling, with its whole subtree and ratios, takes
  the parent's place.
- **Layout:**
  - A depth-first pass. An unlocked node re-chooses its orientation as
    `w > h * width_bias`.
  - The span minus the gap is split by `ratio`, and both parts are at
    least one pixel.
  - Each leaf is reported through the `place` callback.
- **Choosing a split:**
  - `tp_dwindle_auto_split` (longer side; the pointer's half decides
    which side comes first);
  - `tp_dwindle_smart_split` (the pointer's direction from the centre
    picks the edge, and the orientation is locked).
- **Resizing and moving:**
  - `tp_dwindle_resize` adjusts the nearest split ancestor in a direction
    (falling back to the nearest ancestor on that axis at screen edges);
  - `tp_dwindle_drag_begin` / `tp_dwindle_drag_update` capture and update
    the nearest horizontal and vertical splits during a pointer resize;
  - `tp_dwindle_neighbor`, `tp_dwindle_swap_dir`, and `tp_dwindle_move_dir`
    find adjacent leaves geometrically, swap them, or re-insert a window
    next to its neighbor;
  - `proportional_split` (optional, off by default) scales same-direction
    sibling runs proportionally on insert and restores shares on remove.
- **Cost:** insert and remove are O(n) in the number of windows, because
  of the find. Layout is O(n). With tens of windows this is nothing; a
  pointer from client to leaf would make find O(1) if it is ever needed.

## Arrange (`src/layout/arrange.c`)

For one monitor, arrange:

1. Shows or hides every window by tag. Fullscreen and floating windows
   are placed directly.
2. Syncs the current tree: it drops leaves that are no longer visible and
   tiled on this monitor, and inserts newcomers, oldest first, next to the
   most recently focused window that is already in the tree. A window
   being mapped is already at the top of the focus stack, so the plain
   focus top would be the newcomer itself (fixed in 0.1.11, R1 finding
   12).
3. Lays the tree out over `usable` minus the outer gap.
4. Refreshes pointer focus, because windows may have moved under a still
   cursor.
