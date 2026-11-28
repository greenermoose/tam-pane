/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Private header shared between src/visuals.c and src/visuals_anim.c.
 */
#ifndef TAM_PANE_VISUALS_INTERNAL_H
#define TAM_PANE_VISUALS_INTERNAL_H

#include "tam-pane/geom.h"
#include "tam-pane/tags.h"
#include "tam-pane/visuals.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdbool.h>
#include <stdint.h>

enum {
	TP_BLUR_MAX_LEVELS = 7, /* level 0 = full res, 1..6 = half-res pyramid */
	TP_CLIENT_ANIM_MAX = 64,
	TP_CLOSE_GHOST_MAX = 16,
	TP_TAG_ANIM_MAX = 16,
	TP_BATTERY_CACHE_MS = 5000,
};

enum tp_client_anim_kind {
	TP_ANIM_OPEN = 1,
	TP_ANIM_MOVE = 2,
};

struct tp_tex_prog {
	GLuint id;
	GLint a_pos;
	GLint a_uv;
	GLint u_tex;
	GLint u_alpha;
	GLint u_clip_enabled;
	GLint u_clip_center;
	GLint u_clip_half;
	GLint u_clip_radius;
};

struct tp_rect_prog {
	GLuint id;
	GLint a_pos;
	GLint u_color;
	GLint u_ring_enabled;
	GLint u_center;
	GLint u_outer_half;
	GLint u_outer_radius;
	GLint u_inner_half;
	GLint u_inner_radius;
};

struct tp_shadow_prog {
	GLuint id;
	GLint a_pos;
	GLint u_color;
	GLint u_center;
	GLint u_half;
	GLint u_radius;
	GLint u_blur;
};

struct tp_blur_prog {
	GLuint id;
	GLint a_pos;
	GLint a_uv;
	GLint u_tex;
	GLint u_halfpixel;
	GLint u_offset;
	GLint u_uv_max;
};

struct tp_egl_saved {
	EGLDisplay dpy;
	EGLSurface draw;
	EGLSurface read;
	EGLContext ctx;
};

struct tp_client_anim {
	bool in_use;
	bool pending_geom; /* open recorded before first tp_client_place */
	enum tp_client_anim_kind kind;
	uint64_t client_id;
	struct tp_monitor *mon;
	struct tp_rect from_geom;
	struct tp_rect to_geom;
	uint64_t start_ms;
	int32_t duration_ms;
};

struct tp_close_ghost {
	bool in_use;
	struct tp_monitor *mon;
	int layer; /* TP_LAYER_TILE or TP_LAYER_FLOAT */
	GLuint tex;
	int tex_w;
	int tex_h;
	struct tp_rect geom;
	struct tp_rect content_geom;
	int32_t border_w;
	float outer_radius;
	float border_color[4];
	float shadow_color[4];
	uint64_t start_ms;
	int32_t duration_ms;
};

struct tp_tag_anim {
	bool in_use;
	struct tp_monitor *mon;
	int dir; /* -1, 0, +1 */
	int32_t slide_px;
	tp_tags_t prev_tags;
	tp_tags_t new_tags;
	uint64_t start_ms;
	int32_t duration_ms;
};

struct tp_anim_transform {
	bool active;
	struct tp_rect geom;
	float alpha;
};

struct tp_visuals {
	bool gl_ready;
	bool gl_failed;
	bool fallback_warned;
	bool ext_oes_supported;
	struct tp_tex_prog tex_2d;
	struct tp_tex_prog tex_ext;
	struct tp_rect_prog rect;
	struct tp_shadow_prog shadow;
	struct tp_blur_prog blur_down;
	struct tp_blur_prog blur_up;
	GLuint blur_fbo[TP_BLUR_MAX_LEVELS];
	GLuint blur_tex[TP_BLUR_MAX_LEVELS];
	int blur_w[TP_BLUR_MAX_LEVELS];
	int blur_h[TP_BLUR_MAX_LEVELS];
	int blur_levels;
	int blur_fb_w;
	int blur_fb_h;
	uint64_t blur_ops;
	uint64_t frames_visual;
	uint64_t frames_plain;

	/* Battery power cache (-1 = auto sysfs, 0 = AC, 1 = battery). */
	int battery_override;
	bool battery_cached;
	bool on_battery;
	uint64_t battery_check_ms;

	/* Frame-clock animation tables (owned here; tp_client stays clean). */
	struct tp_client_anim client_anims[TP_CLIENT_ANIM_MAX];
	struct tp_close_ghost close_ghosts[TP_CLOSE_GHOST_MAX];
	struct tp_tag_anim tag_anims[TP_TAG_ANIM_MAX];
};

/* Helpers in src/visuals.c used by src/visuals_anim.c */
bool tp_visuals_egl_enter(struct tp_server *s, struct tp_egl_saved *saved);
void tp_visuals_egl_leave(struct tp_server *s, const struct tp_egl_saved *saved);
bool tp_visuals_ensure_gl_ready(struct tp_server *s, struct tp_visuals *v);

/* Helpers in src/visuals_anim.c used by src/visuals.c */
uint64_t tp_visuals_now_ms(void);
bool tp_visuals_is_on_battery(struct tp_visuals *v, uint64_t now_ms, bool force);
bool tp_visuals_anim_is_active(const struct tp_server *s, struct tp_visuals *v,
							   uint64_t now_ms);
uint32_t tp_visuals_anim_count(const struct tp_visuals *v);
bool tp_visuals_anim_has_active_on_monitor(const struct tp_visuals *v,
										   const struct tp_monitor *m);
void tp_visuals_anim_client_transform(const struct tp_server *s,
									  const struct tp_visuals *v,
									  const struct tp_client *c,
									  uint64_t now_ms,
									  struct tp_anim_transform *out);
bool tp_visuals_anim_prune_monitor(struct tp_server *s, struct tp_visuals *v,
								   struct tp_monitor *m, uint64_t now_ms);
void tp_visuals_anim_clear_gl(struct tp_visuals *v);
void tp_visuals_anim_clear_all(struct tp_server *s, struct tp_visuals *v);

#endif
