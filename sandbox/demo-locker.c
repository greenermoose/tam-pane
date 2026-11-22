/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal ext-session-lock-v1 demo client for sandbox/demo-0.2.0.
 * Launched by tam-pane via tp.lock (WAYLAND_SOCKET): locks the session,
 * paints a solid dark-blue surface on the output, and unlocks when Escape
 * or Enter is pressed. Not a real locker (tam-hide owns real locking).
 */
#define _POSIX_C_SOURCE 200809L
#include "ext-session-lock-v1-client-protocol.h"
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

struct demo_locker {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_output *output;
	struct ext_session_lock_manager_v1 *lock_mgr;
	struct ext_session_lock_v1 *lock;
	struct wl_surface *surface;
	struct ext_session_lock_surface_v1 *lock_surface;
	struct wl_buffer *buffer;
	bool locked;
	bool done;
};

static struct wl_buffer *make_solid_buffer(struct wl_shm *shm, uint32_t w,
										   uint32_t h, uint32_t xrgb) {
	if (!shm || w == 0 || h == 0)
		return NULL;
	size_t stride = (size_t)w * 4;
	size_t size = stride * h;
	char path[] = "/tmp/tam-pane-demo-locker-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0)
		return NULL;
	unlink(path);
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return NULL;
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	size_t n = (size_t)w * h;
	for (size_t i = 0; i < n; i++)
		px[i] = xrgb;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(
		pool, 0, (int32_t)w, (int32_t)h, (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd,
					  uint32_t sz) {
	close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s,
					 struct wl_surface *surf, struct wl_array *keys) {}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s,
					 struct wl_surface *surf) {}
static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial,
				   uint32_t time, uint32_t key, uint32_t state) {
	struct demo_locker *dl = data;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED &&
		(key == KEY_ESC || key == KEY_ENTER)) {
		if (dl->lock && dl->locked) {
			ext_session_lock_v1_unlock_and_destroy(dl->lock);
			dl->lock = NULL;
			wl_display_flush(dl->display);
		}
		dl->done = true;
	}
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep,
					uint32_t lat, uint32_t lck, uint32_t grp) {}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t del) {}
static const struct wl_keyboard_listener kb_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_mods,
	.repeat_info = kb_repeat,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
	struct demo_locker *dl = data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !dl->keyboard) {
		dl->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(dl->keyboard, &kb_listener, dl);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name = seat_name,
};

static void lock_surf_configure(void *data,
								struct ext_session_lock_surface_v1 *ls,
								uint32_t serial, uint32_t w, uint32_t h) {
	struct demo_locker *dl = data;
	ext_session_lock_surface_v1_ack_configure(ls, serial);
	if (dl->buffer)
		wl_buffer_destroy(dl->buffer);
	/* Solid deep slate-blue so the lock screen is unmistakable. */
	dl->buffer = make_solid_buffer(dl->shm, w, h, 0x162447);
	if (dl->buffer) {
		wl_surface_attach(dl->surface, dl->buffer, 0, 0);
		wl_surface_damage(dl->surface, 0, 0, (int32_t)w, (int32_t)h);
		wl_surface_commit(dl->surface);
	}
}
static const struct ext_session_lock_surface_v1_listener lock_surf_listener = {
	.configure = lock_surf_configure,
};

static void lock_locked(void *data, struct ext_session_lock_v1 *lock) {
	((struct demo_locker *)data)->locked = true;
}
static void lock_finished(void *data, struct ext_session_lock_v1 *lock) {
	struct demo_locker *dl = data;
	ext_session_lock_v1_destroy(lock);
	dl->lock = NULL;
	dl->done = true;
}
static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
					   const char *iface, uint32_t ver) {
	struct demo_locker *dl = data;
	if (!strcmp(iface, wl_compositor_interface.name))
		dl->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		dl->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name)) {
		dl->seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
		wl_seat_add_listener(dl->seat, &seat_listener, dl);
	} else if (!strcmp(iface, wl_output_interface.name) && !dl->output) {
		dl->output = wl_registry_bind(reg, name, &wl_output_interface, 1);
	} else if (!strcmp(iface, ext_session_lock_manager_v1_interface.name)) {
		dl->lock_mgr = wl_registry_bind(
			reg, name, &ext_session_lock_manager_v1_interface, 1);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

int main(void) {
	struct demo_locker dl = {0};
	dl.display = wl_display_connect(NULL);
	if (!dl.display) {
		fprintf(stderr, "demo-locker: cannot connect to Wayland display\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dl.display);
	wl_registry_add_listener(reg, &reg_listener, &dl);
	wl_display_roundtrip(dl.display);
	wl_display_roundtrip(dl.display);

	if (!dl.compositor || !dl.shm || !dl.output || !dl.lock_mgr) {
		fprintf(stderr, "demo-locker: missing required Wayland globals "
						"(ext_session_lock_manager_v1=%p)\n",
				(void *)dl.lock_mgr);
		wl_display_disconnect(dl.display);
		return 1;
	}

	dl.lock = ext_session_lock_manager_v1_lock(dl.lock_mgr);
	ext_session_lock_v1_add_listener(dl.lock, &lock_listener, &dl);
	dl.surface = wl_compositor_create_surface(dl.compositor);
	dl.lock_surface =
		ext_session_lock_v1_get_lock_surface(dl.lock, dl.surface, dl.output);
	ext_session_lock_surface_v1_add_listener(dl.lock_surface,
											 &lock_surf_listener, &dl);

	while (!dl.done && wl_display_dispatch(dl.display) != -1)
		;

	if (dl.lock_surface)
		ext_session_lock_surface_v1_destroy(dl.lock_surface);
	if (dl.surface)
		wl_surface_destroy(dl.surface);
	if (dl.buffer)
		wl_buffer_destroy(dl.buffer);
	wl_display_disconnect(dl.display);
	return 0;
}
