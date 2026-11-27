/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/capture.h"
#include "tam-pane/client.h"
#include "tam-pane/monitor.h"
#include "tam-pane/security.h"
#include "tam-pane/server.h"
#include "tam-pane/spawn.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <tam.h>
#include <unistd.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>

static void on_client_capture_source_destroy(struct wl_listener *l,
											 void *data) {
	struct tp_client *c = wl_container_of(l, c, capture_source_destroy);
	wl_list_remove(&c->capture_source_destroy.link);
	c->capture_source_destroy.notify = NULL;
	c->capture_source = NULL;
}

void tp_capture_client_finish(struct tp_client *c) {
	if (!c)
		return;
	if (c->capture_source_destroy.notify) {
		wl_list_remove(&c->capture_source_destroy.link);
		c->capture_source_destroy.notify = NULL;
	}
	c->capture_source = NULL;
}

void tp_capture_reset_sources(struct tp_server *s) {
	if (!s)
		return;
	struct tp_client *c;
	wl_list_for_each(c, &s->clients, link) {
		tp_capture_client_finish(c);
	}
}

static void on_new_toplevel_capture_request(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_toplevel_capture_request);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *req =
		data;
	if (!req || !req->toplevel_handle || s->locked || s->stopping)
		return;

	struct tp_client *c = req->toplevel_handle->data;
	if (!c || !c->mapped || !c->tree)
		return;

	if (!c->capture_source) {
		c->capture_source =
			wlr_ext_image_capture_source_v1_create_with_scene_node(
				&c->content->node, s->loop, s->allocator, s->renderer);
		if (!c->capture_source) {
			tam_log_warn("capture: cannot create toplevel capture source");
			return;
		}
		c->capture_source_destroy.notify = on_client_capture_source_destroy;
		wl_signal_add(&c->capture_source->events.destroy,
					  &c->capture_source_destroy);
	}

	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
		req, c->capture_source);
}

bool tp_capture_init(struct tp_server *s) {
	s->screencopy_mgr = wlr_screencopy_manager_v1_create(s->display);
	if (!s->screencopy_mgr) {
		tam_log_error("cannot create zwlr_screencopy_manager_v1");
		return false;
	}
	s->image_copy_capture_mgr =
		wlr_ext_image_copy_capture_manager_v1_create(s->display, 1);
	if (!s->image_copy_capture_mgr) {
		tam_log_error("cannot create ext_image_copy_capture_manager_v1");
		return false;
	}
	s->output_capture_source_mgr =
		wlr_ext_output_image_capture_source_manager_v1_create(s->display, 1);
	if (!s->output_capture_source_mgr) {
		tam_log_error("cannot create ext_output_image_capture_source_manager_v1");
		return false;
	}
	s->toplevel_capture_source_mgr =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(
			s->display, 1);
	if (!s->toplevel_capture_source_mgr) {
		tam_log_error(
			"cannot create ext_foreign_toplevel_image_capture_source_manager_v1");
		return false;
	}
	s->new_toplevel_capture_request.notify = on_new_toplevel_capture_request;
	wl_signal_add(&s->toplevel_capture_source_mgr->events.new_request,
				  &s->new_toplevel_capture_request);
	return true;
}

void tp_capture_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->new_toplevel_capture_request.notify) {
		wl_list_remove(&s->new_toplevel_capture_request.link);
		s->new_toplevel_capture_request.notify = NULL;
	}
}

void tp_capture_launch_argv(struct tp_server *s, const char *const argv[]) {
	if (!s || !s->display || !argv || !argv[0] || !*argv[0])
		return;
	int sv[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		tam_log_error("capture: socketpair failed: %s", strerror(errno));
		return;
	}
	struct wl_client *cl = wl_client_create(s->display, sv[0]);
	if (!cl) {
		tam_log_error("capture: wl_client_create failed");
		close(sv[0]);
		close(sv[1]);
		return;
	}
	if (!tp_security_add_capture_client(s, cl)) {
		tam_log_error("capture: cannot register authorized client");
		wl_client_destroy(cl);
		close(sv[1]);
		return;
	}
	tp_spawn_with_fd_argv(argv, sv[1]);
}

void tp_capture_launch(struct tp_server *s, const char *cmd) {
	if (!s || !cmd || !*cmd)
		return;
	const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
	tp_capture_launch_argv(s, argv);
}

/* Render a single monitor's current scene into a locked wlr_buffer.
 * Caller must call wlr_buffer_unlock() on the returned buffer. */
static struct wlr_buffer *snapshot_monitor(struct tp_server *s,
										   struct tp_monitor *m) {
	if (!s || !m || !m->output || !m->scene_output || !m->output->enabled ||
		!s->allocator || !s->renderer)
		return NULL;
	int w = m->output->width;
	int h = m->output->height;
	if (w <= 0 || h <= 0)
		return NULL;

	const struct wlr_drm_format_set *fmts = wlr_renderer_get_texture_formats(
		s->renderer, s->renderer->render_buffer_caps);
	const struct wlr_drm_format *fmt = NULL;
	if (fmts) {
		fmt = wlr_drm_format_set_get(fmts, DRM_FORMAT_XRGB8888);
		if (!fmt)
			fmt = wlr_drm_format_set_get(fmts, DRM_FORMAT_ARGB8888);
		if (!fmt)
			fmt = wlr_drm_format_set_get(fmts, DRM_FORMAT_XBGR8888);
		if (!fmt)
			fmt = wlr_drm_format_set_get(fmts, DRM_FORMAT_ABGR8888);
	}
	struct wlr_drm_format fallback = {.format = DRM_FORMAT_XRGB8888};
	if (!fmt)
		fmt = &fallback;

	struct wlr_swapchain *sc = wlr_swapchain_create(s->allocator, w, h, fmt);
	if (!sc)
		return NULL;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	struct wlr_scene_output_state_options opts = {.swapchain = sc};
	struct wlr_buffer *buf = NULL;
	if (wlr_scene_output_build_state(m->scene_output, &state, &opts) &&
		state.buffer) {
		buf = wlr_buffer_lock(state.buffer);
	}
	wlr_output_state_finish(&state);
	wlr_swapchain_destroy(sc);
	return buf;
}

static bool extract_pixel_from_word(uint32_t fmt, uint32_t word,
									uint8_t rgb[3]) {
	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		rgb[0] = (uint8_t)((word >> 16) & 0xffu);
		rgb[1] = (uint8_t)((word >> 8) & 0xffu);
		rgb[2] = (uint8_t)(word & 0xffu);
		return true;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		rgb[0] = (uint8_t)(word & 0xffu);
		rgb[1] = (uint8_t)((word >> 8) & 0xffu);
		rgb[2] = (uint8_t)((word >> 16) & 0xffu);
		return true;
	default:
		return false;
	}
}

bool tp_capture_freeze(struct tp_server *s) {
	if (!s || s->locked || s->stopping || !s->layer[TP_LAYER_FREEZE])
		return false;

	tp_capture_unfreeze(s);

	struct wlr_scene_tree *tree =
		wlr_scene_tree_create(s->layer[TP_LAYER_FREEZE]);
	if (!tree)
		return false;

	size_t count = 0;
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		if (!m->output || !m->output->enabled || m->area.w <= 0 ||
			m->area.h <= 0)
			continue;
		struct wlr_buffer *buf = snapshot_monitor(s, m);
		if (!buf) {
			tam_log_warn("capture: freeze failed to snapshot output %s",
						 m->output->name ? m->output->name : "(unnamed)");
			continue;
		}
		struct wlr_scene_buffer *sb = wlr_scene_buffer_create(tree, buf);
		wlr_buffer_unlock(buf);
		if (!sb)
			continue;
		wlr_scene_node_set_position(&sb->node, m->area.x, m->area.y);
		wlr_scene_buffer_set_dest_size(sb, m->area.w, m->area.h);
		wlr_scene_buffer_set_transform(
			sb, wlr_output_transform_invert(m->output->transform));
		count++;
	}

	if (count == 0) {
		wlr_scene_node_destroy(&tree->node);
		return false;
	}

	s->freeze_tree = tree;
	s->frozen = true;
	wl_list_for_each(m, &s->monitors, link) {
		if (m->output && m->output->enabled)
			wlr_output_schedule_frame(m->output);
	}
	return true;
}

void tp_capture_unfreeze(struct tp_server *s) {
	if (!s)
		return;
	if (s->freeze_tree) {
		wlr_scene_node_destroy(&s->freeze_tree->node);
		s->freeze_tree = NULL;
	}
	bool was_frozen = s->frozen;
	s->frozen = false;
	if (was_frozen && !s->stopping) {
		struct tp_monitor *m;
		wl_list_for_each(m, &s->monitors, link) {
			if (m->output && m->output->enabled)
				wlr_output_schedule_frame(m->output);
		}
	}
}

bool tp_capture_sample_pixel(struct tp_server *s, int32_t lx, int32_t ly,
							 uint8_t rgb[3], char *err, size_t err_size) {
	if (!s || !rgb)
		return false;
	if (s->locked) {
		if (err && err_size > 0)
			snprintf(err, err_size, "session is locked");
		return false;
	}
	if (s->stopping || !s->output_layout) {
		if (err && err_size > 0)
			snprintf(err, err_size, "compositor is not running");
		return false;
	}

	struct wlr_output *out =
		wlr_output_layout_output_at(s->output_layout, (double)lx, (double)ly);
	struct tp_monitor *m = out ? out->data : NULL;
	if (!m || !m->output || !m->output->enabled) {
		if (err && err_size > 0)
			snprintf(err, err_size, "coordinates outside active outputs");
		return false;
	}

	struct wlr_buffer *buf = snapshot_monitor(s, m);
	if (!buf) {
		if (err && err_size > 0)
			snprintf(err, err_size, "cannot snapshot output");
		return false;
	}

	double ox = (double)(lx - m->area.x);
	double oy = (double)(ly - m->area.y);
	int sx = (int)floor(ox * (double)m->output->scale);
	int sy = (int)floor(oy * (double)m->output->scale);
	int eff_w = m->output->width;
	int eff_h = m->output->height;
	if (m->output->transform == WL_OUTPUT_TRANSFORM_90 ||
		m->output->transform == WL_OUTPUT_TRANSFORM_270 ||
		m->output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
		m->output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		int tmp = eff_w;
		eff_w = eff_h;
		eff_h = tmp;
	}
	if (sx < 0)
		sx = 0;
	if (sy < 0)
		sy = 0;
	if (sx >= eff_w)
		sx = eff_w - 1;
	if (sy >= eff_h)
		sy = eff_h - 1;

	struct wlr_box src = {.x = sx, .y = sy, .width = 1, .height = 1};
	struct wlr_box phys;
	wlr_box_transform(&phys, &src,
					  wlr_output_transform_invert(m->output->transform),
					  eff_w, eff_h);
	int px = phys.x;
	int py = phys.y;
	if (px < 0)
		px = 0;
	if (py < 0)
		py = 0;
	if (px >= buf->width)
		px = buf->width - 1;
	if (py >= buf->height)
		py = buf->height - 1;

	bool ok = false;
	void *data = NULL;
	uint32_t fmt = 0;
	size_t stride = 0;
	if (wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_READ,
										 &data, &fmt, &stride)) {
		const uint8_t *row = (const uint8_t *)data + (size_t)py * stride;
		uint32_t word = 0;
		memcpy(&word, row + (size_t)px * 4, sizeof(word));
		ok = extract_pixel_from_word(fmt, word, rgb);
		wlr_buffer_end_data_ptr_access(buf);
	}

	if (!ok && s->renderer) {
		struct wlr_texture *tex = wlr_texture_from_buffer(s->renderer, buf);
		if (tex) {
			uint32_t word = 0;
			struct wlr_texture_read_pixels_options ropts = {
				.data = &word,
				.format = DRM_FORMAT_XRGB8888,
				.stride = sizeof(word),
				.dst_x = 0,
				.dst_y = 0,
				.src_box = {.x = px, .y = py, .width = 1, .height = 1},
			};
			if (wlr_texture_read_pixels(tex, &ropts))
				ok = extract_pixel_from_word(DRM_FORMAT_XRGB8888, word, rgb);
			wlr_texture_destroy(tex);
		}
	}

	wlr_buffer_unlock(buf);
	if (!ok && err && err_size > 0)
		snprintf(err, err_size, "cannot read pixel from output buffer");
	return ok;
}
