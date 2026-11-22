/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Foreign toplevel protocols (0.2.0):
 *   - zwlr_foreign_toplevel_manager_v1 (window lists and window control)
 *   - ext_foreign_toplevel_list_v1 (standard read-only window list)
 */
#ifndef TAM_PANE_FOREIGN_TOPLEVEL_H
#define TAM_PANE_FOREIGN_TOPLEVEL_H

#include <stdbool.h>

struct tp_server;
struct tp_client;
struct wlr_output;

bool tp_foreign_toplevel_init(struct tp_server *s);
void tp_foreign_toplevel_finish(struct tp_server *s);

/* Lifecycle hooks called by client.c and focus.c. */
void tp_foreign_toplevel_map(struct tp_client *c);
void tp_foreign_toplevel_unmap(struct tp_client *c);
void tp_foreign_toplevel_update_info(struct tp_client *c);
void tp_foreign_toplevel_set_activated(struct tp_client *c, bool activated);
void tp_foreign_toplevel_set_fullscreen(struct tp_client *c, bool fullscreen);
void tp_foreign_toplevel_set_output(struct tp_client *c,
									struct wlr_output *output);

#endif
