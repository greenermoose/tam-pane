# Wayland protocols (0.2.0)

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
- **Exclusive OVERLAY keyboard focus (0.1.18):** reserved for the
  configured shell connection (`tp_server.shell_client`). Ordinary
  layer-shell use (`BACKGROUND`, `BOTTOM`, `TOP`, and non-exclusive
  `OVERLAY` surfaces) remains available to direct same-UID clients.
- **Per-role grants:**
  - **Session lock (0.2.0)** (`ext_session_lock_manager_v1`): granted
    exclusively to the locker connection launched by `tam-pane` via
    `tp.lock(...)` / `tp_session_lock_launch*` (pre-created `socketpair`
    + `wl_client_create` + `WAYLAND_SOCKET`, tracked on
    `tp_server.locker_client`). Hidden from all other clients, including
    direct same-UID connections.
  - **Raw capture (0.2.1)** (`zwlr_screencopy_manager_v1`,
    `ext_image_copy_capture_manager_v1`, and capture-source companions):
    granted to the portal backend plus any explicitly launched
    screenshot tool.

## Active globals (0.2.0)

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
| `wp_security_context_manager_v1` | privileged | Sandbox engines register restricted listen sockets; hidden from sandboxed clients per spec |
| `zwlr_layer_shell_v1` | privileged | Panels, bars, wallpapers, launchers; exclusive OVERLAY keyboard focus is reserved for the configured shell connection |
| `zwlr_foreign_toplevel_manager_v1` | privileged | Window list and activation/fullscreen/close control for bars and shells |
| `ext_foreign_toplevel_list_v1` | privileged | Standard read-only window list |
| `ext_workspace_manager_v1` | privileged | Per-monitor tag groups exposed as workspaces (`1`..`9`) |
| `ext_session_lock_manager_v1` | privileged | Granted exclusively to the locker launched by `tam-pane` via `WAYLAND_SOCKET` |
| `ext_idle_notifier_v1` | privileged | Idle notifications for lock/power daemons |
| `zwlr_output_power_manager_v1` | privileged | DPMS output power control |

## Planned P3 globals (pre-classified in `src/security.c`)

| Interface | Class | Target | Notes |
| --- | --- | --- | --- |
| `zwlr_screencopy_manager_v1` | privileged | 0.2.1 | Raw capture: portal backend + explicitly launched screenshot tool |
| `ext_image_copy_capture_manager_v1` | privileged | 0.2.1 | Raw capture: portal backend + explicitly launched screenshot tool |
| `ext_output_image_capture_source_manager_v1` | privileged | 0.2.1 | Capture-source companion for raw capture |
| `ext_foreign_toplevel_image_capture_source_manager_v1` | privileged | 0.2.1 | Capture-source companion for raw capture |
| `zwlr_output_manager_v1` | privileged | 0.2.1 | Monitor layout configuration |
| `zwlr_gamma_control_manager_v1` | privileged | 0.2.1 | Night light / gamma ramps |
| `zwlr_data_control_manager_v1` | privileged | 0.2.2 | Unfocused clipboard access (`wl-clipboard`) |
| `ext_data_control_manager_v1` | privileged | 0.2.2 | Replacement clipboard protocol |
| `zwp_input_method_manager_v2` | privileged | 0.2.2 | Input method daemon (`fcitx5`) |
