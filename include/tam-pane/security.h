/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Privileged-protocol filter and shell-connection policy (0.1.18, R1 #9).
 *
 * Every Wayland global advertised by tam-pane must have an explicit
 * classification. Unclassified globals fail closed (treated as privileged).
 * Privileged globals are denied to clients carrying a wp_security_context_v1
 * sandbox context and to clients whose peer UID differs from the compositor's
 * own UID; direct same-UID clients are allowed.
 *
 * Exclusive OVERLAY keyboard focus is reserved for the configured shell
 * connection while ordinary layer-shell use remains available.
 */
#ifndef TAM_PANE_SECURITY_H
#define TAM_PANE_SECURITY_H

#include <stdbool.h>
#include <sys/types.h>

struct tp_server;
struct wl_client;

enum tp_global_class {
	TP_GLOBAL_UNKNOWN = 0,
	TP_GLOBAL_UNPRIVILEGED,
	TP_GLOBAL_PRIVILEGED,
};

/* Explicit classification for a Wayland global interface name. */
enum tp_global_class tp_security_classify_global(const char *interface_name);

/* Pure policy check: whether a client with the given sandbox state and UID
 * may see and bind privileged globals. */
bool tp_security_allow_privileged(bool has_security_context, uid_t peer_uid,
								  uid_t self_uid);

/* Create wp_security_context_manager_v1 and install the display global filter. */
bool tp_security_init(struct tp_server *s);

/* Remove security listeners before display teardown. */
void tp_security_finish(struct tp_server *s);

/* Designate (or clear, with NULL) the configured shell connection allowed to
 * take exclusive OVERLAY keyboard focus. Automatically cleared on disconnect. */
void tp_security_set_shell_client(struct tp_server *s,
								  struct wl_client *client);

/* Whether `client` may take exclusive keyboard focus on the OVERLAY layer. */
bool tp_security_allow_exclusive_overlay(const struct tp_server *s,
										 const struct wl_client *client);

/* Designate (or clear, with NULL) the locker connection allowed to see and
 * bind ext_session_lock_manager_v1. Automatically cleared on disconnect. */
void tp_security_set_locker_client(struct tp_server *s,
								   struct wl_client *client);

/* Whether `client` is the authorized locker connection. */
bool tp_security_allow_session_lock(const struct tp_server *s,
									const struct wl_client *client);

#endif
