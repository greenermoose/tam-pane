/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Line-based IPC over a same-UID Unix domain socket
 * ($XDG_RUNTIME_DIR/tam-pane/<socket>.sock) and the `tam-pane msg` client.
 *
 * Pure formatting, escaping, socket path, and command-parsing helpers live in
 * src/ipc_proto.c (no wlroots dependency) so unit tests exercise the exact
 * wire logic without a display. Server-side socket and event fan-out live in
 * src/ipc.c.
 */
#ifndef TAM_PANE_IPC_H
#define TAM_PANE_IPC_H

#include "tam-pane/bindings.h"
#include "tam-pane/events.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tam.h>

struct tp_server;
struct tp_ipc_server;

enum {
	TP_IPC_MAX_LINE = 4096,
	TP_IPC_MAX_OUTBUF = 65536,
	TP_IPC_MAX_CLIENTS = 64,
};

/*
 * Pure protocol helpers (src/ipc_proto.c, no wlroots).
 */

/* Build "$runtime_dir/tam-pane/$wayland_socket.sock" into out, checking both
 * out_size and sockaddr_un.sun_path limits. Returns false on invalid input or
 * overflow. */
bool tp_ipc_socket_path(char *out, size_t out_size, const char *runtime_dir,
						const char *wayland_socket);

/* Append val to s, escaping '\\', '\t', '\n', '\r' and scrubbing other ASCII
 * control bytes so every record stays strictly on one line. Returns the
 * updated tam_str_t (or NULL on OOM). */
tam_str_t tp_ipc_escape_append(tam_str_t s, const char *val);

/* Append a formatted "event\t<kind>\t...\n" line for ev to s. Returns the
 * updated tam_str_t (or NULL on OOM). */
tam_str_t tp_ipc_format_event(tam_str_t s, const struct tp_event *ev);

/* Parse arguments to `subscribe [event ...]` into a bitmask of
 * (1u << enum tp_event_kind). Empty or "all" selects all subscribable events.
 * Returns false and writes err on unknown event names. */
bool tp_ipc_parse_subscribe(const char *args, uint32_t *out_mask, char *err,
							size_t err_size);

/* Parse arguments to `dispatch <action> [args...]` into *out. For
 * `monitor focus <name>` or `monitor move <name>`, out->dir is set to 0 and
 * the monitor name is written to mon_name (if non-NULL). Returns false and
 * writes err on invalid syntax or arguments. */
bool tp_ipc_parse_dispatch(const char *args, struct tp_action *out,
						   char *mon_name, size_t mon_name_size, char *err,
						   size_t err_size);

/* Format a human-readable action description (for `binds` output). */
tam_str_t tp_ipc_format_action(tam_str_t s, const struct tp_action *act);

/* CLI entry point for `tam-pane msg [-S socket] <command> [args...]`. */
int tp_ipc_client_main(int argc, char *argv[]);

/*
 * Server-side IPC lifecycle and event delivery (src/ipc.c).
 */

/* Create $XDG_RUNTIME_DIR/tam-pane (0700), bind <socket>.sock (0600), export
 * TAM_PANE_SOCKET, and attach the listener to s->loop. */
bool tp_ipc_start(struct tp_server *s);

/* Disconnect all IPC clients, remove event sources, and unlink the socket. */
void tp_ipc_finish(struct tp_server *s);

/* True when at least one IPC client is subscribed to kind. */
bool tp_ipc_has_subscribers(const struct tp_server *s, enum tp_event_kind kind);

/* Deliver a drained compositor event to all matching IPC subscribers. */
void tp_ipc_emit_event(struct tp_server *s, const struct tp_event *ev);

#endif
