/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Monocle layout: every tiled window gets the full usable area, and only the
 * active (focused) tiled window is shown. Pure logic with no compositor state.
 */
#ifndef TAM_PANE_LAYOUT_MONOCLE_H
#define TAM_PANE_LAYOUT_MONOCLE_H

#include "tam-pane/geom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*tp_monocle_place_fn)(void *item, struct tp_rect box,
									bool visible, void *data);

struct tp_monocle {
	void **items;
	size_t len;
	size_t cap;
	void *active;
	unsigned long gen;
};

void tp_monocle_init(struct tp_monocle *mo);
void tp_monocle_clear(struct tp_monocle *mo);

bool tp_monocle_insert(struct tp_monocle *mo, void *item);
bool tp_monocle_remove(struct tp_monocle *mo, void *item);
bool tp_monocle_contains(const struct tp_monocle *mo, const void *item);
size_t tp_monocle_count(const struct tp_monocle *mo);
size_t tp_monocle_items(const struct tp_monocle *mo, void **out, size_t max);

bool tp_monocle_swap_dir(struct tp_monocle *mo, void *item, enum tp_dir dir);
bool tp_monocle_move_dir(struct tp_monocle *mo, void *item, enum tp_dir dir);

void tp_monocle_layout(struct tp_monocle *mo, struct tp_rect area,
					   const void *focused, tp_monocle_place_fn place,
					   void *data);

#endif
