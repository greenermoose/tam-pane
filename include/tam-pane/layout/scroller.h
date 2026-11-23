/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Scroller layout: a horizontal strip of columns (niri / MangoWM style).
 * Each column keeps its own width proportion of the viewport, opening a
 * window never shrinks existing columns, and the viewport scrolls to keep
 * the focused column visible. Pure logic with no compositor state.
 */
#ifndef TAM_PANE_LAYOUT_SCROLLER_H
#define TAM_PANE_LAYOUT_SCROLLER_H

#include "tam-pane/geom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TP_SCROLLER_WIDTH_MIN 0.15f
#define TP_SCROLLER_WIDTH_MAX 1.00f
#define TP_SCROLLER_WIDTH_DEFAULT 0.50f

typedef void (*tp_scroller_place_fn)(void *item, struct tp_rect box,
									 bool visible, void *data);

struct tp_scroller_col {
	void *item;
	float width_frac;
};

struct tp_scroller {
	struct tp_scroller_col *cols;
	size_t len;
	size_t cap;
	float default_width;
	int32_t scroll_x;
	struct tp_rect last_area;
	unsigned long gen;
};

struct tp_scroller_drag {
	struct tp_scroller *scroller;
	unsigned long gen;
	size_t index;
	float start_width;
};

void tp_scroller_init(struct tp_scroller *sc);
void tp_scroller_clear(struct tp_scroller *sc);

/* Insert `item` immediately after `near` (or at the end if `near` is absent). */
bool tp_scroller_insert(struct tp_scroller *sc, void *item, void *near);
bool tp_scroller_remove(struct tp_scroller *sc, void *item);
bool tp_scroller_contains(const struct tp_scroller *sc, const void *item);
size_t tp_scroller_count(const struct tp_scroller *sc);
size_t tp_scroller_items(const struct tp_scroller *sc, void **out, size_t max);

bool tp_scroller_swap_dir(struct tp_scroller *sc, void *item, enum tp_dir dir);
bool tp_scroller_move_dir(struct tp_scroller *sc, void *item, enum tp_dir dir);
bool tp_scroller_resize(struct tp_scroller *sc, const void *item,
						enum tp_dir dir, int32_t delta);

bool tp_scroller_drag_begin(struct tp_scroller *sc, const void *item,
							struct tp_scroller_drag *drag);
bool tp_scroller_drag_valid(const struct tp_scroller_drag *drag,
							const struct tp_scroller *sc);
bool tp_scroller_drag_update(struct tp_scroller_drag *drag, double dx);

void tp_scroller_layout(struct tp_scroller *sc, struct tp_rect area,
						int32_t gap, const void *focused,
						tp_scroller_place_fn place, void *data);

#endif
