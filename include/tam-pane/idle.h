/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Idle notification and inhibition (0.2.0):
 *   - ext_idle_notifier_v1 (privileged)
 *   - zwp_idle_inhibit_manager_v1 (unprivileged, visibility-gated)
 */
#ifndef TAM_PANE_IDLE_H
#define TAM_PANE_IDLE_H

#include <stdbool.h>

struct tp_server;

bool tp_idle_init(struct tp_server *s);
void tp_idle_finish(struct tp_server *s);

/* Notify user activity on s->seat (called on keyboard and pointer events). */
void tp_idle_notify_activity(struct tp_server *s);

/* Re-evaluate whether any active idle inhibitor belongs to a visible surface. */
void tp_idle_update_inhibit(struct tp_server *s);

#endif
