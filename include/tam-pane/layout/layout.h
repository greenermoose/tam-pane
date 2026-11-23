/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Unified layout interface shared by dwindle, master-stack, scroller, and
 * monocle. Each tag slot on a monitor owns one tp_layout; switching layout
 * kinds preserves each engine's state. Pure logic with no compositor state.
 */
#ifndef TAM_PANE_LAYOUT_LAYOUT_H
#define TAM_PANE_LAYOUT_LAYOUT_H

#include "tam-pane/geom.h"
#include "tam-pane/layout/dwindle.h"
#include "tam-pane/layout/master.h"
#include "tam-pane/layout/monocle.h"
#include "tam-pane/layout/scroller.h"
#include "tam-pane/settings.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum tp_layout_kind {
	TP_LAYOUT_DWINDLE = 0,
	TP_LAYOUT_MASTER,
	TP_LAYOUT_SCROLLER,
	TP_LAYOUT_MONOCLE,
	TP_LAYOUT_COUNT
};

typedef void (*tp_layout_place_fn)(void *item, struct tp_rect box, bool visible,
								   void *data);

struct tp_layout {
	enum tp_layout_kind kind;
	struct tp_dwindle dwindle;
	struct tp_master master;
	struct tp_scroller scroller;
	struct tp_monocle monocle;
};

struct tp_layout_insert_ctx {
	struct tp_rect fallback_box;
	double cursor_x;
	double cursor_y;
	bool smart_split;
	float split_ratio;
};

struct tp_layout_drag {
	enum tp_layout_kind kind;
	union {
		struct tp_dwindle_drag dwindle;
		struct tp_master_drag master;
		struct tp_scroller_drag scroller;
	} u;
};

const char *tp_layout_kind_name(enum tp_layout_kind kind);
bool tp_layout_kind_from_name(const char *name, enum tp_layout_kind *out);

void tp_layout_init(struct tp_layout *l, const struct tp_settings *cfg);
void tp_layout_clear(struct tp_layout *l);
void tp_layout_apply_settings(struct tp_layout *l,
							  const struct tp_settings *cfg);

bool tp_layout_set_kind(struct tp_layout *l, enum tp_layout_kind kind);
enum tp_layout_kind tp_layout_cycle(struct tp_layout *l, int dir);
bool tp_layout_adjust_nmaster(struct tp_layout *l, int delta);

size_t tp_layout_count(const struct tp_layout *l);
bool tp_layout_contains(const struct tp_layout *l, const void *item);
size_t tp_layout_items(const struct tp_layout *l, void **out, size_t max);
bool tp_layout_insert(struct tp_layout *l, void *item, void *near,
					  const struct tp_layout_insert_ctx *ctx);
bool tp_layout_remove(struct tp_layout *l, void *item);

/* Remove `item` from every layout engine in this slot. */
void tp_layout_forget(struct tp_layout *l, void *item);

void tp_layout_layout(struct tp_layout *l, struct tp_rect area, int32_t gap,
					  const void *focused, tp_layout_place_fn place,
					  void *data);

bool tp_layout_swap_dir(struct tp_layout *l, void *item, enum tp_dir dir);
bool tp_layout_move_dir(struct tp_layout *l, void *item, enum tp_dir dir,
						float split_ratio);
bool tp_layout_resize(struct tp_layout *l, const void *item, enum tp_dir dir,
					  int32_t delta);

bool tp_layout_drag_begin(struct tp_layout *l, const void *item, double px,
						  double py, struct tp_layout_drag *drag);
bool tp_layout_drag_valid(const struct tp_layout_drag *drag,
						  const struct tp_layout *l);
bool tp_layout_drag_update(struct tp_layout_drag *drag, double dx, double dy);

#endif
