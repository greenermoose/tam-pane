/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_LAYOUT_ARRANGE_H
#define TAM_PANE_LAYOUT_ARRANGE_H

struct tp_server;
struct tp_monitor;

/* Lay out and show or hide every window on `m`. */
void tp_arrange(struct tp_monitor *m);
void tp_arrange_all(struct tp_server *s);

#endif
