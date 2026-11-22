/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/layout/arrange.h"
#include "tam-pane/client.h"
#include "tam-pane/input.h"
#include "tam-pane/layout/dwindle.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>

static bool is_tiled(const struct tp_client *c) {
	return tp_client_visible(c) && !c->floating && !c->fullscreen;
}

static void place_tile(void *item, struct tp_rect box, void *data) {
	tp_client_place(item, box);
}

/* The most recently focused window on m that is already in the tree. A
 * window being mapped is at the top of the focus stack before it is in the
 * tree, so tp_focus_top would return the newcomer itself. */
static struct tp_client *focused_in_tree(struct tp_server *s,
										 struct tp_monitor *m,
										 struct tp_dwindle *tree) {
	struct tp_client *c;
	wl_list_for_each(c, &s->focus_stack, flink) {
		if (c->mon == m && tp_client_visible(c) && tp_dwindle_find(tree, c))
			return c;
	}
	return NULL;
}

/* Make the tree hold exactly the monitor's visible tiled windows: drop the
 * ones that left, then add newcomers next to the focused window. */
static void sync_tree(struct tp_monitor *m, struct tp_dwindle *tree) {
	struct tp_server *s = m->server;
	size_t n = tp_dwindle_count(tree);
	if (n) {
		void **items = malloc(n * sizeof(*items));
		if (!items)
			return;
		tp_dwindle_items(tree, items, n);
		for (size_t i = 0; i < n; i++) {
			struct tp_client *c = items[i];
			if (c->mon != m || !is_tiled(c))
				tp_dwindle_remove(tree, c);
		}
		free(items);
	}

	struct tp_client *near = focused_in_tree(s, m, tree);

	struct tp_client *c;
	wl_list_for_each_reverse(c, &s->clients, link) {
		if (c->mon != m || !is_tiled(c) || tp_dwindle_find(tree, c))
			continue;
		struct tp_dwindle_node *at = near ? tp_dwindle_find(tree, near) : NULL;
		struct tp_rect box = at ? at->box : m->usable;
		double px = s->cursor->x, py = s->cursor->y;
		bool over = tp_rect_contains(box, px, py);
		struct tp_dwindle_split split =
			s->settings.smart_split && over
				? tp_dwindle_smart_split(box, px, py, s->settings.split_ratio)
				: tp_dwindle_auto_split(tree, box, over, px, py,
										s->settings.split_ratio);
		tp_dwindle_insert(tree, c, near, split);
		near = c;
	}
}

void tp_arrange(struct tp_monitor *m) {
	struct tp_server *s = m->server;
	struct tp_dwindle *tree = tp_monitor_tree(m);

	struct tp_client *c;
	wl_list_for_each(c, &s->clients, link) {
		if (c->mon != m)
			continue;
		bool show = tp_client_visible(c);
		wlr_scene_node_set_enabled(&c->tree->node, show);
		if (!show)
			continue;
		if (c->fullscreen)
			tp_client_place(c, m->area);
		else if (c->floating)
			tp_client_place(c, c->float_geom);
	}

	sync_tree(m, tree);
	tp_dwindle_layout(tree, tp_rect_inset(m->usable, s->settings.gap_outer),
					  s->settings.gap_inner, place_tile, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	tp_pointer_refocus(s, (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000));
}

void tp_arrange_all(struct tp_server *s) {
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) tp_arrange(m);
}
