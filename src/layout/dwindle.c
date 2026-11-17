/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/layout/dwindle.h"
#include <stdlib.h>

static float clamp_ratio(float r) {
	if (r != r) /* NaN */
		return 0.5f;
	if (r < TP_DWINDLE_RATIO_MIN)
		return TP_DWINDLE_RATIO_MIN;
	return r > TP_DWINDLE_RATIO_MAX ? TP_DWINDLE_RATIO_MAX : r;
}

static bool is_leaf(const struct tp_dwindle_node *n) {
	return !n->child[0];
}

static void free_subtree(struct tp_dwindle_node *n) {
	if (!n)
		return;
	free_subtree(n->child[0]);
	free_subtree(n->child[1]);
	free(n);
}

static struct tp_dwindle_node *find(struct tp_dwindle_node *n,
									const void *item) {
	if (!n)
		return NULL;
	if (is_leaf(n))
		return n->item == item ? n : NULL;
	struct tp_dwindle_node *hit = find(n->child[0], item);
	return hit ? hit : find(n->child[1], item);
}

static struct tp_dwindle_node *last_leaf(struct tp_dwindle_node *n) {
	while (n && !is_leaf(n))
		n = n->child[1];
	return n;
}

/* Point whatever referred to `old` (its parent or the root) at `repl`. */
static void replace_in_parent(struct tp_dwindle *t, struct tp_dwindle_node *old,
							  struct tp_dwindle_node *repl) {
	struct tp_dwindle_node *p = old->parent;
	repl->parent = p;
	if (!p)
		t->root = repl;
	else
		p->child[p->child[0] == old ? 0 : 1] = repl;
}

void tp_dwindle_init(struct tp_dwindle *t) {
	t->root = NULL;
	t->width_bias = 1.0f;
	t->preserve_split = false;
}

void tp_dwindle_clear(struct tp_dwindle *t) {
	free_subtree(t->root);
	t->root = NULL;
}

struct tp_dwindle_split tp_dwindle_auto_split(const struct tp_dwindle *t,
											  struct tp_rect target,
											  bool have_point, double px,
											  double py, float ratio) {
	struct tp_dwindle_split s = {0};
	s.split_h = (float)target.w > (float)target.h * t->width_bias;
	s.ratio = ratio;
	if (have_point)
		s.before = s.split_h ? px < target.x + target.w / 2.0
							 : py < target.y + target.h / 2.0;
	return s;
}

struct tp_dwindle_split tp_dwindle_smart_split(struct tp_rect target,
											   double px, double py,
											   float ratio) {
	struct tp_dwindle_split s = {.lock = true, .ratio = ratio};
	double cx = target.x + target.w / 2.0, cy = target.y + target.h / 2.0;
	double dx = px - cx, dy = py - cy;
	/* Compare the pointer's slope with the box's diagonal, as Hyprland does
	 * (|dy/dx| < h/w), multiplied out so a zero delta never divides. */
	double adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
	s.split_h = ady * target.w < adx * target.h;
	s.before = s.split_h ? dx < 0 : dy < 0;
	return s;
}

bool tp_dwindle_insert(struct tp_dwindle *t, void *item, void *near,
					   struct tp_dwindle_split split) {
	if (!item || find(t->root, item))
		return false;

	struct tp_dwindle_node *leaf = calloc(1, sizeof(*leaf));
	if (!leaf)
		return false;
	leaf->item = item;

	if (!t->root) {
		t->root = leaf;
		return true;
	}

	struct tp_dwindle_node *target = near ? find(t->root, near) : NULL;
	if (!target)
		target = last_leaf(t->root);

	struct tp_dwindle_node *inner = calloc(1, sizeof(*inner));
	if (!inner) {
		free(leaf);
		return false;
	}
	inner->ratio = clamp_ratio(split.ratio);
	inner->split_h = split.split_h;
	inner->locked = split.lock || t->preserve_split;
	inner->box = target->box;

	replace_in_parent(t, target, inner);
	inner->child[split.before ? 0 : 1] = leaf;
	inner->child[split.before ? 1 : 0] = target;
	leaf->parent = target->parent = inner;
	return true;
}

bool tp_dwindle_remove(struct tp_dwindle *t, void *item) {
	struct tp_dwindle_node *leaf = find(t->root, item);
	if (!leaf)
		return false;

	struct tp_dwindle_node *parent = leaf->parent;
	if (!parent) {
		t->root = NULL;
	} else {
		/* The sibling, with all its own ratios, takes the parent's place. */
		struct tp_dwindle_node *sib =
			parent->child[parent->child[0] == leaf ? 1 : 0];
		replace_in_parent(t, parent, sib);
		free(parent);
	}
	free(leaf);
	return true;
}

bool tp_dwindle_swap(struct tp_dwindle *t, void *a, void *b) {
	struct tp_dwindle_node *na = find(t->root, a), *nb = find(t->root, b);
	if (!na || !nb)
		return false;
	na->item = b;
	nb->item = a;
	return true;
}

struct tp_dwindle_node *tp_dwindle_find(const struct tp_dwindle *t,
										const void *item) {
	return find(t->root, item);
}

static size_t collect(const struct tp_dwindle_node *n, void **out, size_t max,
					  size_t at) {
	if (!n)
		return at;
	if (is_leaf(n)) {
		if (out && at < max)
			out[at] = n->item;
		return at + 1;
	}
	at = collect(n->child[0], out, max, at);
	return collect(n->child[1], out, max, at);
}

size_t tp_dwindle_count(const struct tp_dwindle *t) {
	return collect(t->root, NULL, 0, 0);
}

size_t tp_dwindle_items(const struct tp_dwindle *t, void **out, size_t max) {
	size_t n = collect(t->root, out, max, 0);
	return n < max ? n : max;
}

static void layout_node(struct tp_dwindle *t, struct tp_dwindle_node *n,
						struct tp_rect box, int32_t gap,
						tp_dwindle_place_fn place, void *data) {
	n->box = box;
	if (is_leaf(n)) {
		place(n->item, box, data);
		return;
	}
	if (!n->locked)
		n->split_h = (float)box.w > (float)box.h * t->width_bias;

	struct tp_rect a = box, b = box;
	int32_t span = n->split_h ? box.w : box.h;
	int32_t g = span > 2 * gap ? gap : 0;
	int32_t first = (int32_t)((float)(span - g) * n->ratio);
	if (first < 1)
		first = 1;
	int32_t second = span - g - first;
	if (second < 1)
		second = 1;
	if (n->split_h) {
		a.w = first;
		b.x = box.x + first + g;
		b.w = second;
	} else {
		a.h = first;
		b.y = box.y + first + g;
		b.h = second;
	}
	layout_node(t, n->child[0], a, gap, place, data);
	layout_node(t, n->child[1], b, gap, place, data);
}

void tp_dwindle_layout(struct tp_dwindle *t, struct tp_rect area,
					   int32_t gap, tp_dwindle_place_fn place, void *data) {
	if (t->root)
		layout_node(t, t->root, area, gap < 0 ? 0 : gap, place, data);
}
