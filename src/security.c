/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/security.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <string.h>
#include <tam.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_security_context_v1.h>
#if defined(TP_XWAYLAND) && TP_XWAYLAND
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/xwayland.h>
#endif

/*
 * Every Wayland global advertised by tam-pane must be explicitly listed in
 * one of the two tables below and documented in arch/protocols.md.
 * Unclassified globals return TP_GLOBAL_UNKNOWN and fail closed (treated as
 * privileged) so a new global cannot be exposed to sandboxed clients by
 * accident.
 */
static const char *const UNPRIVILEGED_GLOBALS[] = {
	"wl_compositor",
	"wl_subcompositor",
	"wl_shm",
	"wl_drm",
	"zwp_linux_dmabuf_v1",
	"wp_linux_drm_syncobj_manager_v1",
	"wl_fixes",
	"wl_data_device_manager",
	"wl_output",
	"wl_seat",
	"xdg_wm_base",
	"wp_viewporter",
	"wp_fractional_scale_manager_v1",
	"wp_presentation",
	"zwp_idle_inhibit_manager_v1",
	"wp_cursor_shape_manager_v1",
	"zxdg_output_manager_v1",
	"zwp_text_input_manager_v3",
	"xdg_activation_v1",
	"zwp_primary_selection_device_manager_v1",
	"zxdg_decoration_manager_v1",
};

static const char *const PRIVILEGED_GLOBALS[] = {
	/* Active in 0.1.18 */
	"wp_security_context_manager_v1",
	"zwlr_layer_shell_v1",
	/* Planned P3 privileged globals (including capture-source companions and
	 * replacement clipboard protocols). */
	"zwlr_foreign_toplevel_manager_v1",
	"ext_foreign_toplevel_list_v1",
	"ext_workspace_manager_v1",
	"ext_session_lock_manager_v1",
	"ext_idle_notifier_v1",
	"zwlr_output_power_manager_v1",
	"zwlr_screencopy_manager_v1",
	"ext_image_copy_capture_manager_v1",
	"ext_output_image_capture_source_manager_v1",
	"ext_foreign_toplevel_image_capture_source_manager_v1",
	"zwlr_output_manager_v1",
	"zwlr_gamma_control_manager_v1",
	"zwlr_data_control_manager_v1",
	"ext_data_control_manager_v1",
	"zwp_input_method_manager_v2",
	"xwayland_shell_v1",
};

static bool list_contains(const char *const *list, size_t count,
						  const char *name) {
	for (size_t i = 0; i < count; i++) {
		if (strcmp(list[i], name) == 0)
			return true;
	}
	return false;
}

enum tp_global_class tp_security_classify_global(const char *interface_name) {
	if (!interface_name || !*interface_name)
		return TP_GLOBAL_UNKNOWN;
	if (list_contains(UNPRIVILEGED_GLOBALS,
					  sizeof(UNPRIVILEGED_GLOBALS) /
						  sizeof(UNPRIVILEGED_GLOBALS[0]),
					  interface_name))
		return TP_GLOBAL_UNPRIVILEGED;
	if (list_contains(PRIVILEGED_GLOBALS,
					  sizeof(PRIVILEGED_GLOBALS) /
						  sizeof(PRIVILEGED_GLOBALS[0]),
					  interface_name))
		return TP_GLOBAL_PRIVILEGED;
	return TP_GLOBAL_UNKNOWN;
}

bool tp_security_allow_privileged(bool has_security_context, uid_t peer_uid,
								  uid_t self_uid) {
	if (has_security_context)
		return false;
	if (peer_uid != self_uid)
		return false;
	return true;
}

static bool global_filter(const struct wl_client *client,
						  const struct wl_global *global, void *data) {
	struct tp_server *s = data;
	const struct wl_interface *iface = wl_global_get_interface(global);
	const char *name = iface ? iface->name : NULL;
	enum tp_global_class cls = tp_security_classify_global(name);

	if (cls == TP_GLOBAL_UNPRIVILEGED)
		return true;

	if (cls == TP_GLOBAL_UNKNOWN) {
		tam_log_warn("unclassified Wayland global '%s': treating as privileged",
					 name ? name : "(null)");
	}

	bool has_ctx =
		s->security_context_mgr &&
		wlr_security_context_manager_v1_lookup_client(s->security_context_mgr,
													  client) != NULL;
	uid_t peer_uid = (uid_t)-1;
	wl_client_get_credentials((struct wl_client *)client, NULL, &peer_uid,
							  NULL);
	if (!tp_security_allow_privileged(has_ctx, peer_uid, getuid()))
		return false;
	if (name && strcmp(name, "ext_session_lock_manager_v1") == 0)
		return tp_security_allow_session_lock(s, client);
	if (name &&
		(strcmp(name, "zwlr_screencopy_manager_v1") == 0 ||
		 strcmp(name, "ext_image_copy_capture_manager_v1") == 0 ||
		 strcmp(name, "ext_output_image_capture_source_manager_v1") == 0 ||
		 strcmp(name, "ext_foreign_toplevel_image_capture_source_manager_v1") ==
			 0))
		return tp_security_allow_capture(s, client);
	if (name && strcmp(name, "xwayland_shell_v1") == 0)
		return tp_security_allow_xwayland(s, client);
	return true;
}

bool tp_security_allow_exclusive_overlay(const struct tp_server *s,
										 const struct wl_client *client) {
	if (!s || !client)
		return false;
	bool has_ctx =
		s->security_context_mgr &&
		wlr_security_context_manager_v1_lookup_client(s->security_context_mgr,
													  client) != NULL;
	uid_t peer_uid = (uid_t)-1;
	wl_client_get_credentials((struct wl_client *)client, NULL, &peer_uid,
							  NULL);
	return tp_security_allow_privileged(has_ctx, peer_uid, getuid());
}

static void on_locker_client_destroy(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, locker_client_destroy);
	wl_list_remove(&s->locker_client_destroy.link);
	s->locker_client_destroy.notify = NULL;
	s->locker_client = NULL;
}

void tp_security_set_locker_client(struct tp_server *s,
								   struct wl_client *client) {
	if (!s || s->locker_client == client)
		return;
	if (s->locker_client_destroy.notify) {
		wl_list_remove(&s->locker_client_destroy.link);
		s->locker_client_destroy.notify = NULL;
	}
	s->locker_client = client;
	if (client) {
		s->locker_client_destroy.notify = on_locker_client_destroy;
		wl_client_add_destroy_listener(client, &s->locker_client_destroy);
	}
}

bool tp_security_allow_session_lock(const struct tp_server *s,
									const struct wl_client *client) {
	return s && client && s->locker_client == client;
}

struct tp_capture_client {
	struct wl_list link; /* tp_server.capture_clients */
	struct wl_client *client;
	struct wl_listener destroy;
};

static void on_capture_client_destroy(struct wl_listener *l, void *data) {
	struct tp_capture_client *cc = wl_container_of(l, cc, destroy);
	wl_list_remove(&cc->destroy.link);
	wl_list_remove(&cc->link);
	free(cc);
}

bool tp_security_add_capture_client(struct tp_server *s,
									struct wl_client *client) {
	if (!s || !client)
		return false;
	struct tp_capture_client *cc;
	wl_list_for_each(cc, &s->capture_clients, link) {
		if (cc->client == client)
			return true;
	}
	cc = calloc(1, sizeof(*cc));
	if (!cc)
		return false;
	cc->client = client;
	cc->destroy.notify = on_capture_client_destroy;
	wl_client_add_destroy_listener(client, &cc->destroy);
	wl_list_insert(&s->capture_clients, &cc->link);
	return true;
}

void tp_security_remove_capture_client(struct tp_server *s,
									   struct wl_client *client) {
	if (!s || !client)
		return;
	struct tp_capture_client *cc, *tmp;
	wl_list_for_each_safe(cc, tmp, &s->capture_clients, link) {
		if (cc->client == client) {
			wl_list_remove(&cc->destroy.link);
			wl_list_remove(&cc->link);
			free(cc);
			return;
		}
	}
}

bool tp_security_allow_capture(const struct tp_server *s,
							   const struct wl_client *client) {
	if (!s || !client)
		return false;
	const struct tp_capture_client *cc;
	wl_list_for_each(cc, &s->capture_clients, link) {
		if (cc->client == client)
			return true;
	}
	return false;
}

bool tp_security_allow_xwayland(const struct tp_server *s,
								const struct wl_client *client) {
#if defined(TP_XWAYLAND) && TP_XWAYLAND
	return s && client && s->xwayland && s->xwayland->server &&
		   s->xwayland->server->client == client;
#else
	(void)s;
	(void)client;
	return false;
#endif
}

bool tp_security_init(struct tp_server *s) {
	wl_list_init(&s->capture_clients);
	s->security_context_mgr =
		wlr_security_context_manager_v1_create(s->display);
	if (!s->security_context_mgr) {
		tam_log_error("cannot create wp_security_context_manager_v1");
		return false;
	}
	wl_display_set_global_filter(s->display, global_filter, s);
	return true;
}

void tp_security_finish(struct tp_server *s) {
	if (!s)
		return;
	tp_security_set_locker_client(s, NULL);
	if (s->capture_clients.next) {
		struct tp_capture_client *cc, *tmp;
		wl_list_for_each_safe(cc, tmp, &s->capture_clients, link) {
			wl_list_remove(&cc->destroy.link);
			wl_list_remove(&cc->link);
			free(cc);
		}
	}
}
