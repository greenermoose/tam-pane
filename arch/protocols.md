# Wayland protocols (0.2.6)

Every Wayland global `tam-pane` advertises must be listed here with its
reason and its classification (`unprivileged` or `privileged`), and must
be classified in `src/security.c`. Unclassified globals fail closed
(treated as privileged), and `tests/test_input_headless.c` fails if the
compositor advertises any unclassified global.

Whenever a new global lands — including any capture-source companion
(`ext_*_image_capture_source_manager_v1`) or replacement clipboard
protocol (`ext_data_control_manager_v1`) — add its explicit
classification here and in `src/security.c` in the same commit.

## Access policy

- **Unprivileged globals:** offered to every client.
- **Privileged globals (0.1.18):** denied via `wl_display_set_global_filter`
  to any client with a `wp_security_context_v1` sandbox context and to
  any client whose peer UID differs from the compositor's own UID.
  Direct same-UID clients are allowed (no executable-path allowlist).
- **Exclusive OVERLAY keyboard focus:** allowed for direct same-UID,
  non-sandboxed clients (matching `TOP` + `EXCLUSIVE` and
  `zwlr_layer_shell_v1`). Sandboxed (`wp_security_context_v1`) and
  cross-UID clients are denied both at the global filter and at the focus
  check; `ext_session_lock_v1` stays above `OVERLAY` on `TP_LAYER_LOCK`
  and strictly gated to `locker_client`.
- **Per-role grants:**
  - **Session lock (0.2.0)** (`ext_session_lock_manager_v1`): granted
    exclusively to the locker connection launched by `tam-pane` via
    `tp.lock(...)` / `tp_session_lock_launch*` (pre-created `socketpair`
    + `wl_client_create` + `WAYLAND_SOCKET`, tracked on
    `tp_server.locker_client`). Hidden from all other clients, including
    direct same-UID connections.
  - **Raw capture (0.2.1)** (`zwlr_screencopy_manager_v1`,
    `ext_image_copy_capture_manager_v1`,
    `ext_output_image_capture_source_manager_v1`,
    `ext_foreign_toplevel_image_capture_source_manager_v1`): granted only
    to connections authorized via `tp.capture(...)` / `tp_capture_launch*`
    (pre-created `socketpair` + `wl_client_create` + `WAYLAND_SOCKET`,
    tracked in `tp_server.capture_clients`, automatically revoked on
    disconnect) — e.g. a portal backend started in `startup` plus
    explicitly launched screenshot tools (`grim`). Hidden from all other
    clients, including direct same-UID connections.
  - **XWayland shell (0.2.6, opt-in `XWAYLAND=1`)** (`xwayland_shell_v1`):
    granted exclusively to the compositor's own `Xwayland` server connection
    (`s->xwayland->server->client`). Hidden from all other clients,
    including direct same-UID connections. Not advertised in default
    `XWAYLAND=0` builds.

## Active globals (0.2.6)

| Interface | Class | Why it is here |
| --- | --- | --- |
| `wl_compositor` | unprivileged | Surfaces and regions (core Wayland) |
| `wl_subcompositor` | unprivileged | Subsurfaces (core Wayland) |
| `wl_shm` | unprivileged | Shared-memory buffers (core Wayland) |
| `wl_drm` | unprivileged | wlroots GPU buffer sharing |
| `zwp_linux_dmabuf_v1` | unprivileged | GPU dma-buf buffers |
| `wp_linux_drm_syncobj_manager_v1` | unprivileged | Explicit GPU sync (wlroots 0.20) |
| `wl_fixes` | unprivileged | libwayland display lifecycle fixes |
| `wl_data_device_manager` | unprivileged | Clipboard and drag-and-drop for focused clients |
| `wl_output` | unprivileged | Monitor geometry and scale |
| `wl_seat` | unprivileged | Keyboard and pointer input |
| `xdg_wm_base` | unprivileged | Application toplevels and popups |
| `wp_viewporter` | unprivileged | Surface cropping and scaling |
| `wp_fractional_scale_manager_v1` | unprivileged | HiDPI fractional scale reporting |
| `wp_presentation` | unprivileged | Frame presentation timing (GTK 4 nested, B6) |
| `zwp_idle_inhibit_manager_v1` | unprivileged | Visibility-gated idle inhibition for media players and browsers (ignored while locked) |
| `wp_cursor_shape_manager_v1` | unprivileged | Named pointer cursor shapes on the focused pointer seat |
| `zxdg_output_manager_v1` | unprivileged | Logical output geometry and names (`grim`, `slurp`, fractional-scale clients) |
| `zwp_text_input_manager_v3` | unprivileged | Text input state and preedit/commit delivery for focused clients (cleared while locked) |
| `xdg_activation_v1` | unprivileged | Token-based window activation across tags/monitors (ignored while locked) |
| `zwp_primary_selection_device_manager_v1` | unprivileged | Middle-click primary selection for focused clients |
| `zxdg_decoration_manager_v1` | unprivileged | CSD/SSD negotiation (server-side enforced on tiled windows) |
| `wp_security_context_manager_v1` | privileged | Sandbox engines register restricted listen sockets; hidden from sandboxed clients per spec |
| `zwlr_layer_shell_v1` | privileged | Panels, bars, wallpapers, launchers, and polkit prompts; direct same-UID non-sandboxed clients allowed (including exclusive OVERLAY focus) |
| `zwlr_foreign_toplevel_manager_v1` | privileged | Window list and activation/fullscreen/close control for bars and shells |
| `ext_foreign_toplevel_list_v1` | privileged | Standard read-only window list |
| `ext_workspace_manager_v1` | privileged | Per-monitor tag groups exposed as workspaces (`1`..`9`) |
| `ext_session_lock_manager_v1` | privileged | Granted exclusively to the locker launched by `tam-pane` via `WAYLAND_SOCKET` |
| `ext_idle_notifier_v1` | privileged | Idle notifications for lock/power daemons |
| `zwlr_output_power_manager_v1` | privileged | DPMS output power control |
| `zwlr_screencopy_manager_v1` | privileged | Raw output capture; granted only to `tp.capture` connections |
| `ext_image_copy_capture_manager_v1` | privileged | Standard frame capture; granted only to `tp.capture` connections |
| `ext_output_image_capture_source_manager_v1` | privileged | Output capture-source companion; granted only to `tp.capture` connections |
| `ext_foreign_toplevel_image_capture_source_manager_v1` | privileged | Foreign-toplevel capture-source companion; granted only to `tp.capture` connections (refused while locked) |
| `zwlr_output_manager_v1` | privileged | Runtime monitor layout, mode, scale, and transform configuration |
| `zwlr_gamma_control_manager_v1` | privileged | Night light / per-output gamma ramps (wired through `wlr_scene`) |
| `zwlr_data_control_manager_v1` | privileged | Unfocused clipboard and primary-selection access (`wl-clipboard`) |
| `ext_data_control_manager_v1` | privileged | Standard replacement clipboard protocol |
| `zwp_input_method_manager_v2` | privileged | Input method daemon (`fcitx5`); deactivated and keyboard grabs bypassed while locked |
| `zwp_keyboard_shortcuts_inhibit_manager_v1` | privileged | Focused-surface shortcut inhibition for Keybinding Explorer, VMs, and remote desktop clients; deactivated on focus loss or session lock; hard-wired VT/emergency-quit keys are never inhibited |
| `xwayland_shell_v1` | privileged | Opt-in `XWAYLAND=1` only; granted exclusively to `Xwayland` (`s->xwayland->server->client`) |
