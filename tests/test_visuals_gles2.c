/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Headless Mesa GLES2 integration test for P5 visuals (rounded corners,
 * rounded borders, drop shadows, plain-scene bypass, zero idle frames,
 * and GPU renderer-loss recovery).
 */
#define _GNU_SOURCE
#include "check.h"
#include "tam-pane/client.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/visuals.h"
#include "tam-pane/visuals_math.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <tam.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wlr/backend/headless.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

struct pixel_capture {
	struct tp_server *server;
	struct wl_listener commit;
	uint8_t *rgba;
	int w, h;
	int commits;
};

static void on_capture_commit(struct wl_listener *l, void *data) {
	struct pixel_capture *cap = wl_container_of(l, cap, commit);
	struct wlr_output_event_commit *ev = data;
	if (!ev || !ev->state || !(ev->state->committed & WLR_OUTPUT_STATE_BUFFER) ||
		!ev->state->buffer)
		return;
	cap->commits++;
	struct tp_server *s = cap->server;
	if (!s || !s->renderer || !wlr_renderer_is_gles2(s->renderer))
		return;

	struct wlr_buffer *buf = ev->state->buffer;
	struct wlr_egl *wlr_egl = wlr_gles2_renderer_get_egl(s->renderer);
	if (!wlr_egl)
		return;
	EGLDisplay dpy = wlr_egl_get_display(wlr_egl);
	EGLContext ctx = wlr_egl_get_context(wlr_egl);
	EGLDisplay prev_dpy = eglGetCurrentDisplay();
	EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
	EGLContext prev_ctx = eglGetCurrentContext();
	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return;

	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(s->renderer, buf);
	if (fbo) {
		size_t need = (size_t)buf->width * (size_t)buf->height * 4;
		uint8_t *px = realloc(cap->rgba, need);
		if (px) {
			cap->rgba = px;
			cap->w = buf->width;
			cap->h = buf->height;
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glReadPixels(0, 0, buf->width, buf->height, GL_RGBA,
						 GL_UNSIGNED_BYTE, cap->rgba);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	if (prev_dpy != EGL_NO_DISPLAY && prev_ctx != EGL_NO_CONTEXT)
		eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx);
	else
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static void get_px(const struct pixel_capture *cap, int x, int y, uint8_t out[4]) {
	if (!cap || !cap->rgba || x < 0 || y < 0 || x >= cap->w || y >= cap->h) {
		memset(out, 0, 4);
		return;
	}
	const uint8_t *p = cap->rgba + ((size_t)y * (size_t)cap->w + (size_t)x) * 4;
	memcpy(out, p, 4);
}

/* ---- Minimal Wayland client ---- */

struct vis_client {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surf;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buf;
	struct wl_surface *panel_surface;
	struct zwlr_layer_surface_v1 *panel_layer;
	struct wl_buffer *panel_buf;
	bool configured;
	bool panel_configured;
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
							uint32_t serial, uint32_t w, uint32_t h) {
	struct vis_client *vc = data;
	(void)w;
	(void)h;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	vc->panel_configured = true;
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
	(void)data;
	(void)ls;
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void xdg_surf_configure(void *data, struct xdg_surface *xs,
							   uint32_t serial) {
	struct vis_client *vc = data;
	xdg_surface_ack_configure(xs, serial);
	vc->configured = true;
}
static const struct xdg_surface_listener xdg_surf_listener = {
	.configure = xdg_surf_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *tl, int32_t w,
							   int32_t h, struct wl_array *states) {
	(void)data;
	(void)tl;
	(void)w;
	(void)h;
	(void)states;
}
static void toplevel_close(void *data, struct xdg_toplevel *tl) {
	(void)data;
	(void)tl;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
					   const char *iface, uint32_t ver) {
	struct vis_client *vc = data;
	(void)ver;
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		vc->comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		vc->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		vc->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(vc->wm_base, &wm_listener, vc);
	} else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
		vc->layer_shell =
			wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
	}
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {
	(void)data;
	(void)reg;
	(void)name;
}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

static struct wl_buffer *create_solid_shm_fmt(struct wl_shm *shm, int w, int h,
											  uint32_t pixel, uint32_t fmt) {
	int stride = w * 4;
	size_t size = (size_t)stride * (size_t)h;
	int fd = memfd_create("tp-vis-test", MFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return NULL;
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
		px[i] = pixel;
	munmap(px, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buf =
		wl_shm_pool_create_buffer(pool, 0, w, h, stride, fmt);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static struct wl_buffer *create_solid_shm(struct wl_shm *shm, int w, int h,
										  uint32_t argb) {
	return create_solid_shm_fmt(shm, w, h, argb, WL_SHM_FORMAT_ARGB8888);
}

static void pump(struct tp_server *s, struct vis_client *vc, int iters) {
	for (int i = 0; i < iters; i++) {
		if (vc && vc->dpy) {
			wl_display_flush(vc->dpy);
			wl_display_dispatch_pending(vc->dpy);
		}
		wl_event_loop_dispatch(s->loop, 5);
		wl_display_flush_clients(s->display);
		if (vc && vc->dpy) {
			struct pollfd pfd = {
				.fd = wl_display_get_fd(vc->dpy),
				.events = POLLIN,
			};
			if (poll(&pfd, 1, 2) > 0)
				wl_display_dispatch(vc->dpy);
		}
	}
}

int main(void) {
	tam_log_set_level(TAM_LOG_WARN);

	struct tp_server s = {0};
	tp_settings_defaults(&s.settings);
	CHECK(tp_server_init(&s));
	CHECK(tp_server_start(&s));

	if (!wlr_renderer_is_gles2(s.renderer)) {
		fprintf(stderr, "SKIP: renderer is not GLES2\n");
		tp_server_finish(&s);
		return 0;
	}

	pump(&s, NULL, 10);
	CHECK(!wl_list_empty(&s.monitors));
	struct tp_monitor *m = wl_container_of(s.monitors.next, m, link);
	CHECK(m != NULL && m->output != NULL);

	/* Ensure known 640x480 mode on the headless output. */
	struct wlr_output_state mode_state;
	wlr_output_state_init(&mode_state);
	wlr_output_state_set_custom_mode(&mode_state, 640, 480, 60000);
	CHECK(wlr_output_commit_state(m->output, &mode_state));
	wlr_output_state_finish(&mode_state);
	pump(&s, NULL, 10);

	struct pixel_capture cap = {.server = &s};
	cap.commit.notify = on_capture_commit;
	wl_signal_add(&m->output->events.commit, &cap.commit);

	/* 1. Default settings: plain scene path, zero visual frames. */
	wlr_output_schedule_frame(m->output);
	pump(&s, NULL, 10);
	struct tp_visuals_stats st;
	tp_visuals_get_stats(&s, &st);
	CHECK(!st.active);
	CHECK(st.gles2);
	CHECK(st.frames_visual == 0);
	CHECK(st.frames_plain >= 1);

	/* Add a solid white background rect so shadow and corner clipping are clear. */
	float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	struct wlr_scene_rect *bg =
		wlr_scene_rect_create(s.layer[TP_LAYER_BACKGROUND], 640, 480, white);
	CHECK(bg != NULL);

	/* Connect a Wayland client and map a bright red floating window. */
	struct vis_client vc = {0};
	vc.dpy = wl_display_connect(s.socket);
	CHECK(vc.dpy != NULL);
	vc.reg = wl_display_get_registry(vc.dpy);
	wl_registry_add_listener(vc.reg, &reg_listener, &vc);
	pump(&s, &vc, 10);
	CHECK(vc.comp && vc.shm && vc.wm_base);

	vc.surface = wl_compositor_create_surface(vc.comp);
	vc.xdg_surf = xdg_wm_base_get_xdg_surface(vc.wm_base, vc.surface);
	xdg_surface_add_listener(vc.xdg_surf, &xdg_surf_listener, &vc);
	vc.toplevel = xdg_surface_get_toplevel(vc.xdg_surf);
	xdg_toplevel_add_listener(vc.toplevel, &toplevel_listener, &vc);
	wl_surface_commit(vc.surface);
	pump(&s, &vc, 10);
	CHECK(vc.configured);

	/* 192x152 content inside 200x160 geom with 4px border. */
	vc.buf = create_solid_shm(vc.shm, 192, 152, 0xFFFF0000u);
	CHECK(vc.buf != NULL);
	wl_surface_attach(vc.surface, vc.buf, 0, 0);
	wl_surface_commit(vc.surface);
	pump(&s, &vc, 10);

	CHECK(!wl_list_empty(&s.clients));
	struct tp_client *c = wl_container_of(s.clients.next, c, link);
	CHECK(c->mapped);

	/* 2. Enable P5 visuals: rounded corners + green border + drop shadow. */
	s.settings.border_width = 4;
	tp_settings_parse_color("#00ff00ff", s.settings.border_color_focused);
	s.settings.corner_radius = 16;
	s.settings.shadow_enabled = true;
	s.settings.shadow_radius = 16;
	s.settings.shadow_offset_x = 0;
	s.settings.shadow_offset_y = 6;
	tp_settings_parse_color("#000000b0", s.settings.shadow_color);

	tp_client_set_floating(c, true);
	c->float_geom = (struct tp_rect){100, 100, 200, 160};
	tp_client_place(c, c->float_geom);
	tp_arrange_flush(&s);
	CHECK(c->geom.x == 100 && c->geom.y == 100 && c->geom.w == 200 &&
		  c->geom.h == 160);
	tp_visuals_damage_all(&s);
	pump(&s, &vc, 12);

	tp_visuals_get_stats(&s, &st);
	CHECK(st.active);
	CHECK(st.frames_visual >= 1);
	CHECK(cap.rgba != NULL && cap.w == 640 && cap.h == 480);

	/* Pixel assertions: */
	uint8_t px[4];
	/* Far background (20, 20): pure white. */
	get_px(&cap, 20, 20, px);
	CHECK(px[0] > 250 && px[1] > 250 && px[2] > 250);

	/* Window interior (200, 180): pure red. */
	get_px(&cap, 200, 180, px);
	CHECK(px[0] > 240 && px[1] < 15 && px[2] < 15);

	/* Top border midpoint (200, 102): pure green. */
	get_px(&cap, 200, 102, px);
	CHECK(px[1] > 200 && px[0] < 40 && px[2] < 40);

	/* Top-left corner exterior (101, 101): rounded away (not red, not green). */
	get_px(&cap, 101, 101, px);
	CHECK(!(px[0] > 200 && px[1] < 40 && px[2] < 40));
	CHECK(!(px[1] > 200 && px[0] < 40 && px[2] < 40));

	/* Drop shadow below bottom edge (200, 266): darkened over white background. */
	get_px(&cap, 200, 266, px);
	CHECK(px[0] < 220 && px[1] < 220 && px[2] < 220);

	/* 3. Idle desktop draws 0 frames. */
	uint64_t vis_before = st.frames_visual;
	for (int i = 0; i < 20; i++)
		wl_event_loop_dispatch(s.loop, 10);
	tp_visuals_get_stats(&s, &st);
	CHECK(st.frames_visual == vis_before);

	/* 4. Toggle visuals off -> returns to plain scene path. */
	s.settings.corner_radius = 0;
	s.settings.shadow_enabled = false;
	uint64_t plain_before = st.frames_plain;
	tp_visuals_damage_all(&s);
	pump(&s, &vc, 10);
	tp_visuals_get_stats(&s, &st);
	CHECK(!st.active);
	CHECK(st.frames_visual == vis_before);
	CHECK(st.frames_plain > plain_before);

	/* 5. GPU renderer-loss recovery with visuals active. */
	s.settings.corner_radius = 12;
	s.settings.shadow_enabled = true;
	uint64_t vis_pre_reset = st.frames_visual;
	wl_signal_emit_mutable(&s.renderer->events.lost, NULL);
	pump(&s, &vc, 15);
	CHECK(s.renderer_recoveries == 1);
	tp_visuals_damage_all(&s);
	pump(&s, &vc, 10);
	tp_visuals_get_stats(&s, &st);
	CHECK(st.frames_visual > vis_pre_reset);

	/* 6. Dual-Kawase blur behind a semi-transparent window across a red/blue seam. */
	float red_bg[4] = {1.0f, 0.0f, 0.0f, 1.0f};
	float blue_bg[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	struct wlr_scene_rect *bg_left =
		wlr_scene_rect_create(s.layer[TP_LAYER_BACKGROUND], 200, 480, red_bg);
	struct wlr_scene_rect *bg_right =
		wlr_scene_rect_create(s.layer[TP_LAYER_BACKGROUND], 440, 480, blue_bg);
	CHECK(bg_left && bg_right);
	wlr_scene_node_set_position(&bg_right->node, 200, 0);

	/* 50% alpha dark grey window straddling x=200. */
	struct wl_buffer *trans_buf =
		create_solid_shm_fmt(vc.shm, 200, 160, 0x80202020u,
							 WL_SHM_FORMAT_ARGB8888);
	CHECK(trans_buf != NULL);
	wl_surface_attach(vc.surface, trans_buf, 0, 0);
	wl_surface_damage_buffer(vc.surface, 0, 0, 200, 160);
	wl_surface_commit(vc.surface);

	s.settings.border_width = 0;
	s.settings.corner_radius = 0;
	s.settings.shadow_enabled = false;
	s.settings.blur_enabled = true;
	s.settings.blur_radius = 8;
	s.settings.blur_passes = 2;
	s.settings.blur_panels = true;
	c->float_geom = (struct tp_rect){100, 100, 200, 160};
	tp_client_place(c, c->float_geom);
	tp_arrange_flush(&s);
	tp_visuals_damage_all(&s);
	pump(&s, &vc, 12);

	tp_visuals_get_stats(&s, &st);
	CHECK(st.active);
	CHECK(st.blur_ops >= 1);

	/* Near the red/blue seam inside the transparent window: blurred (both R and B). */
	get_px(&cap, 198, 180, px);
	CHECK(px[0] > 60 && px[2] > 60);
	/* Far from seam inside the window: red-dominant (overlay B=32, no blue bleed). */
	get_px(&cap, 120, 180, px);
	CHECK(px[0] > 100 && px[2] < 45);
	/* Outside the window near the seam: sharp red on left, sharp blue on right. */
	get_px(&cap, 198, 60, px);
	CHECK(px[0] > 240 && px[2] < 10);
	get_px(&cap, 202, 60, px);
	CHECK(px[2] > 240 && px[0] < 10);

	/* 7. Fully opaque (XRGB8888) window skips blur completely. */
	struct wl_buffer *opaque_buf =
		create_solid_shm_fmt(vc.shm, 200, 140, 0xFF303030u,
							 WL_SHM_FORMAT_XRGB8888);
	CHECK(opaque_buf != NULL);
	c->float_geom = (struct tp_rect){100, 300, 200, 140};
	tp_client_place(c, c->float_geom);
	tp_arrange_flush(&s);
	wl_surface_attach(vc.surface, opaque_buf, 0, 0);
	wl_surface_damage_buffer(vc.surface, 0, 0, 200, 140);
	wl_surface_commit(vc.surface);
	pump(&s, &vc, 10);

	tp_visuals_get_stats(&s, &st);
	uint64_t blur_before_opaque = st.blur_ops;
	tp_visuals_damage_all(&s);
	pump(&s, &vc, 10);
	tp_visuals_get_stats(&s, &st);
	CHECK(st.blur_ops == blur_before_opaque);

	/* 8. Semi-transparent TOP layer-shell panel blur (blur_panels true vs false). */
	CHECK(vc.layer_shell != NULL);
	vc.panel_surface = wl_compositor_create_surface(vc.comp);
	vc.panel_layer = zwlr_layer_shell_v1_get_layer_surface(
		vc.layer_shell, vc.panel_surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "test-panel");
	zwlr_layer_surface_v1_set_size(vc.panel_layer, 640, 40);
	zwlr_layer_surface_v1_set_anchor(vc.panel_layer,
									 ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
										 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
										 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_add_listener(vc.panel_layer, &layer_listener, &vc);
	wl_surface_commit(vc.panel_surface);
	pump(&s, &vc, 10);
	CHECK(vc.panel_configured);

	vc.panel_buf = create_solid_shm_fmt(vc.shm, 640, 40, 0x80202020u,
										WL_SHM_FORMAT_ARGB8888);
	CHECK(vc.panel_buf != NULL);
	wl_surface_attach(vc.panel_surface, vc.panel_buf, 0, 0);
	wl_surface_damage_buffer(vc.panel_surface, 0, 0, 640, 40);
	wl_surface_commit(vc.panel_surface);
	pump(&s, &vc, 12);

	tp_visuals_get_stats(&s, &st);
	CHECK(st.blur_ops > blur_before_opaque);
	/* Seam inside the transparent panel is blurred (both R and B). */
	get_px(&cap, 198, 20, px);
	CHECK(px[0] > 60 && px[2] > 60);

	/* Disable blur_panels -> seam behind panel becomes sharp red (overlay B=32). */
	s.settings.blur_panels = false;
	tp_visuals_damage_all(&s);
	pump(&s, &vc, 10);
	get_px(&cap, 198, 20, px);
	CHECK(px[0] > 100 && px[2] < 45);

	/* 9. Damage tracking: damaging only the bottom opaque window skips panel blur. */
	s.settings.blur_panels = true;
	tp_visuals_damage_all(&s);
	pump(&s, &vc, 10);
	/* Warm all swapchain buffers with full-frame renders so damage ring is steady. */
	for (int k = 0; k < 3; k++) {
		tp_visuals_damage_all(&s);
		pump(&s, &vc, 6);
	}
	tp_visuals_get_stats(&s, &st);
	uint64_t vis_pre_dmg = st.frames_visual;
	uint64_t blur_pre_dmg = st.blur_ops;

	wl_surface_damage_buffer(vc.surface, 10, 10, 20, 20);
	wl_surface_commit(vc.surface);
	pump(&s, &vc, 10);

	tp_visuals_get_stats(&s, &st);
	CHECK(st.frames_visual > vis_pre_dmg);
	CHECK(st.blur_ops == blur_pre_dmg);

	/* 10. GPU renderer-loss recovery with blur active recreates FBO pyramid. */
	wl_signal_emit_mutable(&s.renderer->events.lost, NULL);
	pump(&s, &vc, 15);
	CHECK(s.renderer_recoveries == 2);
	tp_visuals_get_stats(&s, &st);
	uint64_t blur_pre_reset = st.blur_ops;
	wl_surface_attach(vc.panel_surface, vc.panel_buf, 0, 0);
	wl_surface_damage_buffer(vc.panel_surface, 0, 0, 640, 40);
	wl_surface_commit(vc.panel_surface);
	tp_visuals_damage_all(&s);
	pump(&s, &vc, 12);
	tp_visuals_get_stats(&s, &st);
	CHECK(st.blur_ops > blur_pre_reset);
	get_px(&cap, 198, 20, px);
	CHECK(px[0] > 60 && px[2] > 60);

	/*
	 * 11. Frame-clock animations (0.4.4): disable corners/shadows/blur so we
	 * also verify plain-scene bypass when animations settle and go idle.
	 */
	s.settings.corner_radius = 0;
	s.settings.shadow_enabled = false;
	s.settings.blur_enabled = false;
	s.settings.animations_enabled = true;
	s.settings.animations_on_battery = false;
	s.settings.animation_duration_ms = 120;
	s.settings.animation_curve = TP_ANIM_CURVE_LINEAR;
	tp_visuals_set_battery_override(&s, 0); /* AC power */
	tp_visuals_on_config_reload(&s);
	pump(&s, &vc, 8);

	/* 11a. Open animation: triggers on open + first place, settles to 0 active. */
	tp_visuals_get_stats(&s, &st);
	CHECK(st.animations_active && !st.on_battery);
	struct tp_rect orig_geom = c->geom;
	tp_visuals_notify_client_open(&s, c);
	tp_client_place(c, orig_geom);
	tp_visuals_get_stats(&s, &st);
	CHECK(st.active_animations > 0);
	for (int k = 0; k < 25; k++) {
		pump(&s, &vc, 2);
		usleep(8000);
	}
	tp_visuals_get_stats(&s, &st);
	CHECK(st.active_animations == 0);

	/* 11b. Move animation: start a move, see active_animations > 0, settle, 0 idle frames. */
	uint64_t vis_before_move = st.frames_visual;
	struct tp_rect moved_geom = {orig_geom.x + 80, orig_geom.y + 40,
								 orig_geom.w, orig_geom.h};
	tp_client_place(c, moved_geom);
	tp_visuals_get_stats(&s, &st);
	CHECK(st.active_animations > 0);
	CHECK(tp_visuals_needs_render(&s));
	/* Pump past 120ms so animation progresses and finishes. */
	for (int k = 0; k < 25; k++) {
		pump(&s, &vc, 2);
		usleep(8000);
	}
	tp_visuals_get_stats(&s, &st);
	CHECK(st.frames_visual > vis_before_move);
	CHECK(st.active_animations == 0);
	CHECK(!tp_visuals_needs_render(&s)); /* back to plain scene path */

	/* Idle after animation: 0 frames drawn. */
	uint64_t idle_vis = st.frames_visual;
	uint64_t idle_plain = st.frames_plain;
	pump(&s, &vc, 10);
	tp_visuals_get_stats(&s, &st);
	CHECK(st.frames_visual == idle_vis);
	CHECK(st.frames_plain == idle_plain);

	/* 11b. Tag change animation: triggers, settles, and returns to 0 active. */
	tp_visuals_notify_tag_change(&s, m, tp_tag_mask(1), tp_tag_mask(2));
	tp_visuals_get_stats(&s, &st);
	CHECK(st.active_animations > 0);
	for (int k = 0; k < 25; k++) {
		pump(&s, &vc, 2);
		usleep(8000);
	}
	tp_visuals_get_stats(&s, &st);
	CHECK(st.active_animations == 0);

	/* 11c. Battery suppression: on_battery=true with animations_on_battery=false
	 * blocks animations from starting. */
	tp_visuals_set_battery_override(&s, 1); /* on battery */
	tp_visuals_get_stats(&s, &st);
	CHECK(st.on_battery && !st.animations_active);
	tp_client_place(c, orig_geom);
	tp_visuals_notify_tag_change(&s, m, tp_tag_mask(2), tp_tag_mask(1));
	tp_visuals_get_stats(&s, &st);
	CHECK(st.active_animations == 0);

	/* Opting in with animations_on_battery=true re-enables animations on battery. */
	s.settings.animations_on_battery = true;
	tp_visuals_get_stats(&s, &st);
	CHECK(st.on_battery && st.animations_active);
	tp_client_place(c, moved_geom);
	tp_visuals_get_stats(&s, &st);
	CHECK(st.active_animations > 0);
	for (int k = 0; k < 25; k++) {
		pump(&s, &vc, 2);
		usleep(8000);
	}
	tp_visuals_get_stats(&s, &st);
	CHECK(st.active_animations == 0);

	/* 11d. Close animation (ghost snapshot) + GPU reset while ghost is active. */
	wl_surface_attach(vc.surface, opaque_buf, 0, 0);
	wl_surface_damage_buffer(vc.surface, 0, 0, 200, 150);
	wl_surface_commit(vc.surface);
	pump(&s, &vc, 8);
	tp_visuals_set_battery_override(&s, 0);
	s.settings.animation_duration_ms = 200;
	tp_visuals_notify_client_close(&s, c);
	tp_visuals_get_stats(&s, &st);
	CHECK(st.active_animations > 0);
	pump(&s, &vc, 3);
	/* Trigger renderer loss mid-close-ghost to confirm clean GL teardown. */
	wl_signal_emit_mutable(&s.renderer->events.lost, NULL);
	pump(&s, &vc, 10);
	tp_visuals_get_stats(&s, &st);
	CHECK(st.active_animations == 0);
	CHECK(s.renderer_recoveries == 3);

	wl_list_remove(&cap.commit.link);
	free(cap.rgba);

	wl_buffer_destroy(vc.panel_buf);
	zwlr_layer_surface_v1_destroy(vc.panel_layer);
	wl_surface_destroy(vc.panel_surface);
	zwlr_layer_shell_v1_destroy(vc.layer_shell);
	wl_buffer_destroy(opaque_buf);
	wl_buffer_destroy(trans_buf);
	wl_buffer_destroy(vc.buf);
	xdg_toplevel_destroy(vc.toplevel);
	xdg_surface_destroy(vc.xdg_surf);
	wl_surface_destroy(vc.surface);
	xdg_wm_base_destroy(vc.wm_base);
	wl_shm_destroy(vc.shm);
	wl_compositor_destroy(vc.comp);
	wl_registry_destroy(vc.reg);
	wl_display_disconnect(vc.dpy);

	tp_server_finish(&s);
	CHECK_DONE("test_visuals_gles2");
}
