# P3: Daily-driver parity (0.2.x)

**Status:** not started.

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

## Acceptance

- Each protocol is checked with a real client: Quickshell, grim,
  xdg-desktop-portal-wlr, swaylock or hyprlock, fcitx5, wl-clipboard.
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
