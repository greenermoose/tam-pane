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
	struct wl_surface *surface;
	struct xdg_surface *xdg_surf;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buf;
	bool configured;
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

static struct wl_buffer *create_solid_shm(struct wl_shm *shm, int w, int h,
										  uint32_t argb) {
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
		px[i] = argb;
	munmap(px, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buf =
		wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
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

	wl_list_remove(&cap.commit.link);
	free(cap.rgba);

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
