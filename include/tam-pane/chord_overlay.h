/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Zero-dependency built-in chord composer overlay rendered onto
 * TP_LAYER_OVERLAY via wlr_scene_buffer + embedded bitmap font.
 */
#ifndef TAM_PANE_CHORD_OVERLAY_H
#define TAM_PANE_CHORD_OVERLAY_H

#include <stdbool.h>

struct tp_server;
struct tp_chord_overlay;

bool tp_chord_overlay_init(struct tp_server *s);
void tp_chord_overlay_finish(struct tp_server *s);

/* Redraw and show/hide the overlay to match s->chord and s->locked. */
void tp_chord_overlay_update(struct tp_server *s);

#endif
