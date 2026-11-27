/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ext-session-lock-v1 protocol (0.2.0):
 *   - Granted exclusively to the locker connection launched by tam-pane via
 *     WAYLAND_SOCKET (tp_session_lock_launch / tp_session_lock_launch_argv).
 *   - Covers all monitors with solid black rects in TP_LAYER_LOCK and disables
 *     non-lock layers while locked.
 *   - Fail-locked: if the locker crashes without unlocking, the session stays
 *     locked until a newly launched locker takes over and unlocks.
 */
#ifndef TAM_PANE_SESSION_LOCK_H
#define TAM_PANE_SESSION_LOCK_H

#include <stdbool.h>
#include <stddef.h>

struct tp_server;
struct wlr_surface;

bool tp_session_lock_init(struct tp_server *s);
void tp_session_lock_finish(struct tp_server *s);

/* Number of currently mapped ext-session-lock-v1 lock surfaces. */
size_t tp_session_lock_mapped_surface_count(const struct tp_server *s);

/* Resize black cover rects and reconfigure lock surfaces on output changes. */
void tp_session_lock_arrange(struct tp_server *s);

/* Focus a mapped lock surface (e.g. when clicked on another monitor). */
void tp_session_lock_focus_surface(struct tp_server *s,
								   struct wlr_surface *surface);

/* Launch a locker process over a pre-created WAYLAND_SOCKET and grant that
 * exact wl_client access to ext_session_lock_manager_v1. */
void tp_session_lock_launch(struct tp_server *s, const char *cmd);
bool tp_session_lock_launch_argv(struct tp_server *s, const char *const argv[]);

#endif
