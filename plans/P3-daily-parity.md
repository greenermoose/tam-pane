# P3: Daily-driver parity (0.2.x)

**Status:** in progress (0.2.0 accepted by Fred on 2026-11-22).

## Goal

Everything Fred's desktop needs from Hyprland exists in tam-pane: the
shell protocols, screen capture, input methods, IPC, the usual layouts,
and X11 programs.

## Versions

- **0.2.0** foreign-toplevel (window lists), ext-workspace (tags shown as
  workspaces to bars), session-lock, idle notify and inhibit, output
  power management.
- **0.2.1** screencopy and ext-image-copy-capture (screenshots,
  recording, portals), wlr-output-management, gamma control, fractional
  scale, viewporter, cursor-shape.
- **0.2.2** text-input v3 and input-method v2, xdg-activation, primary
  selection, data control, xdg-decoration (server side on tiled windows).
- **0.2.3** An IPC socket (`$XDG_RUNTIME_DIR/tam-pane/<socket>.sock`) with
  a line-based request and event format, written with libtam's string
  helpers, so no cJSON. A `tam-pane msg` client.
- **0.2.4** master-stack, scroller and monocle layouts, sharing the
  layout interface that dwindle uses. A scratchpad.
- **0.2.5** XWayland, behind a build option. It adds xcb as a temporary
  dependency.

## Design work from review R1

Review R1 (`reviews/R1-code-review-0.1.4.md`) left these for P3. Each
is designed before the version that needs it and lands with a test that
fails first.

- **Privileged-protocol filter (finding 9), done in 0.1.18.**
  `src/security.c` installs `wl_display_set_global_filter` with
  `wp_security_context_manager_v1`. Every global requires an explicit
  classification in `src/security.c` and `arch/protocols.md` (unknown
  globals fail closed). Privileged globals are denied to clients with a
  security context and to connections with a different peer UID; direct
  same-UID clients are allowed with no executable-path allowlist.
  Exclusive OVERLAY keyboard focus is reserved for the configured shell
  connection (`tp_server.shell_client`) while ordinary layer-shell use
  remains available. In P3, session lock is granted to the locker
  launched by tam-pane, and raw capture to the portal backend plus any
  explicitly launched screenshot tool.
- **One arrange per idle (finding 15), before 0.2.4.** `tp_arrange` runs
  several times per action, each with a `malloc`, quadratic finds and a
  pointer re-enter. A per-monitor dirty flag and one arrange per idle or
  frame saves work and wakeups. The new layouts in 0.2.4 should be
  written against that.
- **An explicit focused window (finding 16), done in 0.1.19.**
  `tp_server.focused_client` tracks the focused toplevel explicitly, and
  `tp_focused()` returns it directly instead of inferring focus from
  `s->seat->keyboard_state.focused_surface`. Window bindings and unmap
  focus restoration keep working while an xdg popup grab (or later
  session-lock / input-method) holds the seat's keyboard surface.
- **Smaller points (finding 17).**
  - A gate on bindings, which session-lock needs — **done in 0.2.0**
    (`tp.bind(chord, action, { locked = true })`, default gated while
    locked, and `Ctrl+Alt+Shift+Backspace` gated while locked).
  - Keyboard groups, so modifiers on one keyboard combine with keys on
    another.
  - NULL checks on the scene and output-layout helpers.
  - `validate_lua_pattern` misses patterns malformed only after their
    first item.
  - Event strings can be cut inside a UTF-8 sequence.
  - The nested backend's scale setup makes a blocking roundtrip.
  - `tp_monitor_cycle` passes a NULL-typed pointer to `wl_container_of`.
  - SIGHUP means reload, so closing the launching terminal reloads
    instead of exiting. Decide with the IPC socket (0.2.3), which can
    carry a reload request instead.
  - ON_DEMAND layer surfaces get no focus when they map.

## Acceptance

- Each protocol is checked with a real client: Quickshell, grim,
  xdg-desktop-portal-wlr, `tam-hide` (for screen locking; `tam-pane`
  manages screen blanking, and `hyprlock` is not used), fcitx5,
  wl-clipboard.
- The IPC format is documented in `arch/ipc.md`, with tests.

## Dependencies

- Adds (temporarily): xcb and xcb-icccm, for XWayland only.

## Risks

- **Protocol count.** Mango's `setup()` creates about 50 globals. We add
  only those a real client of Fred's needs, each with its reason in
  `arch/protocols.md`.

## Open points

- Which XWayland programs Fred still uses, which decides whether 0.2.5 is
  needed before dogfooding.
