/* SPDX-License-Identifier: GPL-3.0-or-later */
/* xdg-decoration-v1: server-side decorations on tiled windows. */
#ifndef TAM_PANE_DECORATION_H
#define TAM_PANE_DECORATION_H

#include <stdbool.h>

struct tp_client;
struct tp_server;

bool tp_decoration_init(struct tp_server *s);
void tp_decoration_finish(struct tp_server *s);

/* Apply the decoration policy (server-side when tiled; honor client-side when
 * floating if requested) once the xdg_surface is initialized. */
void tp_decoration_update(struct tp_client *c);

/* Detach a client from its decoration handle when the client is destroyed. */
void tp_decoration_detach_client(struct tp_client *c);

#endif
