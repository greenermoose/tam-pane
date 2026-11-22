/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Output management, logical output geometry, and gamma control (0.2.1):
 *   - zxdg_output_manager_v1 (unprivileged, logical layout for grim/slurp)
 *   - zwlr_output_manager_v1 (privileged, runtime output configuration)
 *   - zwlr_gamma_control_manager_v1 (privileged, wired to wlr_scene)
 */
#ifndef TAM_PANE_OUTPUT_MGMT_H
#define TAM_PANE_OUTPUT_MGMT_H

#include <stdbool.h>

struct tp_server;

bool tp_output_mgmt_init(struct tp_server *s);
void tp_output_mgmt_finish(struct tp_server *s);

/* Broadcast the current output configuration to zwlr_output_manager_v1 clients. */
void tp_output_mgmt_update(struct tp_server *s);

#endif
