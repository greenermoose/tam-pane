/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_LAYOUT_ARRANGE_H
#define TAM_PANE_LAYOUT_ARRANGE_H

struct tp_server;
struct tp_monitor;

/* Mark `m` (or every monitor) dirty and schedule one arrange on the next idle. */
void tp_arrange(struct tp_monitor *m);
void tp_arrange_all(struct tp_server *s);

/* Immediately lay out every dirty monitor and cancel any pending idle arrange. */
void tp_arrange_flush(struct tp_server *s);

/* Cancel any pending idle arrange during server teardown. */
void tp_arrange_finish(struct tp_server *s);

#endif
