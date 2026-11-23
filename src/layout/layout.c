/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/layout/layout.h"
#include <string.h>

const char *tp_layout_kind_name(enum tp_layout_kind kind) {
	switch (kind) {
	case TP_LAYOUT_DWINDLE:
		return "dwindle";
	case TP_LAYOUT_MASTER:
		return "master";
	case TP_LAYOUT_SCROLLER:
		return "scroller";
	case TP_LAYOUT_MONOCLE:
		return "monocle";
	case TP_LAYOUT_COUNT:
		break;
	}
	return "dwindle";
}

bool tp_layout_kind_from_name(const char *name, enum tp_layout_kind *out) {
	if (!name || !out)
		return false;
	if (strcmp(name, "dwindle") == 0) {
		*out = TP_LAYOUT_DWINDLE;
		return true;
	}
	if (strcmp(name, "master") == 0) {
		*out = TP_LAYOUT_MASTER;
		return true;
	}
	if (strcmp(name, "scroller") == 0) {
		*out = TP_LAYOUT_SCROLLER;
		return true;
	}
	if (strcmp(name, "monocle") == 0) {
		*out = TP_LAYOUT_MONOCLE;
		return true;
	}
	return false;
}

void tp_layout_init(struct tp_layout *l, const struct tp_settings *cfg) {
	if (!l)
		return;
	tp_dwindle_init(&l->dwindle);
	tp_master_init(&l->master);
	tp_scroller_init(&l->scroller);
	tp_monocle_init(&l->monocle);
	l->kind = TP_LAYOUT_DWINDLE;
	if (cfg) {
		if (cfg->layout >= 0 && cfg->layout < TP_LAYOUT_COUNT)
			l->kind = (enum tp_layout_kind)cfg->layout;
		l->master.mfact = cfg->master_ratio;
		l->master.nmaster = cfg->master_count;
		l->scroller.default_width = cfg->scroller_width;
		tp_layout_apply_settings(l, cfg);
	}
}

void tp_layout_clear(struct tp_layout *l) {
	if (!l)
		return;
	tp_dwindle_clear(&l->dwindle);
	tp_master_clear(&l->master);
	tp_scroller_clear(&l->scroller);
	tp_monocle_clear(&l->monocle);
}

void tp_layout_apply_settings(struct tp_layout *l,
							  const struct tp_settings *cfg) {
	if (!l || !cfg)
		return;
	l->dwindle.width_bias = cfg->width_bias;
	l->dwindle.preserve_split = cfg->preserve_split;
	l->dwindle.proportional_split = cfg->proportional_split;
	l->scroller.default_width = cfg->scroller_width;
}

bool tp_layout_set_kind(struct tp_layout *l, enum tp_layout_kind kind) {
	if (!l || kind >= TP_LAYOUT_COUNT)
		return false;
	if (l->kind == kind)
		return false;
	l->kind = kind;
	return true;
}

enum tp_layout_kind tp_layout_cycle(struct tp_layout *l, int dir) {
	if (!l)
		return TP_LAYOUT_DWINDLE;
	int cur = (int)l->kind;
	int step = dir < 0 ? -1 : 1;
	int next = (cur + step + TP_LAYOUT_COUNT) % TP_LAYOUT_COUNT;
	l->kind = (enum tp_layout_kind)next;
	return l->kind;
}

bool tp_layout_adjust_nmaster(struct tp_layout *l, int delta) {
	if (!l)
		return false;
	return tp_master_adjust_nmaster(&l->master, delta);
}

size_t tp_layout_count(const struct tp_layout *l) {
	if (!l)
		return 0;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE:
		return tp_dwindle_count(&l->dwindle);
	case TP_LAYOUT_MASTER:
		return tp_master_count(&l->master);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_count(&l->scroller);
	case TP_LAYOUT_MONOCLE:
		return tp_monocle_count(&l->monocle);
	case TP_LAYOUT_COUNT:
		break;
	}
	return 0;
}

bool tp_layout_contains(const struct tp_layout *l, const void *item) {
	if (!l || !item)
		return false;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE:
		return tp_dwindle_find(&l->dwindle, item) != NULL;
	case TP_LAYOUT_MASTER:
		return tp_master_contains(&l->master, item);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_contains(&l->scroller, item);
	case TP_LAYOUT_MONOCLE:
		return tp_monocle_contains(&l->monocle, item);
	case TP_LAYOUT_COUNT:
		break;
	}
	return false;
}

size_t tp_layout_items(const struct tp_layout *l, void **out, size_t max) {
	if (!l || !out || max == 0)
		return 0;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE:
		return tp_dwindle_items(&l->dwindle, out, max);
	case TP_LAYOUT_MASTER:
		return tp_master_items(&l->master, out, max);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_items(&l->scroller, out, max);
	case TP_LAYOUT_MONOCLE:
		return tp_monocle_items(&l->monocle, out, max);
	case TP_LAYOUT_COUNT:
		break;
	}
	return 0;
}

bool tp_layout_insert(struct tp_layout *l, void *item, void *near,
					  const struct tp_layout_insert_ctx *ctx) {
	if (!l || !item)
		return false;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE: {
		struct tp_dwindle_node *at =
			near ? tp_dwindle_find(&l->dwindle, near) : NULL;
		struct tp_rect box =
			at ? at->box
			   : (ctx ? ctx->fallback_box : (struct tp_rect){0, 0, 100, 100});
		double px = ctx ? ctx->cursor_x : 0.0;
		double py = ctx ? ctx->cursor_y : 0.0;
		float ratio = ctx ? ctx->split_ratio : 0.5f;
		bool over = ctx && tp_rect_contains(box, px, py);
		struct tp_dwindle_split split =
			(ctx && ctx->smart_split && over)
				? tp_dwindle_smart_split(box, px, py, ratio)
				: tp_dwindle_auto_split(&l->dwindle, box, over, px, py, ratio);
		return tp_dwindle_insert(&l->dwindle, item, near, split);
	}
	case TP_LAYOUT_MASTER:
		return tp_master_insert(&l->master, item);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_insert(&l->scroller, item, near);
	case TP_LAYOUT_MONOCLE:
		return tp_monocle_insert(&l->monocle, item);
	case TP_LAYOUT_COUNT:
		break;
	}
	return false;
}

bool tp_layout_remove(struct tp_layout *l, void *item) {
	if (!l || !item)
		return false;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE:
		return tp_dwindle_remove(&l->dwindle, item);
	case TP_LAYOUT_MASTER:
		return tp_master_remove(&l->master, item);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_remove(&l->scroller, item);
	case TP_LAYOUT_MONOCLE:
		return tp_monocle_remove(&l->monocle, item);
	case TP_LAYOUT_COUNT:
		break;
	}
	return false;
}

void tp_layout_forget(struct tp_layout *l, void *item) {
	if (!l || !item)
		return;
	tp_dwindle_remove(&l->dwindle, item);
	tp_master_remove(&l->master, item);
	tp_scroller_remove(&l->scroller, item);
	tp_monocle_remove(&l->monocle, item);
}

struct dwindle_place_wrap {
	tp_layout_place_fn place;
	void *data;
};

static void dwindle_place_trampoline(void *item, struct tp_rect box,
									 void *data) {
	struct dwindle_place_wrap *w = data;
	w->place(item, box, true, w->data);
}

void tp_layout_layout(struct tp_layout *l, struct tp_rect area, int32_t gap,
					  const void *focused, tp_layout_place_fn place,
					  void *data) {
	if (!l || !place)
		return;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE: {
		struct dwindle_place_wrap wrap = {.place = place, .data = data};
		tp_dwindle_layout(&l->dwindle, area, gap, dwindle_place_trampoline,
						  &wrap);
		break;
	}
	case TP_LAYOUT_MASTER:
		tp_master_layout(&l->master, area, gap, place, data);
		break;
	case TP_LAYOUT_SCROLLER:
		tp_scroller_layout(&l->scroller, area, gap, focused, place, data);
		break;
	case TP_LAYOUT_MONOCLE:
		tp_monocle_layout(&l->monocle, area, focused, place, data);
		break;
	case TP_LAYOUT_COUNT:
		break;
	}
}

bool tp_layout_swap_dir(struct tp_layout *l, void *item, enum tp_dir dir) {
	if (!l || !item)
		return false;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE:
		return tp_dwindle_swap_dir(&l->dwindle, item, dir);
	case TP_LAYOUT_MASTER:
		return tp_master_swap_dir(&l->master, item, dir);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_swap_dir(&l->scroller, item, dir);
	case TP_LAYOUT_MONOCLE:
		return tp_monocle_swap_dir(&l->monocle, item, dir);
	case TP_LAYOUT_COUNT:
		break;
	}
	return false;
}

bool tp_layout_move_dir(struct tp_layout *l, void *item, enum tp_dir dir,
						float split_ratio) {
	if (!l || !item)
		return false;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE:
		return tp_dwindle_move_dir(&l->dwindle, item, dir, split_ratio);
	case TP_LAYOUT_MASTER:
		return tp_master_move_dir(&l->master, item, dir);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_move_dir(&l->scroller, item, dir);
	case TP_LAYOUT_MONOCLE:
		return tp_monocle_move_dir(&l->monocle, item, dir);
	case TP_LAYOUT_COUNT:
		break;
	}
	return false;
}

bool tp_layout_resize(struct tp_layout *l, const void *item, enum tp_dir dir,
					  int32_t delta) {
	if (!l || !item)
		return false;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE:
		return tp_dwindle_resize(&l->dwindle, item, dir, delta);
	case TP_LAYOUT_MASTER:
		return tp_master_resize(&l->master, item, dir, delta);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_resize(&l->scroller, item, dir, delta);
	case TP_LAYOUT_MONOCLE:
	case TP_LAYOUT_COUNT:
		break;
	}
	return false;
}

bool tp_layout_drag_begin(struct tp_layout *l, const void *item, double px,
						  double py, struct tp_layout_drag *drag) {
	if (!l || !item || !drag)
		return false;
	memset(drag, 0, sizeof(*drag));
	drag->kind = l->kind;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE:
		return tp_dwindle_drag_begin(&l->dwindle, item, px, py,
									 &drag->u.dwindle);
	case TP_LAYOUT_MASTER:
		return tp_master_drag_begin(&l->master, item, &drag->u.master);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_drag_begin(&l->scroller, item, &drag->u.scroller);
	case TP_LAYOUT_MONOCLE:
	case TP_LAYOUT_COUNT:
		break;
	}
	return false;
}

bool tp_layout_drag_valid(const struct tp_layout_drag *drag,
						  const struct tp_layout *l) {
	if (!drag || !l || drag->kind != l->kind)
		return false;
	switch (l->kind) {
	case TP_LAYOUT_DWINDLE:
		return tp_dwindle_drag_valid(&drag->u.dwindle, &l->dwindle);
	case TP_LAYOUT_MASTER:
		return tp_master_drag_valid(&drag->u.master, &l->master);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_drag_valid(&drag->u.scroller, &l->scroller);
	case TP_LAYOUT_MONOCLE:
	case TP_LAYOUT_COUNT:
		break;
	}
	return false;
}

bool tp_layout_drag_update(struct tp_layout_drag *drag, double dx, double dy) {
	if (!drag)
		return false;
	switch (drag->kind) {
	case TP_LAYOUT_DWINDLE:
		return tp_dwindle_drag_update(&drag->u.dwindle, dx, dy);
	case TP_LAYOUT_MASTER:
		return tp_master_drag_update(&drag->u.master, dx);
	case TP_LAYOUT_SCROLLER:
		return tp_scroller_drag_update(&drag->u.scroller, dx);
	case TP_LAYOUT_MONOCLE:
	case TP_LAYOUT_COUNT:
		break;
	}
	return false;
}
