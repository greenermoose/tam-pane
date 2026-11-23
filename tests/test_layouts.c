/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure unit tests for master-stack, scroller, monocle, and the unified
 * tp_layout interface (no Wayland, no wlroots).
 */
#include "check.h"
#include "tam-pane/layout/layout.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

struct rec {
	void *item;
	struct tp_rect box;
	bool visible;
};

struct collector {
	struct rec recs[32];
	size_t n;
};

static void collect(void *item, struct tp_rect box, bool visible, void *data) {
	struct collector *c = data;
	if (c->n < 32) {
		c->recs[c->n++] = (struct rec){
			.item = item,
			.box = box,
			.visible = visible,
		};
	}
}

static const struct rec *find_rec(const struct collector *c, void *item) {
	for (size_t i = 0; i < c->n; i++) {
		if (c->recs[i].item == item)
			return &c->recs[i];
	}
	return NULL;
}

static void *id(uintptr_t n) {
	return (void *)n;
}

static void test_master(void) {
	struct tp_master m;
	tp_master_init(&m);
	CHECK(tp_master_count(&m) == 0);
	CHECK(fabsf(m.mfact - TP_MASTER_MFACT_DEFAULT) < 1e-5f);
	CHECK(m.nmaster == 1);

	/* Single window fills area */
	CHECK(tp_master_insert(&m, id(1)));
	CHECK(!tp_master_insert(&m, id(1))); /* duplicate rejected */
	struct tp_rect area = {10, 20, 1000, 600};
	struct collector col = {0};
	tp_master_layout(&m, area, 10, collect, &col);
	CHECK(col.n == 1);
	CHECK(col.recs[0].visible);
	CHECK(col.recs[0].box.x == 10 && col.recs[0].box.y == 20 &&
		  col.recs[0].box.w == 1000 && col.recs[0].box.h == 600);

	/* Three windows: 1 master on left (55%), 2 stacked on right */
	CHECK(tp_master_insert(&m, id(2)));
	CHECK(tp_master_insert(&m, id(3)));
	col.n = 0;
	tp_master_layout(&m, area, 10, collect, &col);
	CHECK(col.n == 3);
	const struct rec *r1 = find_rec(&col, id(1));
	const struct rec *r2 = find_rec(&col, id(2));
	const struct rec *r3 = find_rec(&col, id(3));
	CHECK(r1 && r2 && r3);
	CHECK(r1->box.x == 10 && r1->box.y == 20 && r1->box.h == 600);
	CHECK(r1->box.w == 545); /* round((1000 - 10) * 0.55) = 545 */
	CHECK(r2->box.x == 10 + 545 + 10 && r2->box.w == 445);
	CHECK(r2->box.y == 20 && r2->box.h == 295);
	CHECK(r3->box.x == r2->box.x && r3->box.w == 445);
	CHECK(r3->box.y == 20 + 295 + 10 && r3->box.h == 295);

	/* Resize master wider and narrower */
	float old_mfact = m.mfact;
	CHECK(tp_master_resize(&m, id(1), TP_DIR_RIGHT, 100));
	CHECK(m.mfact > old_mfact);
	CHECK(tp_master_resize(&m, id(1), TP_DIR_LEFT, 100));
	CHECK(fabsf(m.mfact - old_mfact) < 1e-4f);
	CHECK(!tp_master_resize(&m, id(1), TP_DIR_UP, 50));

	/* Pointer drag */
	struct tp_master_drag drag;
	CHECK(tp_master_drag_begin(&m, id(1), &drag));
	CHECK(tp_master_drag_valid(&drag, &m));
	CHECK(tp_master_drag_update(&drag, 200.0));
	CHECK(m.mfact > old_mfact + 0.15f);

	/* Swap stack window left promotes it to master */
	CHECK(tp_master_swap_dir(&m, id(3), TP_DIR_LEFT));
	void *items[4];
	CHECK(tp_master_items(&m, items, 4) == 3);
	CHECK(items[0] == id(3) && items[1] == id(2) && items[2] == id(1));

	/* Adjust nmaster to 2: first two stack in master pane */
	CHECK(tp_master_adjust_nmaster(&m, 1));
	CHECK(m.nmaster == 2);
	col.n = 0;
	tp_master_layout(&m, area, 10, collect, &col);
	r1 = find_rec(&col, id(3));
	r2 = find_rec(&col, id(2));
	r3 = find_rec(&col, id(1));
	CHECK(r1 && r2 && r3);
	CHECK(r1->box.x == 10 && r2->box.x == 10);
	CHECK(r2->box.y > r1->box.y);
	CHECK(r3->box.x > r1->box.x && r3->box.h == 600);

	/* Removing invalidates drag */
	CHECK(tp_master_remove(&m, id(2)));
	CHECK(!tp_master_drag_valid(&drag, &m));
	CHECK(tp_master_count(&m) == 2);

	tp_master_clear(&m);
	CHECK(tp_master_count(&m) == 0);
}

static void test_scroller(void) {
	struct tp_scroller sc;
	tp_scroller_init(&sc);
	sc.default_width = 0.5f;

	/* Insert after focused: 1, then 2 after 1, then 3 after 1 -> order 1, 3, 2 */
	CHECK(tp_scroller_insert(&sc, id(1), NULL));
	CHECK(tp_scroller_insert(&sc, id(2), id(1)));
	CHECK(tp_scroller_insert(&sc, id(3), id(1)));
	void *items[4];
	CHECK(tp_scroller_items(&sc, items, 4) == 3);
	CHECK(items[0] == id(1) && items[1] == id(3) && items[2] == id(2));

	/* Viewport 1000px wide, each col 500px, gap 10px -> total 1520px.
	 * Focusing id(1) shows id(1) [0..500) and id(3) [510..1010); id(2) [1020..1520) is off-screen. */
	struct tp_rect area = {0, 0, 1000, 600};
	struct collector col = {0};
	tp_scroller_layout(&sc, area, 10, id(1), collect, &col);
	CHECK(col.n == 3);
	const struct rec *r1 = find_rec(&col, id(1));
	const struct rec *r3 = find_rec(&col, id(3));
	const struct rec *r2 = find_rec(&col, id(2));
	CHECK(r1 && r1->visible && r1->box.x == 0 && r1->box.w == 500);
	CHECK(r3 && r3->visible && r3->box.x == 510 && r3->box.w == 500);
	CHECK(r2 && !r2->visible && r2->box.x == 1020);

	/* Focusing id(2) scrolls right so id(2) ends at x=1000 (scroll_x = 520),
	 * hiding id(1) completely off the left edge. */
	col.n = 0;
	tp_scroller_layout(&sc, area, 10, id(2), collect, &col);
	r1 = find_rec(&col, id(1));
	r3 = find_rec(&col, id(3));
	r2 = find_rec(&col, id(2));
	CHECK(sc.scroll_x == 520);
	CHECK(r1 && !r1->visible);
	CHECK(r3 && r3->visible);
	CHECK(r2 && r2->visible && r2->box.x == 500 && r2->box.w == 500);

	/* Focusing id(1) again scrolls back to 0 */
	col.n = 0;
	tp_scroller_layout(&sc, area, 10, id(1), collect, &col);
	CHECK(sc.scroll_x == 0);
	CHECK(find_rec(&col, id(1))->visible);

	/* Resize column wider and drag */
	CHECK(tp_scroller_resize(&sc, id(1), TP_DIR_RIGHT, 200));
	CHECK(fabsf(sc.cols[0].width_frac - 0.7f) < 1e-4f);
	struct tp_scroller_drag drag;
	CHECK(tp_scroller_drag_begin(&sc, id(1), &drag));
	CHECK(tp_scroller_drag_valid(&drag, &sc));
	CHECK(tp_scroller_drag_update(&drag, 100.0));
	CHECK(fabsf(sc.cols[0].width_frac - 0.8f) < 1e-4f);

	/* Swap right moves id(1) past id(3) and invalidates drag */
	CHECK(tp_scroller_swap_dir(&sc, id(1), TP_DIR_RIGHT));
	CHECK(!tp_scroller_drag_valid(&drag, &sc));
	CHECK(tp_scroller_items(&sc, items, 4) == 3);
	CHECK(items[0] == id(3) && items[1] == id(1) && items[2] == id(2));

	tp_scroller_clear(&sc);
	CHECK(tp_scroller_count(&sc) == 0);
}

static void test_monocle(void) {
	struct tp_monocle mo;
	tp_monocle_init(&mo);
	CHECK(tp_monocle_insert(&mo, id(1)));
	CHECK(tp_monocle_insert(&mo, id(2)));
	CHECK(tp_monocle_insert(&mo, id(3)));

	struct tp_rect area = {5, 10, 800, 600};
	struct collector col = {0};
	tp_monocle_layout(&mo, area, id(2), collect, &col);
	CHECK(col.n == 3);
	CHECK(!find_rec(&col, id(1))->visible);
	CHECK(find_rec(&col, id(2))->visible);
	CHECK(!find_rec(&col, id(3))->visible);
	CHECK(find_rec(&col, id(2))->box.x == 5 &&
		  find_rec(&col, id(2))->box.w == 800 &&
		  find_rec(&col, id(2))->box.h == 600);

	/* Removing active falls back to neighbor */
	CHECK(tp_monocle_remove(&mo, id(2)));
	col.n = 0;
	tp_monocle_layout(&mo, area, NULL, collect, &col);
	CHECK(col.n == 2);
	CHECK(find_rec(&col, id(3))->visible);
	CHECK(!find_rec(&col, id(1))->visible);

	/* Reorder */
	CHECK(tp_monocle_swap_dir(&mo, id(3), TP_DIR_LEFT));
	void *items[4];
	CHECK(tp_monocle_items(&mo, items, 4) == 2);
	CHECK(items[0] == id(3) && items[1] == id(1));

	tp_monocle_clear(&mo);
	CHECK(tp_monocle_count(&mo) == 0);
}

static void test_unified_layout(void) {
	enum tp_layout_kind k;
	CHECK(tp_layout_kind_from_name("dwindle", &k) && k == TP_LAYOUT_DWINDLE);
	CHECK(tp_layout_kind_from_name("master", &k) && k == TP_LAYOUT_MASTER);
	CHECK(tp_layout_kind_from_name("scroller", &k) && k == TP_LAYOUT_SCROLLER);
	CHECK(tp_layout_kind_from_name("monocle", &k) && k == TP_LAYOUT_MONOCLE);
	CHECK(!tp_layout_kind_from_name("grid", &k));

	struct tp_settings cfg = {
		.width_bias = 1.0f,
		.layout = TP_LAYOUT_DWINDLE,
		.master_ratio = 0.55f,
		.master_count = 1,
		.scroller_width = 0.5f,
	};
	struct tp_layout l;
	tp_layout_init(&l, &cfg);
	CHECK(l.kind == TP_LAYOUT_DWINDLE);

	/* Cycle through all 4 layouts and back */
	CHECK(tp_layout_cycle(&l, +1) == TP_LAYOUT_MASTER);
	CHECK(tp_layout_cycle(&l, +1) == TP_LAYOUT_SCROLLER);
	CHECK(tp_layout_cycle(&l, +1) == TP_LAYOUT_MONOCLE);
	CHECK(tp_layout_cycle(&l, +1) == TP_LAYOUT_DWINDLE);
	CHECK(tp_layout_cycle(&l, -1) == TP_LAYOUT_MONOCLE);

	/* Insert into monocle and master, then forget drops from all engines */
	struct tp_layout_insert_ctx ctx = {
		.fallback_box = {0, 0, 800, 600},
		.split_ratio = 0.5f,
	};
	CHECK(tp_layout_insert(&l, id(1), NULL, &ctx));
	CHECK(tp_layout_set_kind(&l, TP_LAYOUT_MASTER));
	CHECK(tp_layout_insert(&l, id(1), NULL, &ctx));
	CHECK(tp_layout_set_kind(&l, TP_LAYOUT_DWINDLE));
	CHECK(tp_layout_insert(&l, id(1), NULL, &ctx));

	tp_layout_forget(&l, id(1));
	CHECK(tp_dwindle_count(&l.dwindle) == 0);
	CHECK(tp_master_count(&l.master) == 0);
	CHECK(tp_scroller_count(&l.scroller) == 0);
	CHECK(tp_monocle_count(&l.monocle) == 0);

	tp_layout_clear(&l);
}

int main(void) {
	test_master();
	test_scroller();
	test_monocle();
	test_unified_layout();
	CHECK_DONE("test_layouts");
}
