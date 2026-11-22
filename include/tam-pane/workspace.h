/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ext-workspace-v1 protocol (0.2.0): exposes each monitor's 9 tags as
 * workspaces in a per-monitor workspace group.
 */
#ifndef TAM_PANE_WORKSPACE_H
#define TAM_PANE_WORKSPACE_H

#include <stdbool.h>

struct tp_server;
struct tp_monitor;

bool tp_workspace_init(struct tp_server *s);
void tp_workspace_finish(struct tp_server *s);

void tp_workspace_monitor_init(struct tp_monitor *m);
void tp_workspace_monitor_finish(struct tp_monitor *m);
void tp_workspace_sync_monitor(struct tp_monitor *m);

#endif
