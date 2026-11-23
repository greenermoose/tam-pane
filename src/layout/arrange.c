/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/layout/arrange.h"
#include "tam-pane/client.h"
#include "tam-pane/idle.h"
#include "tam-pane/input.h"
#include "tam-pane/layout/dwindle.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include <stdlib.h>
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
		void *stack_items[64];
		void **items = n <= 64 ? stack_items : malloc(n * sizeof(*items));
		if (!items)
			return;
		tp_dwindle_items(tree, items, n);
		for (size_t i = 0; i < n; i++) {
			struct tp_client *c = items[i];
			if (c->mon != m || !is_tiled(c))
				tp_dwindle_remove(tree, c);
		}
		if (items != stack_items)
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

static void arrange_monitor(struct tp_monitor *m) {
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
}

static void on_arrange_idle(void *data) {
	struct tp_server *s = data;
	s->arrange_idle = NULL;
	tp_arrange_flush(s);
}

static void schedule_arrange(struct tp_server *s) {
	if (!s || s->stopping || !s->loop || s->arrange_idle)
		return;
	s->arrange_idle = wl_event_loop_add_idle(s->loop, on_arrange_idle, s);
}

void tp_arrange(struct tp_monitor *m) {
	if (!m || !m->server || m->server->stopping)
		return;
	m->arrange_dirty = true;
	schedule_arrange(m->server);
}

void tp_arrange_all(struct tp_server *s) {
	if (!s || s->stopping)
		return;
	bool any = false;
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		m->arrange_dirty = true;
		any = true;
	}
	if (any)
		schedule_arrange(s);
}

void tp_arrange_flush(struct tp_server *s) {
	if (!s)
		return;
	if (s->arrange_idle) {
		wl_event_source_remove(s->arrange_idle);
		s->arrange_idle = NULL;
	}
	if (s->stopping) {
		struct tp_monitor *m;
		wl_list_for_each(m, &s->monitors, link) m->arrange_dirty = false;
		return;
	}
	bool arranged = false;
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		if (!m->arrange_dirty)
			continue;
		m->arrange_dirty = false;
		arrange_monitor(m);
		m->arrange_count++;
		arranged = true;
	}
	if (arranged) {
		tp_idle_update_inhibit(s);
		tp_pointer_refocus_now(s);
	}
}

void tp_arrange_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->arrange_idle) {
		wl_event_source_remove(s->arrange_idle);
		s->arrange_idle = NULL;
	}
}
