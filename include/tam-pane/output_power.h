/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * zwlr_output_power_manager_v1 protocol (0.2.0): DPMS output power control.
 */
#ifndef TAM_PANE_OUTPUT_POWER_H
#define TAM_PANE_OUTPUT_POWER_H

#include <stdbool.h>

struct tp_server;

bool tp_output_power_init(struct tp_server *s);
void tp_output_power_finish(struct tp_server *s);

#endif
