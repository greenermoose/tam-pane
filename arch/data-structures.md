# Data structures (0.1.3)

This file describes the structures as they are built today. Update it in
the same commit as any change to them.

## tp_server (`include/tam-pane/server.h`)

- **What it is:** the one object that holds all compositor state. It is
  created on `main`'s stack and passed to every module. There are no
  globals.
- **It holds:**
  - the wlroots core: display, event loop, backend, renderer, allocator
    and compositor;
  - the scene, with seven layer trees (`enum tp_layer`) from background
    to overlay;
  - the output layout;
  - lists of monitors, clients, the focus stack and keyboards, and
    `selmon`, the monitor that receives new windows;
  - the listeners, grouped by the module that owns them;
  - `grab` (`struct tp_pointer_grab`), explicit state for an active
    `Meta+Left/Right` pointer move or resize drag (no globals). A tiled
    drag (`struct tp_dwindle_drag`) holds split-node pointers, plus the
    tree and generation it began on. When `tp_dwindle_drag_valid` fails,
    the drag begins again from the pointer, so it never touches a freed
    node (0.1.5).
  - `layer_shell` (`struct wlr_layer_shell_v1`) and `focused_layer`
    (`struct tp_layer_surface *`) for exclusive or on-demand layer keyboard
    focus.
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
  enqueued only when hooks exist for that event kind.
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
- **`trees`:** `TP_TAG_SLOTS` dwindle trees, one per single tag plus slot
  0, which is shared by every multi-tag view. `tp_monitor_tree()` returns
  the tree for the current view, so each tag keeps its own layout.

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

- One xdg toplevel.
- **Fields:** its monitor; `home_output` (remembered output name when
  displaced by hot-plug disconnect, cleared when reclaimed or explicitly moved
  to another monitor); its tag mask; whether it is mapped, floating or
  fullscreen; its current and floating geometry; its scene tree; and its
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
