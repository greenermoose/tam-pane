/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Dwindle: dynamic tiling as a binary tree. Every leaf holds one window;
 * every inner node splits its area between two children, side by side or
 * stacked, with `ratio` giving the first child's share. A new window splits
 * the leaf it opens next to; a closed window's sibling takes over its
 * parent's whole area.
 *
 * The tree is refactored from mango's src/layout/dwindle.c, and its split
 * rules are checked against Hyprland's DwindleAlgorithm.cpp (see
 * UPSTREAM.md). Unlike both, it holds no compositor state and no globals:
 * items are opaque pointers, settings live in the tree, and placement is
 * reported through a callback. That keeps it testable without a display
 * and lets Lua layouts reuse it later.
 */
#ifndef TAM_PANE_LAYOUT_DWINDLE_H
#define TAM_PANE_LAYOUT_DWINDLE_H

#include "tam-pane/geom.h"
#include <stdbool.h>
#include <stddef.h>

struct tp_dwindle_node {
	struct tp_dwindle_node *parent;
	struct tp_dwindle_node *child[2]; /* both NULL on a leaf */
	void *item;						  /* leaf only */
	float ratio;					  /* first child's share, inner only */
	bool split_h;					  /* children side by side */
	bool locked;					  /* orientation never re-chosen */
	struct tp_rect box;				  /* area from the last layout */
};

struct tp_dwindle {
	struct tp_dwindle_node *root;
	/* Settings, set by tp_dwindle_init and changeable at any time. */
	float width_bias;		 /* side by side when w > h * width_bias */
	bool preserve_split;	 /* keep each split's first orientation */
	bool proportional_split; /* keep same-direction runs proportional */
	/* Changes whenever a node is freed, so anything holding node pointers
	 * (a pointer drag) can tell they may be stale. */
	unsigned long gen;
};

/* State captured at the start of a pointer resize drag on a tiled window.
 * The node pointers are valid only while tp_dwindle_drag_valid() holds. */
struct tp_dwindle_drag {
	const struct tp_dwindle *tree;
	unsigned long gen;
	struct tp_dwindle_node *h_node;
	struct tp_dwindle_node *v_node;
	float h_ratio;
	float v_ratio;
};

/* How a new window joins the tree next to an existing one. */
struct tp_dwindle_split {
	bool split_h; /* side by side (true) or stacked (false) */
	bool before;  /* new window takes the first (left/top) half */
	bool lock;	  /* fix this orientation for good */
	float ratio;  /* first child's share */
};

typedef void (*tp_dwindle_place_fn)(void *item, struct tp_rect box,
									void *data);

#define TP_DWINDLE_RATIO_MIN 0.05f
#define TP_DWINDLE_RATIO_MAX 0.95f

void tp_dwindle_init(struct tp_dwindle *t);
void tp_dwindle_clear(struct tp_dwindle *t);

/* Split along the longer side (biased by width_bias). Insert before the
 * target if the pointer is more than 8 logical pixels left of or above its
 * midpoint on that axis. Otherwise insert after the target. */
struct tp_dwindle_split tp_dwindle_auto_split(const struct tp_dwindle *t,
											  struct tp_rect target,
											  bool have_point, double px,
											  double py, float ratio);

/* Hyprland's smart split: the pointer's direction from the target's centre
 * picks the edge the new window opens on. The orientation is locked. */
struct tp_dwindle_split tp_dwindle_smart_split(struct tp_rect target,
											   double px, double py,
											   float ratio);

/* Insert `item` next to `near`. When `near` is NULL or not in the tree, the
 * last leaf is used. Returns false on allocation failure or when `item` is
 * NULL or already present. */
bool tp_dwindle_insert(struct tp_dwindle *t, void *item, void *near,
					   struct tp_dwindle_split split);

/* Remove `item`. Returns false when it was not in the tree. */
bool tp_dwindle_remove(struct tp_dwindle *t, void *item);

/* Exchange the places of two items. Returns false unless both are present. */
bool tp_dwindle_swap(struct tp_dwindle *t, void *a, void *b);

/* Resize the split affecting `item` along `dir` by `delta` pixels.
 * Positive moves the boundary in `dir`, negative moves it opposite. */
bool tp_dwindle_resize(struct tp_dwindle *t, const void *item, enum tp_dir dir,
					   int32_t delta);

/* Capture the nearest horizontal/vertical split ancestors of `item` for a
 * pointer resize drag, then apply cursor deltas from the drag origin. */
bool tp_dwindle_drag_begin(const struct tp_dwindle *t, const void *item,
						   double px, double py, struct tp_dwindle_drag *drag);
bool tp_dwindle_drag_update(struct tp_dwindle_drag *drag, double dx, double dy);

/* True while `drag` was begun on `t` and `t` has freed no node since, so its
 * node pointers are still safe. Otherwise begin the drag again. */
bool tp_dwindle_drag_valid(const struct tp_dwindle_drag *drag,
						   const struct tp_dwindle *t);

/* Find the closest laid-out leaf in `dir` from `item`, or NULL. */
void *tp_dwindle_neighbor(const struct tp_dwindle *t, const void *item,
						  enum tp_dir dir);

/* Swap `item` with its neighbor in `dir`. */
bool tp_dwindle_swap_dir(struct tp_dwindle *t, void *item, enum tp_dir dir);

/* Move `item` into the slot next to its neighbor in `dir`. */
bool tp_dwindle_move_dir(struct tp_dwindle *t, void *item, enum tp_dir dir,
						 float ratio);

struct tp_dwindle_node *tp_dwindle_find(const struct tp_dwindle *t,
										const void *item);
size_t tp_dwindle_count(const struct tp_dwindle *t);

/* Copy up to `max` items into `out`, first leaf first; returns the count. */
size_t tp_dwindle_items(const struct tp_dwindle *t, void **out, size_t max);

/* Lay the tree out over `area`, leaving `gap` pixels between siblings, and
 * call `place` once per leaf. */
void tp_dwindle_layout(struct tp_dwindle *t, struct tp_rect area,
					   int32_t gap, tp_dwindle_place_fn place, void *data);

#endif
