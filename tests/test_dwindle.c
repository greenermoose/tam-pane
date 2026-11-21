/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "check.h"
#include "tam-pane/layout/dwindle.h"
#include <string.h>

/* Placement results, indexed by the small integers used as items. */
#define N 8
static struct tp_rect placed[N];
static int placed_count;

static void record(void *item, struct tp_rect box, void *data) {
	(void)data;
	int i = (int)(long)item;
	if (i >= 0 && i < N)
		placed[i] = box;
	placed_count++;
}

#define ITEM(i) ((void *)(long)(i))

static void run_layout(struct tp_dwindle *t, struct tp_rect area, int gap) {
	memset(placed, 0, sizeof(placed));
	placed_count = 0;
	tp_dwindle_layout(t, area, gap, record, NULL);
}

static bool rect_eq(struct tp_rect a, int x, int y, int w, int h) {
	return a.x == x && a.y == y && a.w == w && a.h == h;
}

static struct tp_dwindle_split after(float ratio) {
	return (struct tp_dwindle_split){.ratio = ratio};
}

int main(void) {
	struct tp_dwindle t;
	struct tp_rect screen = {0, 0, 1000, 600};
	tp_dwindle_init(&t);

	/* Empty tree: layout calls nothing; remove fails. */
	run_layout(&t, screen, 10);
	CHECK(placed_count == 0);
	CHECK(!tp_dwindle_remove(&t, ITEM(1)));
	CHECK(!tp_dwindle_insert(&t, NULL, NULL, after(0.5f)));

	/* One window fills the area. */
	CHECK(tp_dwindle_insert(&t, ITEM(1), NULL, after(0.5f)));
	CHECK(!tp_dwindle_insert(&t, ITEM(1), NULL, after(0.5f))); /* dup */
	run_layout(&t, screen, 10);
	CHECK(placed_count == 1);
	CHECK(rect_eq(placed[1], 0, 0, 1000, 600));

	/* A second window on a wide area goes side by side, gap between. */
	CHECK(tp_dwindle_insert(&t, ITEM(2), ITEM(1), after(0.5f)));
	run_layout(&t, screen, 10);
	CHECK(placed_count == 2);
	CHECK(rect_eq(placed[1], 0, 0, 495, 600));
	CHECK(rect_eq(placed[2], 505, 0, 495, 600));

	/* A third next to 2 splits 2's tall area into a stack: the spiral. */
	CHECK(tp_dwindle_insert(&t, ITEM(3), ITEM(2), after(0.5f)));
	run_layout(&t, screen, 10);
	CHECK(rect_eq(placed[1], 0, 0, 495, 600));
	CHECK(rect_eq(placed[2], 505, 0, 495, 295));
	CHECK(rect_eq(placed[3], 505, 305, 495, 295));
	CHECK(tp_dwindle_count(&t) == 3);

	void *items[N];
	CHECK(tp_dwindle_items(&t, items, N) == 3);
	CHECK(items[0] == ITEM(1) && items[1] == ITEM(2) && items[2] == ITEM(3));
	CHECK(tp_dwindle_items(&t, items, 2) == 2);

	/* Removing 2 gives its area to 3 and keeps the root's ratio. */
	struct tp_dwindle_node *root = t.root;
	root->ratio = 0.7f;
	CHECK(tp_dwindle_remove(&t, ITEM(2)));
	CHECK(t.root == root && root->ratio == 0.7f);
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[1], 0, 0, 700, 600));
	CHECK(rect_eq(placed[3], 700, 0, 300, 600));
	CHECK(tp_dwindle_find(&t, ITEM(2)) == NULL);

	/* Swap exchanges places only. */
	CHECK(tp_dwindle_swap(&t, ITEM(1), ITEM(3)));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[3], 0, 0, 700, 600));
	CHECK(rect_eq(placed[1], 700, 0, 300, 600));
	CHECK(!tp_dwindle_swap(&t, ITEM(1), ITEM(6)));

	/* Insert before: the new window takes the first half. */
	struct tp_dwindle_split before = {.ratio = 0.5f, .before = true};
	CHECK(tp_dwindle_insert(&t, ITEM(4), ITEM(1), before));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[4], 700, 0, 300, 300));
	CHECK(rect_eq(placed[1], 700, 300, 300, 300));

	/* An unknown `near` falls back to the last leaf (here item 1). */
	CHECK(tp_dwindle_insert(&t, ITEM(5), ITEM(7), after(0.5f)));
	CHECK(tp_dwindle_find(&t, ITEM(5))->parent ==
		  tp_dwindle_find(&t, ITEM(1))->parent);

	/* Removing down to empty leaves a valid empty tree. */
	for (int i = 1; i <= 5; i++)
		tp_dwindle_remove(&t, ITEM(i));
	CHECK(t.root == NULL && tp_dwindle_count(&t) == 0);

	/* Unlocked splits follow the area's shape; locked ones keep theirs. */
	CHECK(tp_dwindle_insert(&t, ITEM(1), NULL, after(0.5f)));
	CHECK(tp_dwindle_insert(&t, ITEM(2), ITEM(1), after(0.5f)));
	run_layout(&t, (struct tp_rect){0, 0, 400, 800}, 0);
	CHECK(rect_eq(placed[1], 0, 0, 400, 400)); /* tall area: stacked */
	t.root->locked = true;
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[1], 0, 0, 1000, 300)); /* stays stacked */

	/* preserve_split locks new splits as they are made. */
	tp_dwindle_clear(&t);
	t.preserve_split = true;
	CHECK(tp_dwindle_insert(&t, ITEM(1), NULL, after(0.5f)));
	CHECK(tp_dwindle_insert(&t, ITEM(2), ITEM(1), after(0.5f)));
	CHECK(t.root->locked);
	t.preserve_split = false;

	/* Ratios are clamped; tiny areas never yield zero sizes. */
	tp_dwindle_clear(&t);
	CHECK(tp_dwindle_insert(&t, ITEM(1), NULL, after(0.5f)));
	CHECK(tp_dwindle_insert(&t, ITEM(2), ITEM(1), after(5.0f)));
	CHECK(t.root->ratio == TP_DWINDLE_RATIO_MAX);
	run_layout(&t, (struct tp_rect){0, 0, 3, 1}, 10);
	CHECK(placed[1].w >= 1 && placed[2].w >= 1);

	/* Split choice: Hyprland's default and smart rules. */
	struct tp_rect wide = {0, 0, 800, 400}, tall = {0, 0, 400, 800};
	struct tp_dwindle_split s;
	s = tp_dwindle_auto_split(&t, wide, false, 0, 0, 0.5f);
	CHECK(s.split_h && !s.before && !s.lock);
	s = tp_dwindle_auto_split(&t, tall, false, 0, 0, 0.5f);
	CHECK(!s.split_h);
	s = tp_dwindle_auto_split(&t, wide, true, 100, 200, 0.5f);
	CHECK(s.split_h && s.before);
	s = tp_dwindle_auto_split(&t, tall, true, 100, 700, 0.5f);
	CHECK(!s.split_h && !s.before);
	s = tp_dwindle_smart_split(wide, 790, 200, 0.5f); /* right edge */
	CHECK(s.split_h && !s.before && s.lock);
	s = tp_dwindle_smart_split(wide, 400, 10, 0.5f); /* top edge */
	CHECK(!s.split_h && s.before);
	s = tp_dwindle_smart_split(wide, 400, 200, 0.5f); /* dead centre */
	CHECK(!s.split_h && !s.before);

	/* Keyboard resize: grow/shrink by direction, edge fallback, and clamp. */
	tp_dwindle_clear(&t);
	CHECK(tp_dwindle_insert(&t, ITEM(1), NULL, after(0.5f)));
	run_layout(&t, screen, 0);
	CHECK(!tp_dwindle_resize(&t, ITEM(1), TP_DIR_RIGHT, 100)); /* single */
	CHECK(tp_dwindle_insert(&t, ITEM(2), ITEM(1), after(0.5f)));
	run_layout(&t, screen, 0);
	CHECK(!tp_dwindle_resize(&t, ITEM(1), TP_DIR_UP, 50)); /* no v-split */
	CHECK(tp_dwindle_resize(&t, ITEM(1), TP_DIR_RIGHT, 100));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[1], 0, 0, 600, 600));
	CHECK(rect_eq(placed[2], 600, 0, 400, 600));
	/* Screen-edge fallback: 1 pressing LEFT shrinks 1 by moving divider left. */
	CHECK(tp_dwindle_resize(&t, ITEM(1), TP_DIR_LEFT, 100));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[1], 0, 0, 500, 600));
	CHECK(tp_dwindle_resize(&t, ITEM(2), TP_DIR_LEFT, 100));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[1], 0, 0, 400, 600));
	CHECK(rect_eq(placed[2], 400, 0, 600, 600));
	CHECK(tp_dwindle_resize(&t, ITEM(2), TP_DIR_RIGHT, 100));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[1], 0, 0, 500, 600));
	/* Clamp at RATIO_MAX and RATIO_MIN. */
	CHECK(tp_dwindle_resize(&t, ITEM(1), TP_DIR_RIGHT, 5000));
	CHECK(t.root->ratio == TP_DWINDLE_RATIO_MAX);
	CHECK(!tp_dwindle_resize(&t, ITEM(1), TP_DIR_RIGHT, 100));
	CHECK(tp_dwindle_resize(&t, ITEM(1), TP_DIR_LEFT, 5000));
	CHECK(t.root->ratio == TP_DWINDLE_RATIO_MIN);

	/* Nested 3-window tree [1 | [2 / 3]]: vertical resize, drag, neighbor,
	 * directional swap, and directional move. */
	tp_dwindle_clear(&t);
	CHECK(tp_dwindle_insert(&t, ITEM(1), NULL, after(0.5f)));
	CHECK(tp_dwindle_insert(&t, ITEM(2), ITEM(1), after(0.5f)));
	CHECK(tp_dwindle_insert(&t, ITEM(3), ITEM(2), after(0.5f)));
	run_layout(&t, screen, 0);
	CHECK(tp_dwindle_resize(&t, ITEM(2), TP_DIR_DOWN, 60));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[2], 500, 0, 500, 360));
	CHECK(rect_eq(placed[3], 500, 360, 500, 240));
	CHECK(tp_dwindle_resize(&t, ITEM(3), TP_DIR_UP, 60));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[2], 500, 0, 500, 300));
	CHECK(rect_eq(placed[3], 500, 300, 500, 300));

	/* Pointer drag updates both horizontal and vertical splits. */
	struct tp_dwindle_drag drag;
	CHECK(tp_dwindle_drag_begin(&t, ITEM(2), 700, 150, &drag));
	CHECK(drag.h_node == t.root && drag.v_node == t.root->child[1]);
	CHECK(tp_dwindle_drag_update(&drag, 100.0, -60.0));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[1], 0, 0, 600, 600));
	CHECK(rect_eq(placed[2], 600, 0, 400, 240));
	CHECK(rect_eq(placed[3], 600, 240, 400, 360));

	/* 3-window horizontal row [1 | [2 | 3]]: 2 resizes inner on RIGHT and
	 * outer on LEFT. */
	tp_dwindle_clear(&t);
	struct tp_dwindle_split hlock = {.split_h = true, .lock = true, .ratio = 0.5f};
	CHECK(tp_dwindle_insert(&t, ITEM(1), NULL, hlock));
	CHECK(tp_dwindle_insert(&t, ITEM(2), ITEM(1), hlock));
	CHECK(tp_dwindle_insert(&t, ITEM(3), ITEM(2), hlock));
	run_layout(&t, (struct tp_rect){0, 0, 1000, 600}, 0);
	CHECK(rect_eq(placed[1], 0, 0, 500, 600));
	CHECK(rect_eq(placed[2], 500, 0, 250, 600));
	CHECK(rect_eq(placed[3], 750, 0, 250, 600));
	CHECK(tp_dwindle_resize(&t, ITEM(2), TP_DIR_RIGHT, 50));
	run_layout(&t, (struct tp_rect){0, 0, 1000, 600}, 0);
	CHECK(placed[1].w == 500 && placed[2].w == 300 && placed[3].w == 200);
	CHECK(tp_dwindle_resize(&t, ITEM(2), TP_DIR_LEFT, 100));
	run_layout(&t, (struct tp_rect){0, 0, 1000, 600}, 0);
	CHECK(placed[1].w == 400);

	/* Neighbors, directional swap, and directional move on [1 | [2 / 3]]. */
	tp_dwindle_clear(&t);
	CHECK(tp_dwindle_insert(&t, ITEM(1), NULL, after(0.5f)));
	CHECK(tp_dwindle_insert(&t, ITEM(2), ITEM(1), after(0.5f)));
	CHECK(tp_dwindle_insert(&t, ITEM(3), ITEM(2), after(0.5f)));
	run_layout(&t, screen, 0);
	CHECK(tp_dwindle_neighbor(&t, ITEM(1), TP_DIR_LEFT) == NULL);
	CHECK(tp_dwindle_neighbor(&t, ITEM(1), TP_DIR_RIGHT) == ITEM(2));
	CHECK(tp_dwindle_neighbor(&t, ITEM(2), TP_DIR_LEFT) == ITEM(1));
	CHECK(tp_dwindle_neighbor(&t, ITEM(2), TP_DIR_DOWN) == ITEM(3));
	CHECK(tp_dwindle_neighbor(&t, ITEM(3), TP_DIR_UP) == ITEM(2));
	CHECK(tp_dwindle_neighbor(&t, ITEM(3), TP_DIR_LEFT) == ITEM(1));
	CHECK(tp_dwindle_neighbor(&t, ITEM(3), TP_DIR_DOWN) == NULL);

	CHECK(tp_dwindle_swap_dir(&t, ITEM(2), TP_DIR_DOWN));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[3], 500, 0, 500, 300));
	CHECK(rect_eq(placed[2], 500, 300, 500, 300));
	CHECK(!tp_dwindle_swap_dir(&t, ITEM(1), TP_DIR_LEFT));

	/* Move sibling: 3 DOWN swaps sibling order back to [2 / 3]. */
	CHECK(tp_dwindle_move_dir(&t, ITEM(3), TP_DIR_DOWN, 0.5f));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[2], 500, 0, 500, 300));
	CHECK(rect_eq(placed[3], 500, 300, 500, 300));

	/* Move across subtrees: 1 RIGHT moves into 2's slot -> [[1 | 2] / 3]. */
	CHECK(tp_dwindle_move_dir(&t, ITEM(1), TP_DIR_RIGHT, 0.5f));
	run_layout(&t, screen, 0);
	CHECK(rect_eq(placed[1], 0, 0, 500, 300));
	CHECK(rect_eq(placed[2], 500, 0, 500, 300));
	CHECK(rect_eq(placed[3], 0, 300, 1000, 300));

	/* Optional proportional split: equal 1/N shares on insert and rollback
	 * on remove, plus preserving custom ratios. */
	tp_dwindle_clear(&t);
	t.proportional_split = true;
	struct tp_rect wide1200 = {0, 0, 1200, 600};
	struct tp_dwindle_split hsplit = {.split_h = true, .ratio = 0.5f};
	CHECK(tp_dwindle_insert(&t, ITEM(1), NULL, hsplit));
	CHECK(tp_dwindle_insert(&t, ITEM(2), ITEM(1), hsplit));
	CHECK(tp_dwindle_insert(&t, ITEM(3), ITEM(2), hsplit));
	CHECK(tp_dwindle_insert(&t, ITEM(4), ITEM(3), hsplit));
	run_layout(&t, wide1200, 0);
	CHECK(placed[1].w == 300 && placed[2].w == 300 &&
		  placed[3].w == 300 && placed[4].w == 300);

	CHECK(tp_dwindle_remove(&t, ITEM(4)));
	run_layout(&t, wide1200, 0);
	CHECK(placed[1].w == 400 && placed[2].w == 400 && placed[3].w == 400);

	CHECK(tp_dwindle_remove(&t, ITEM(2)));
	run_layout(&t, wide1200, 0);
	CHECK(placed[1].w == 600 && placed[3].w == 600);

	/* Custom ratio [0.6, 0.4] -> insert 3 -> [0.4, 0.267, 0.333] -> remove 3
	 * restores [0.6, 0.4]. */
	t.root->ratio = 0.6f;
	CHECK(tp_dwindle_insert(&t, ITEM(4), ITEM(3), hsplit));
	run_layout(&t, wide1200, 0);
	CHECK(placed[1].w == 480 && placed[3].w == 320 && placed[4].w == 400);
	CHECK(tp_dwindle_remove(&t, ITEM(4)));
	run_layout(&t, wide1200, 0);
	CHECK(placed[1].w == 720 && placed[3].w == 480);

	/* Perpendicular split inside one column does not disturb horizontal run. */
	struct tp_dwindle_split vsplit = {.split_h = false, .ratio = 0.5f};
	CHECK(tp_dwindle_insert(&t, ITEM(5), ITEM(3), vsplit));
	run_layout(&t, wide1200, 0);
	CHECK(placed[1].w == 720 && placed[3].w == 480 && placed[5].w == 480);
	CHECK(placed[3].h == 300 && placed[5].h == 300);

	tp_dwindle_clear(&t);
	CHECK_DONE("test_dwindle");
}
