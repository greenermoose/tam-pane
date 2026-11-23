/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Optional XWayland integration (0.2.6, Phase P3).
 *
 * Compiled only when built with XWAYLAND=1 (-DTP_XWAYLAND=1). Default builds
 * (XWAYLAND=0) use the inline no-op stubs below and carry zero xcb dependency.
 */
#ifndef TAM_PANE_XWAYLAND_H
#define TAM_PANE_XWAYLAND_H

#include "tam-pane/geom.h"
#include <stdbool.h>
#include <stddef.h>

struct tp_client;
struct tp_server;
struct wlr_surface;

#if defined(TP_XWAYLAND) && TP_XWAYLAND

void tp_xwayland_init(struct tp_server *s);
void tp_xwayland_finish(struct tp_server *s);

struct wlr_surface *tp_xwayland_surface(const struct tp_client *c);
const char *tp_xwayland_app_id(const struct tp_client *c);
const char *tp_xwayland_title(const struct tp_client *c);

void tp_xwayland_place(struct tp_client *c, struct tp_rect box);
void tp_xwayland_close(struct tp_client *c);
void tp_xwayland_set_activated(struct tp_client *c, bool activated);
void tp_xwayland_set_fullscreen(struct tp_client *c, bool fullscreen);
bool tp_xwayland_accepts_keyboard(const struct tp_client *c);

/* Managed tp_client owning an XWayland wlr_surface, or NULL. */
struct tp_client *tp_xwayland_client_from_surface(struct wlr_surface *surface);

#else

static inline void tp_xwayland_init(struct tp_server *s) { (void)s; }
static inline void tp_xwayland_finish(struct tp_server *s) { (void)s; }

static inline struct wlr_surface *
tp_xwayland_surface(const struct tp_client *c) {
	(void)c;
	return NULL;
}

static inline const char *tp_xwayland_app_id(const struct tp_client *c) {
	(void)c;
	return NULL;
}

static inline const char *tp_xwayland_title(const struct tp_client *c) {
	(void)c;
	return NULL;
}

static inline void tp_xwayland_place(struct tp_client *c, struct tp_rect box) {
	(void)c;
	(void)box;
}

static inline void tp_xwayland_close(struct tp_client *c) { (void)c; }

static inline void tp_xwayland_set_activated(struct tp_client *c,
											 bool activated) {
	(void)c;
	(void)activated;
}

static inline void tp_xwayland_set_fullscreen(struct tp_client *c,
											  bool fullscreen) {
	(void)c;
	(void)fullscreen;
}

static inline bool tp_xwayland_accepts_keyboard(const struct tp_client *c) {
	(void)c;
	return true;
}

static inline struct tp_client *
tp_xwayland_client_from_surface(struct wlr_surface *surface) {
	(void)surface;
	return NULL;
}

#endif

#endif
