/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/session_lock.h"
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/idle.h"
#include "tam-pane/input.h"
#include "tam-pane/monitor.h"
#include "tam-pane/security.h"
#include "tam-pane/server.h"
#include "tam-pane/spawn.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <tam.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

struct tp_lock_surface {
	struct wl_list link; /* tp_server.lock_surfaces */
	struct tp_server *server;
	struct tp_monitor *mon;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_scene_tree *tree;
	struct wl_listener map;
	struct wl_listener destroy;
};

static void set_non_lock_layers_enabled(struct tp_server *s, bool enabled) {
	for (int i = 0; i < TP_LAYER_LOCK; i++) {
		if (s->layer[i])
			wlr_scene_node_set_enabled(&s->layer[i]->node, enabled);
	}
}

static void rebuild_bg_covers(struct tp_server *s) {
	if (!s || !s->layer[TP_LAYER_LOCK])
		return;
	if (s->lock_bg_tree) {
		wlr_scene_node_destroy(&s->lock_bg_tree->node);
		s->lock_bg_tree = NULL;
	}
	if (!s->locked)
		return;

	s->lock_bg_tree = wlr_scene_tree_create(s->layer[TP_LAYER_LOCK]);
	if (!s->lock_bg_tree)
		return;
	wlr_scene_node_lower_to_bottom(&s->lock_bg_tree->node);

	static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		if (m->area.w <= 0 || m->area.h <= 0)
			continue;
		struct wlr_scene_rect *rect =
			wlr_scene_rect_create(s->lock_bg_tree, m->area.w, m->area.h, black);
		if (rect)
			wlr_scene_node_set_position(&rect->node, m->area.x, m->area.y);
	}
}

static void enter_lock_keyboard(struct tp_server *s,
								struct wlr_surface *surface) {
	if (!s || !s->seat || !surface)
		return;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, surface, kb->keycodes,
									   kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, NULL);
}

static void focus_any_lock_surface(struct tp_server *s) {
	struct tp_lock_surface *ls;
	wl_list_for_each(ls, &s->lock_surfaces, link) {
		if (ls->lock_surface && ls->lock_surface->surface->mapped &&
			ls->mon == s->selmon) {
			enter_lock_keyboard(s, ls->lock_surface->surface);
			return;
		}
	}
	wl_list_for_each(ls, &s->lock_surfaces, link) {
		if (ls->lock_surface && ls->lock_surface->surface->mapped) {
			enter_lock_keyboard(s, ls->lock_surface->surface);
			return;
		}
	}
	if (s->seat)
		wlr_seat_keyboard_notify_clear_focus(s->seat);
}

static void detach_active_lock_listeners(struct tp_server *s) {
	if (s->lock_new_surface.notify) {
		wl_list_remove(&s->lock_new_surface.link);
		s->lock_new_surface.notify = NULL;
	}
	if (s->lock_unlock.notify) {
		wl_list_remove(&s->lock_unlock.link);
		s->lock_unlock.notify = NULL;
	}
	if (s->lock_destroy.notify) {
		wl_list_remove(&s->lock_destroy.link);
		s->lock_destroy.notify = NULL;
	}
}

static void on_lock_surface_map(struct wl_listener *l, void *data) {
	struct tp_lock_surface *ls = wl_container_of(l, ls, map);
	struct tp_server *s = ls->server;
	if (!s->locked || s->stopping)
		return;
	if (!s->seat->keyboard_state.focused_surface || ls->mon == s->selmon)
		enter_lock_keyboard(s, ls->lock_surface->surface);
	tp_idle_update_inhibit(s);
}

static void on_lock_surface_destroy(struct wl_listener *l, void *data) {
	struct tp_lock_surface *ls = wl_container_of(l, ls, destroy);
	struct tp_server *s = ls->server;
	struct wlr_surface *surf = ls->lock_surface->surface;

	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->link);
	if (ls->tree)
		wlr_scene_node_destroy(&ls->tree->node);
	free(ls);

	if (s->stopping)
		return;
	if (s->seat && s->seat->keyboard_state.focused_surface == surf)
		focus_any_lock_surface(s);
	tp_idle_update_inhibit(s);
}

static void on_lock_new_surface(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, lock_new_surface);
	struct wlr_session_lock_surface_v1 *lsurf = data;
	struct tp_monitor *m = lsurf->output ? lsurf->output->data : NULL;
	if (!m)
		return;

	struct tp_lock_surface *ls = calloc(1, sizeof(*ls));
	if (!ls) {
		tam_log_error("session-lock: out of memory");
		return;
	}
	ls->server = s;
	ls->mon = m;
	ls->lock_surface = lsurf;
	ls->tree = wlr_scene_subsurface_tree_create(s->layer[TP_LAYER_LOCK],
												lsurf->surface);
	if (!ls->tree) {
		free(ls);
		return;
	}
	wlr_scene_node_set_position(&ls->tree->node, m->area.x, m->area.y);
	wlr_session_lock_surface_v1_configure(lsurf, (uint32_t)m->area.w,
										  (uint32_t)m->area.h);
	wl_list_insert(&s->lock_surfaces, &ls->link);

	ls->map.notify = on_lock_surface_map;
	wl_signal_add(&lsurf->surface->events.map, &ls->map);
	ls->destroy.notify = on_lock_surface_destroy;
	wl_signal_add(&lsurf->events.destroy, &ls->destroy);
}

static void on_lock_unlock(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, lock_unlock);
	detach_active_lock_listeners(s);
	s->active_lock = NULL;
	s->locked = false;

	if (s->stopping)
		return;
	rebuild_bg_covers(s);
	set_non_lock_layers_enabled(s, true);
	tp_idle_update_inhibit(s);
	tp_focus_refresh(s);
	tp_pointer_refocus_now(s);
	tam_log_info("session unlocked");
}

static void on_lock_destroy(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, lock_destroy);
	detach_active_lock_listeners(s);
	s->active_lock = NULL;
	if (s->stopping)
		return;
	if (s->locked) {
		/* Fail-locked per ext-session-lock-v1: a crashing locker must never
		 * unlock the session. Black covers stay up and bindings stay gated
		 * until a newly launched locker takes over and unlocks. */
		tam_log_warn("session locker exited without unlocking; session stays locked");
		if (s->seat) {
			wlr_seat_keyboard_notify_clear_focus(s->seat);
			wlr_seat_pointer_clear_focus(s->seat);
		}
		rebuild_bg_covers(s);
		tp_idle_update_inhibit(s);
	}
}

static void on_new_lock(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_session_lock);
	struct wlr_session_lock_v1 *lock = data;

	if (s->active_lock) {
		tam_log_warn("session already locked by an active locker; refusing second lock");
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	s->locked = true;
	s->active_lock = lock;
	tp_pointer_cancel_grab(s, NULL);

	if (s->focused_client)
		wlr_xdg_toplevel_set_activated(s->focused_client->toplevel, false);
	s->focused_layer = NULL;
	if (s->seat) {
		wlr_seat_keyboard_notify_clear_focus(s->seat);
		wlr_seat_pointer_clear_focus(s->seat);
	}

	set_non_lock_layers_enabled(s, false);
	rebuild_bg_covers(s);

	s->lock_new_surface.notify = on_lock_new_surface;
	wl_signal_add(&lock->events.new_surface, &s->lock_new_surface);
	s->lock_unlock.notify = on_lock_unlock;
	wl_signal_add(&lock->events.unlock, &s->lock_unlock);
	s->lock_destroy.notify = on_lock_destroy;
	wl_signal_add(&lock->events.destroy, &s->lock_destroy);

	wlr_session_lock_v1_send_locked(lock);
	tp_idle_update_inhibit(s);
	tam_log_info("session locked");
}

bool tp_session_lock_init(struct tp_server *s) {
	wl_list_init(&s->lock_surfaces);
	s->session_lock_mgr = wlr_session_lock_manager_v1_create(s->display);
	if (!s->session_lock_mgr) {
		tam_log_error("cannot create ext_session_lock_manager_v1");
		return false;
	}
	s->new_session_lock.notify = on_new_lock;
	wl_signal_add(&s->session_lock_mgr->events.new_lock, &s->new_session_lock);
	return true;
}

void tp_session_lock_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->new_session_lock.notify) {
		wl_list_remove(&s->new_session_lock.link);
		s->new_session_lock.notify = NULL;
	}
	detach_active_lock_listeners(s);
	s->active_lock = NULL;
	s->locked = false;
	if (s->lock_bg_tree) {
		wlr_scene_node_destroy(&s->lock_bg_tree->node);
		s->lock_bg_tree = NULL;
	}
}

void tp_session_lock_arrange(struct tp_server *s) {
	if (!s || !s->locked || s->stopping)
		return;
	rebuild_bg_covers(s);
	struct tp_lock_surface *ls;
	wl_list_for_each(ls, &s->lock_surfaces, link) {
		if (!ls->mon || !ls->lock_surface)
			continue;
		if (ls->tree)
			wlr_scene_node_set_position(&ls->tree->node, ls->mon->area.x,
										ls->mon->area.y);
		if (ls->mon->area.w > 0 && ls->mon->area.h > 0)
			wlr_session_lock_surface_v1_configure(
				ls->lock_surface, (uint32_t)ls->mon->area.w,
				(uint32_t)ls->mon->area.h);
	}
}

void tp_session_lock_focus_surface(struct tp_server *s,
								   struct wlr_surface *surface) {
	if (!s || !s->locked || !surface)
		return;
	if (!wlr_session_lock_surface_v1_try_from_wlr_surface(surface))
		return;
	enter_lock_keyboard(s, surface);
}

void tp_session_lock_launch_argv(struct tp_server *s, const char *const argv[]) {
	if (!s || !s->display || !argv || !argv[0] || !*argv[0])
		return;
	int sv[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		tam_log_error("session-lock: socketpair failed: %s", strerror(errno));
		return;
	}
	struct wl_client *cl = wl_client_create(s->display, sv[0]);
	if (!cl) {
		tam_log_error("session-lock: wl_client_create failed");
		close(sv[0]);
		close(sv[1]);
		return;
	}
	tp_security_set_locker_client(s, cl);
	tp_spawn_with_fd_argv(argv, sv[1]);
}

void tp_session_lock_launch(struct tp_server *s, const char *cmd) {
	if (!s || !cmd || !*cmd)
		return;
	const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
	tp_session_lock_launch_argv(s, argv);
}
