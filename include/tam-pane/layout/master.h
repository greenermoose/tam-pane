/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Master-stack layout: the first `nmaster` windows stack vertically in the
 * left master pane (taking `mfact` of the width), and the remaining windows
 * stack vertically on the right. Pure logic with no compositor state.
 */
#ifndef TAM_PANE_LAYOUT_MASTER_H
#define TAM_PANE_LAYOUT_MASTER_H

#include "tam-pane/geom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TP_MASTER_MFACT_MIN 0.10f
#define TP_MASTER_MFACT_MAX 0.90f
#define TP_MASTER_MFACT_DEFAULT 0.55f
#define TP_MASTER_COUNT_MIN 1
#define TP_MASTER_COUNT_MAX 8
#define TP_MASTER_COUNT_DEFAULT 1

typedef void (*tp_master_place_fn)(void *item, struct tp_rect box, bool visible,
								   void *data);

struct tp_master {
	void **items;
	size_t len;
	size_t cap;
	float mfact;
	int nmaster;
	struct tp_rect last_area;
	unsigned long gen;
};

struct tp_master_drag {
	struct tp_master *master;
	unsigned long gen;
	float start_mfact;
};

void tp_master_init(struct tp_master *m);
void tp_master_clear(struct tp_master *m);

bool tp_master_insert(struct tp_master *m, void *item);
bool tp_master_remove(struct tp_master *m, void *item);
bool tp_master_contains(const struct tp_master *m, const void *item);
size_t tp_master_count(const struct tp_master *m);
size_t tp_master_items(const struct tp_master *m, void **out, size_t max);

bool tp_master_swap_dir(struct tp_master *m, void *item, enum tp_dir dir);
bool tp_master_move_dir(struct tp_master *m, void *item, enum tp_dir dir);
bool tp_master_resize(struct tp_master *m, const void *item, enum tp_dir dir,
					  int32_t delta);
bool tp_master_adjust_nmaster(struct tp_master *m, int delta);

bool tp_master_drag_begin(struct tp_master *m, const void *item,
						  struct tp_master_drag *drag);
bool tp_master_drag_valid(const struct tp_master_drag *drag,
						  const struct tp_master *m);
bool tp_master_drag_update(struct tp_master_drag *drag, double dx);

void tp_master_layout(struct tp_master *m, struct tp_rect area, int32_t gap,
					  tp_master_place_fn place, void *data);

#endif
