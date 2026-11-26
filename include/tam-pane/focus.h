/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_FOCUS_H
#define TAM_PANE_FOCUS_H

#include "tam-pane/geom.h"

struct tp_server;
struct tp_client;
struct tp_layer_surface;
struct tp_monitor;

/* Give keyboard focus to `c` (NULL clears focus) and raise it. */
void tp_focus(struct tp_server *s, struct tp_client *c);

/* Give keyboard focus to a layer surface (exclusive or on-demand). */
void tp_focus_layer(struct tp_server *s, struct tp_layer_surface *ls);

/* The focused window, or NULL. */
struct tp_client *tp_focused(struct tp_server *s);

/* The most recently focused window visible on `m`, or NULL. */
struct tp_client *tp_focus_top(struct tp_server *s, struct tp_monitor *m);

/* Focus whatever should have focus on the selected monitor now. */
void tp_focus_refresh(struct tp_server *s);

/* Focus the next (dir > 0) or previous visible window on the monitor. */
void tp_focus_cycle(struct tp_server *s, int dir);

/* Focus the nearest visible window in cardinal direction `dir`. */
void tp_focus_dir(struct tp_server *s, enum tp_dir dir);

#endif
