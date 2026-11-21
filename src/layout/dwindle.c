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
	t->proportional_split = false;
	t->gen = 0;
}

void tp_dwindle_clear(struct tp_dwindle *t) {
	free_subtree(t->root);
	t->root = NULL;
	t->gen++;
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

/* Count items (leaves or perpendicular subtrees) in a same-direction run. */
static size_t count_block_items(const struct tp_dwindle_node *n, bool split_h) {
	if (!n)
		return 0;
	if (is_leaf(n) || n->split_h != split_h)
		return 1;
	return count_block_items(n->child[0], split_h) +
		   count_block_items(n->child[1], split_h);
}

/* Walk up contiguous same-direction ancestors starting at `start` and compute
 * each node's absolute share within that block (block root = 1.0). */
static size_t block_path_ratios(struct tp_dwindle_node *start, bool split_h,
								struct tp_dwindle_node ***path_out,
								float **p_out) {
	size_t depth = 1;
	for (struct tp_dwindle_node *cur = start->parent;
		 cur && cur->split_h == split_h; cur = cur->parent)
		depth++;

	struct tp_dwindle_node **path = calloc(depth, sizeof(*path));
	float *p = calloc(depth, sizeof(*p));
	if (!path || !p) {
		free(path);
		free(p);
		return 0;
	}

	size_t len = 0;
	path[len++] = start;
	for (struct tp_dwindle_node *cur = start->parent;
		 cur && cur->split_h == split_h; cur = cur->parent)
		path[len++] = cur;

	p[len - 1] = 1.0f;
	for (size_t i = len - 1; i > 0; i--) {
		struct tp_dwindle_node *s = path[i];
		struct tp_dwindle_node *child = path[i - 1];
		p[i - 1] = s->child[0] == child ? p[i] * s->ratio
										: p[i] * (1.0f - s->ratio);
	}
	*path_out = path;
	*p_out = p;
	return len;
}

/* Scale existing siblings in a same-direction block by (N-1)/N and return the
 * new split's ratio so the inserted item receives 1/N of the block. */
static float proportional_insert(struct tp_dwindle_node *target, bool split_h,
								 bool before, float fallback_ratio) {
	struct tp_dwindle_node **path = NULL;
	float *p = NULL;
	size_t len = block_path_ratios(target, split_h, &path, &p);
	if (!len)
		return fallback_ratio;

	size_t n_old = len > 1 ? count_block_items(path[len - 1], split_h) : 1;
	double n_new = (double)(n_old + 1);
	double keep = (n_new - 1.0) / n_new;
	double share = 1.0 / n_new;

	for (size_t i = len - 1; i > 0; i--) {
		struct tp_dwindle_node *s = path[i];
		struct tp_dwindle_node *child = path[i - 1];
		double p_s = p[i];
		double p_first = p_s * s->ratio;
		double p_s_new = p_s * keep + share;
		double p_first_new =
			s->child[0] == child ? p_first * keep + share : p_first * keep;
		s->ratio = clamp_ratio((float)(p_first_new / p_s_new));
	}

	double p_target = p[0];
	double p_split_new = p_target * keep + share;
	double ratio =
		before ? share / p_split_new : (p_target * keep) / p_split_new;

	free(path);
	free(p);
	return clamp_ratio((float)ratio);
}

/* Redistribute the share of `leaf` proportionally across same-direction
 * ancestors before removing `leaf` and `parent`. */
static void proportional_remove(struct tp_dwindle_node *parent,
								struct tp_dwindle_node *leaf) {
	struct tp_dwindle_node **path = NULL;
	float *p = NULL;
	size_t len = block_path_ratios(parent, parent->split_h, &path, &p);
	if (len <= 1) {
		free(path);
		free(p);
		return;
	}

	float p_del = p[0] * (parent->child[0] == leaf ? parent->ratio
												   : (1.0f - parent->ratio));
	if (p_del > 0.999f)
		p_del = 0.999f;

	for (size_t i = len - 1; i > 0; i--) {
		struct tp_dwindle_node *s = path[i];
		struct tp_dwindle_node *child = path[i - 1];
		float p_s = p[i];
		float p_first = p_s * s->ratio;
		float denom = p_s - p_del;
		if (denom < 0.0001f)
			denom = 0.0001f;
		float ratio = s->child[0] == child ? (p_first - p_del) / denom
										   : p_first / denom;
		s->ratio = clamp_ratio(ratio);
	}
	free(path);
	free(p);
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
	float ratio = clamp_ratio(split.ratio);
	if (t->proportional_split)
		ratio = proportional_insert(target, split.split_h, split.before, ratio);
	inner->ratio = ratio;
	inner->split_h = split.split_h;
	inner->locked = split.lock || t->preserve_split || t->proportional_split;
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
		if (t->proportional_split)
			proportional_remove(parent, leaf);
		/* The sibling, with all its own ratios, takes the parent's place. */
		struct tp_dwindle_node *sib =
			parent->child[parent->child[0] == leaf ? 1 : 0];
		replace_in_parent(t, parent, sib);
		free(parent);
	}
	free(leaf);
	t->gen++;
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

bool tp_dwindle_resize(struct tp_dwindle *t, const void *item, enum tp_dir dir,
					   int32_t delta) {
	if (!delta)
		return false;
	struct tp_dwindle_node *leaf = find(t->root, item);
	if (!leaf || !leaf->parent)
		return false;

	bool want_h = dir == TP_DIR_LEFT || dir == TP_DIR_RIGHT;
	bool positive = dir == TP_DIR_RIGHT || dir == TP_DIR_DOWN;
	int32_t sign = positive ? 1 : -1;

	/* Prefer the nearest ancestor on this axis where `leaf` borders the split
	 * in `dir`; fall back to the nearest ancestor on this axis if `leaf` sits
	 * on the screen edge in `dir`. */
	struct tp_dwindle_node *target = NULL;
	struct tp_dwindle_node *fallback = NULL;
	for (struct tp_dwindle_node *cur = leaf, *p = leaf->parent; p;
		 cur = p, p = p->parent) {
		if (p->split_h != want_h)
			continue;
		if (!fallback)
			fallback = p;
		bool on_near_side = positive ? (p->child[0] == cur)
									 : (p->child[1] == cur);
		if (on_near_side) {
			target = p;
			break;
		}
	}
	if (!target)
		target = fallback;
	if (!target)
		return false;

	int32_t span = want_h ? target->box.w : target->box.h;
	if (span <= 0)
		return false;

	float old = target->ratio;
	target->ratio = clamp_ratio(old + (float)(sign * delta) / (float)span);
	return target->ratio != old;
}

bool tp_dwindle_drag_begin(const struct tp_dwindle *t, const void *item,
						   double px, double py, struct tp_dwindle_drag *drag) {
	(void)px;
	(void)py;
	*drag = (struct tp_dwindle_drag){.tree = t, .gen = t->gen};
	struct tp_dwindle_node *leaf = find(t->root, item);
	if (!leaf)
		return false;

	for (struct tp_dwindle_node *p = leaf->parent; p; p = p->parent) {
		if (p->split_h && !drag->h_node) {
			drag->h_node = p;
			drag->h_ratio = p->ratio;
		} else if (!p->split_h && !drag->v_node) {
			drag->v_node = p;
			drag->v_ratio = p->ratio;
		}
		if (drag->h_node && drag->v_node)
			break;
	}
	return drag->h_node != NULL || drag->v_node != NULL;
}

bool tp_dwindle_drag_valid(const struct tp_dwindle_drag *drag,
						   const struct tp_dwindle *t) {
	return drag->tree && drag->tree == t && t->gen == drag->gen;
}

bool tp_dwindle_drag_update(struct tp_dwindle_drag *drag, double dx,
							double dy) {
	bool changed = false;
	if (!drag->tree || drag->tree->gen != drag->gen)
		return false; /* nodes may have been freed: never touch them */
	if (drag->h_node && drag->h_node->box.w > 0) {
		float next = clamp_ratio(drag->h_ratio +
								 (float)(dx / (double)drag->h_node->box.w));
		if (next != drag->h_node->ratio) {
			drag->h_node->ratio = next;
			changed = true;
		}
	}
	if (drag->v_node && drag->v_node->box.h > 0) {
		float next = clamp_ratio(drag->v_ratio +
								 (float)(dy / (double)drag->v_node->box.h));
		if (next != drag->v_node->ratio) {
			drag->v_node->ratio = next;
			changed = true;
		}
	}
	return changed;
}

struct neighbor_search {
	const struct tp_dwindle_node *src;
	enum tp_dir dir;
	void *best;
	int64_t best_primary;
	int64_t best_secondary;
};

static int64_t i64_abs(int64_t v) {
	return v < 0 ? -v : v;
}

static void search_neighbor(const struct tp_dwindle_node *n,
							struct neighbor_search *st) {
	if (!n)
		return;
	if (!is_leaf(n)) {
		search_neighbor(n->child[0], st);
		search_neighbor(n->child[1], st);
		return;
	}
	if (n == st->src)
		return;

	struct tp_rect s = st->src->box;
	struct tp_rect b = n->box;
	int64_t primary = 0, secondary = 0;

	switch (st->dir) {
	case TP_DIR_LEFT:
		if (b.x + b.w > s.x || b.y >= s.y + s.h || b.y + b.h <= s.y)
			return;
		primary = (int64_t)s.x - ((int64_t)b.x + b.w);
		secondary = i64_abs((2 * (int64_t)b.y + b.h) - (2 * (int64_t)s.y + s.h));
		break;
	case TP_DIR_RIGHT:
		if (b.x < s.x + s.w || b.y >= s.y + s.h || b.y + b.h <= s.y)
			return;
		primary = (int64_t)b.x - ((int64_t)s.x + s.w);
		secondary = i64_abs((2 * (int64_t)b.y + b.h) - (2 * (int64_t)s.y + s.h));
		break;
	case TP_DIR_UP:
		if (b.y + b.h > s.y || b.x >= s.x + s.w || b.x + b.w <= s.x)
			return;
		primary = (int64_t)s.y - ((int64_t)b.y + b.h);
		secondary = i64_abs((2 * (int64_t)b.x + b.w) - (2 * (int64_t)s.x + s.w));
		break;
	case TP_DIR_DOWN:
		if (b.y < s.y + s.h || b.x >= s.x + s.w || b.x + b.w <= s.x)
			return;
		primary = (int64_t)b.y - ((int64_t)s.y + s.h);
		secondary = i64_abs((2 * (int64_t)b.x + b.w) - (2 * (int64_t)s.x + s.w));
		break;
	}

	if (!st->best || primary < st->best_primary ||
		(primary == st->best_primary && secondary < st->best_secondary)) {
		st->best = n->item;
		st->best_primary = primary;
		st->best_secondary = secondary;
	}
}

void *tp_dwindle_neighbor(const struct tp_dwindle *t, const void *item,
						  enum tp_dir dir) {
	struct tp_dwindle_node *src = find(t->root, item);
	if (!src || src->box.w <= 0 || src->box.h <= 0)
		return NULL;
	struct neighbor_search st = {.src = src, .dir = dir};
	search_neighbor(t->root, &st);
	return st.best;
}

bool tp_dwindle_swap_dir(struct tp_dwindle *t, void *item, enum tp_dir dir) {
	void *other = tp_dwindle_neighbor(t, item, dir);
	return other ? tp_dwindle_swap(t, item, other) : false;
}

bool tp_dwindle_move_dir(struct tp_dwindle *t, void *item, enum tp_dir dir,
						 float ratio) {
	struct tp_dwindle_node *src = find(t->root, item);
	void *target = tp_dwindle_neighbor(t, item, dir);
	if (!src || !target)
		return false;
	struct tp_dwindle_node *dst = find(t->root, target);
	if (!dst)
		return false;

	if (src->parent && src->parent == dst->parent) {
		struct tp_dwindle_node *p = src->parent;
		struct tp_dwindle_node *tmp = p->child[0];
		p->child[0] = p->child[1];
		p->child[1] = tmp;
		return true;
	}

	struct tp_rect sbox = src->box;
	struct tp_rect dbox = dst->box;
	struct tp_rect area = dbox;
	if (dst->parent)
		dst->parent->locked = true;
	if (src->parent) {
		struct tp_dwindle_node *sib =
			src->parent->child[src->parent->child[0] == src ? 1 : 0];
		if (find(sib, target) && src->parent->box.w > 0 &&
			src->parent->box.h > 0)
			area = src->parent->box;
	}

	bool split_h = (float)area.w > (float)area.h * t->width_bias;
	double scx = sbox.x + sbox.w / 2.0, scy = sbox.y + sbox.h / 2.0;
	double dcx = dbox.x + dbox.w / 2.0, dcy = dbox.y + dbox.h / 2.0;
	struct tp_dwindle_split split = {
		.split_h = split_h,
		.before = split_h ? scx < dcx : scy < dcy,
		.lock = true,
		.ratio = ratio,
	};

	if (!tp_dwindle_remove(t, item))
		return false;
	return tp_dwindle_insert(t, item, target, split);
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
	int32_t first = (int32_t)((float)(span - g) * n->ratio + 0.5f);
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
