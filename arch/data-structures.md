# Data structures (0.0.1)

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
  - the listeners, grouped by the module that owns them.
- **Listener ownership:** each module removes its own listeners in its
  `*_finish` function, before `tp_server_finish` destroys anything.
- **Settings:** `settings` (`struct tp_settings`) holds every user setting
  in one small struct. Compiled-in defaults now; Lua in P2.

## tp_monitor (`include/tam-pane/monitor.h`)

- **`area`:** the whole output, in layout coordinates.
- **`usable`:** `area` minus panels' exclusive zones (layer-shell, 0.0.5).
- **`view`:** the monitor's tag view (below).
- **`trees`:** `TP_TAG_SLOTS` dwindle trees, one per single tag plus slot
  0, which is shared by every multi-tag view. `tp_monitor_tree()` returns
  the tree for the current view, so each tag keeps its own layout.

## tp_client (`include/tam-pane/client.h`)

- One xdg toplevel.
- **Fields:** its monitor; its tag mask; whether it is mapped, floating or
  fullscreen; its current and floating geometry; its scene tree; and its
  listeners.
- **Kept small on purpose.** Effects, animations and Lua state will live
  in their own modules, keyed by the client, not as more fields.
- **Two lists:**
  - `link`, in `tp_server.clients`, newest first. Layout order uses this
    list.
  - `flink`, in `tp_server.focus_stack`, most recently focused first.

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
- **Cost:** insert and remove are O(n) in the number of windows, because
  of the find. Layout is O(n). With tens of windows this is nothing; a
  pointer from client to leaf would make find O(1) if it is ever needed.

## Arrange (`src/layout/arrange.c`)

For one monitor, arrange:

1. Shows or hides every window by tag. Fullscreen and floating windows
   are placed directly.
2. Syncs the current tree: it drops leaves that are no longer visible and
   tiled on this monitor, and inserts newcomers, oldest first, next to the
   most recently focused window.
3. Lays the tree out over `usable` minus the outer gap.
4. Refreshes pointer focus, because windows may have moved under a still
   cursor.
