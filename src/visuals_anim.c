/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Frame-clock animation state machine for P5 visuals (plans/P5-visuals.md):
 * window open, close (with GLES2 surface snapshot ghost), move/resize, and
 * tag-change slide/fade, plus cached battery-power suppression.
 */
#include "tam-pane/client.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/visuals_math.h"
#include "visuals_internal.h"
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <tam.h>
#include <time.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

static const char *POWER_SUPPLY_DIR = "/sys/class/power_supply";

uint64_t tp_visuals_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

bool tp_visuals_is_on_battery(struct tp_visuals *v, uint64_t now_ms,
							  bool force) {
	if (!v)
		return false;
	if (v->battery_override >= 0) {
		v->on_battery = (v->battery_override != 0);
		v->battery_cached = true;
		v->battery_check_ms = now_ms;
		return v->on_battery;
	}
	if (!force && v->battery_cached && now_ms >= v->battery_check_ms &&
		(now_ms - v->battery_check_ms) < TP_BATTERY_CACHE_MS) {
		return v->on_battery;
	}
	v->on_battery = tp_visuals_detect_battery_dir(POWER_SUPPLY_DIR);
	v->battery_cached = true;
	v->battery_check_ms = now_ms;
	return v->on_battery;
}

bool tp_visuals_anim_is_active(const struct tp_server *s, struct tp_visuals *v,
							   uint64_t now_ms) {
	if (!s || !v)
		return false;
	if (!tp_visuals_anim_configured(&s->settings))
		return false;
	bool on_batt = tp_visuals_is_on_battery(v, now_ms, false);
	return tp_visuals_anim_allowed(&s->settings, on_batt);
}

static void schedule_monitor_anim_frame(struct tp_monitor *m) {
	if (!m)
		return;
	if (m->scene_output)
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
	if (m->output && m->output->enabled)
		wlr_output_schedule_frame(m->output);
}

static struct tp_client_anim *find_client_anim(struct tp_visuals *v,
											   uint64_t client_id) {
	for (int i = 0; i < TP_CLIENT_ANIM_MAX; i++) {
		if (v->client_anims[i].in_use &&
			v->client_anims[i].client_id == client_id)
			return &v->client_anims[i];
	}
	return NULL;
}

static struct tp_client_anim *alloc_client_anim(struct tp_visuals *v) {
	struct tp_client_anim *oldest = &v->client_anims[0];
	for (int i = 0; i < TP_CLIENT_ANIM_MAX; i++) {
		if (!v->client_anims[i].in_use)
			return &v->client_anims[i];
		if (v->client_anims[i].start_ms < oldest->start_ms)
			oldest = &v->client_anims[i];
	}
	memset(oldest, 0, sizeof(*oldest));
	return oldest;
}

static struct tp_tag_anim *find_tag_anim(struct tp_visuals *v,
										 const struct tp_monitor *m) {
	for (int i = 0; i < TP_TAG_ANIM_MAX; i++) {
		if (v->tag_anims[i].in_use && v->tag_anims[i].mon == m)
			return &v->tag_anims[i];
	}
	return NULL;
}

static struct tp_tag_anim *alloc_tag_anim(struct tp_visuals *v,
										  struct tp_monitor *m) {
	struct tp_tag_anim *existing = find_tag_anim(v, m);
	if (existing)
		return existing;
	struct tp_tag_anim *oldest = &v->tag_anims[0];
	for (int i = 0; i < TP_TAG_ANIM_MAX; i++) {
		if (!v->tag_anims[i].in_use)
			return &v->tag_anims[i];
		if (v->tag_anims[i].start_ms < oldest->start_ms)
			oldest = &v->tag_anims[i];
	}
	memset(oldest, 0, sizeof(*oldest));
	return oldest;
}

static void delete_ghost_tex(struct tp_server *s, struct tp_close_ghost *g) {
	if (!g->tex)
		return;
	if (s && s->renderer && wlr_renderer_is_gles2(s->renderer)) {
		struct tp_egl_saved saved;
		if (tp_visuals_egl_enter(s, &saved)) {
			glDeleteTextures(1, &g->tex);
			tp_visuals_egl_leave(s, &saved);
		}
	}
	g->tex = 0;
}

static bool capture_surface_texture(struct tp_server *s, struct tp_visuals *v,
									struct wlr_texture *src_tex,
									GLuint *out_tex, int *out_w, int *out_h) {
	if (!s || !v || !src_tex || !wlr_texture_is_gles2(src_tex))
		return false;
	if (src_tex->width <= 0 || src_tex->height <= 0 || src_tex->width > 4096 ||
		src_tex->height > 4096)
		return false;

	struct tp_egl_saved saved;
	if (!tp_visuals_egl_enter(s, &saved))
		return false;
	if (!tp_visuals_ensure_gl_ready(s, v)) {
		tp_visuals_egl_leave(s, &saved);
		return false;
	}

	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(src_tex, &attribs);
	const struct tp_tex_prog *p = NULL;
	if (attribs.target == GL_TEXTURE_2D)
		p = &v->tex_2d;
	else if (attribs.target == GL_TEXTURE_EXTERNAL_OES && v->ext_oes_supported)
		p = &v->tex_ext;
	if (!p || !p->id) {
		tp_visuals_egl_leave(s, &saved);
		return false;
	}

	int tw = src_tex->width;
	int th = src_tex->height;
	GLuint dst_tex = 0;
	GLuint fbo = 0;
	glGenTextures(1, &dst_tex);
	glBindTexture(GL_TEXTURE_2D, dst_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA,
				 GL_UNSIGNED_BYTE, NULL);

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
						   dst_tex, 0);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &fbo);
		glBindTexture(GL_TEXTURE_2D, 0);
		glDeleteTextures(1, &dst_tex);
		tp_visuals_egl_leave(s, &saved);
		return false;
	}

	static const GLfloat full_pos[8] = {
		-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
	};
	static const GLfloat full_uv[8] = {
		0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
	};

	glViewport(0, 0, tw, th);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(attribs.target, attribs.tex);
	glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glUseProgram(p->id);
	glUniform1i(p->u_tex, 0);
	glUniform1f(p->u_alpha, 1.0f);
	glUniform1f(p->u_clip_enabled, 0.0f);
	glVertexAttribPointer((GLuint)p->a_pos, 2, GL_FLOAT, GL_FALSE, 0, full_pos);
	glEnableVertexAttribArray((GLuint)p->a_pos);
	glVertexAttribPointer((GLuint)p->a_uv, 2, GL_FLOAT, GL_FALSE, 0, full_uv);
	glEnableVertexAttribArray((GLuint)p->a_uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray((GLuint)p->a_uv);
	glDisableVertexAttribArray((GLuint)p->a_pos);
	glBindTexture(attribs.target, 0);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glFlush();

	tp_visuals_egl_leave(s, &saved);
	*out_tex = dst_tex;
	*out_w = tw;
	*out_h = th;
	return true;
}

uint32_t tp_visuals_anim_count(const struct tp_visuals *v) {
	if (!v)
		return 0;
	uint32_t n = 0;
	for (int i = 0; i < TP_CLIENT_ANIM_MAX; i++) {
		if (v->client_anims[i].in_use && !v->client_anims[i].pending_geom)
			n++;
	}
	for (int i = 0; i < TP_CLOSE_GHOST_MAX; i++) {
		if (v->close_ghosts[i].in_use)
			n++;
	}
	for (int i = 0; i < TP_TAG_ANIM_MAX; i++) {
		if (v->tag_anims[i].in_use)
			n++;
	}
	return n;
}

bool tp_visuals_anim_has_active_on_monitor(const struct tp_visuals *v,
										   const struct tp_monitor *m) {
	if (!v || !m)
		return false;
	for (int i = 0; i < TP_CLIENT_ANIM_MAX; i++) {
		if (v->client_anims[i].in_use && !v->client_anims[i].pending_geom &&
			v->client_anims[i].mon == m)
			return true;
	}
	for (int i = 0; i < TP_CLOSE_GHOST_MAX; i++) {
		if (v->close_ghosts[i].in_use && v->close_ghosts[i].mon == m)
			return true;
	}
	for (int i = 0; i < TP_TAG_ANIM_MAX; i++) {
		if (v->tag_anims[i].in_use && v->tag_anims[i].mon == m)
			return true;
	}
	return false;
}

void tp_visuals_anim_client_transform(const struct tp_server *s,
									  const struct tp_visuals *v,
									  const struct tp_client *c,
									  uint64_t now_ms,
									  struct tp_anim_transform *out) {
	out->active = false;
	out->geom = c ? c->geom : (struct tp_rect){0, 0, 0, 0};
	out->alpha = 1.0f;
	if (!s || !v || !c)
		return;

	int curve = s->settings.animation_curve;

	for (int i = 0; i < TP_CLIENT_ANIM_MAX; i++) {
		const struct tp_client_anim *a = &v->client_anims[i];
		if (!a->in_use || a->pending_geom || a->client_id != c->id)
			continue;
		float p = tp_visuals_anim_progress(a->start_ms, now_ms, a->duration_ms);
		float e = tp_visuals_anim_ease(curve, p);
		if (a->kind == TP_ANIM_OPEN) {
			float scale = 0.90f + 0.10f * e;
			out->geom = tp_visuals_rect_scale_center(c->geom, scale);
			out->alpha *= e;
			out->active = true;
		} else if (a->kind == TP_ANIM_MOVE) {
			out->geom = tp_visuals_rect_lerp(a->from_geom, a->to_geom, e);
			out->active = true;
		}
		break;
	}

	if (c->mon && !(c->floating && c->pinned)) {
		for (int i = 0; i < TP_TAG_ANIM_MAX; i++) {
			const struct tp_tag_anim *ta = &v->tag_anims[i];
			if (!ta->in_use || ta->mon != c->mon)
				continue;
			/* Windows already visible on prev_tags stay steady. */
			if ((c->tags & ta->prev_tags) != 0)
				break;
			float p =
				tp_visuals_anim_progress(ta->start_ms, now_ms, ta->duration_ms);
			float e = tp_visuals_anim_ease(curve, p);
			int32_t dx =
				(int32_t)lroundf((float)(ta->dir * ta->slide_px) * (1.0f - e));
			out->geom.x += dx;
			out->alpha *= e;
			out->active = true;
			break;
		}
	}
}

bool tp_visuals_anim_prune_monitor(struct tp_server *s, struct tp_visuals *v,
								   struct tp_monitor *m, uint64_t now_ms) {
	if (!v || !m)
		return false;
	bool remaining = false;

	for (int i = 0; i < TP_CLIENT_ANIM_MAX; i++) {
		struct tp_client_anim *a = &v->client_anims[i];
		if (!a->in_use || a->pending_geom || a->mon != m)
			continue;
		if (tp_visuals_anim_progress(a->start_ms, now_ms, a->duration_ms) >=
			1.0f) {
			a->in_use = false;
		} else {
			remaining = true;
		}
	}

	for (int i = 0; i < TP_CLOSE_GHOST_MAX; i++) {
		struct tp_close_ghost *g = &v->close_ghosts[i];
		if (!g->in_use || g->mon != m)
			continue;
		if (tp_visuals_anim_progress(g->start_ms, now_ms, g->duration_ms) >=
			1.0f) {
			delete_ghost_tex(s, g);
			g->in_use = false;
		} else {
			remaining = true;
		}
	}

	for (int i = 0; i < TP_TAG_ANIM_MAX; i++) {
		struct tp_tag_anim *ta = &v->tag_anims[i];
		if (!ta->in_use || ta->mon != m)
			continue;
		if (tp_visuals_anim_progress(ta->start_ms, now_ms, ta->duration_ms) >=
			1.0f) {
			ta->in_use = false;
		} else {
			remaining = true;
		}
	}

	return remaining;
}

void tp_visuals_anim_clear_gl(struct tp_visuals *v) {
	if (!v)
		return;
	for (int i = 0; i < TP_CLOSE_GHOST_MAX; i++) {
		if (v->close_ghosts[i].tex) {
			glDeleteTextures(1, &v->close_ghosts[i].tex);
			v->close_ghosts[i].tex = 0;
		}
		v->close_ghosts[i].in_use = false;
	}
}

void tp_visuals_anim_clear_all(struct tp_server *s, struct tp_visuals *v) {
	if (!v)
		return;
	for (int i = 0; i < TP_CLIENT_ANIM_MAX; i++)
		v->client_anims[i].in_use = false;
	for (int i = 0; i < TP_TAG_ANIM_MAX; i++)
		v->tag_anims[i].in_use = false;
	for (int i = 0; i < TP_CLOSE_GHOST_MAX; i++) {
		if (v->close_ghosts[i].in_use) {
			delete_ghost_tex(s, &v->close_ghosts[i]);
			v->close_ghosts[i].in_use = false;
		}
	}
}

/* ---- Public notification hooks ---- */

void tp_visuals_notify_client_open(struct tp_server *s, struct tp_client *c) {
	if (!s || !s->visuals || !c || !c->mon || c->fullscreen || s->stopping)
		return;
	struct tp_visuals *v = s->visuals;
	uint64_t now_ms = tp_visuals_now_ms();
	if (!tp_visuals_anim_is_active(s, v, now_ms))
		return;
	if (!s->renderer || !wlr_renderer_is_gles2(s->renderer))
		return;

	struct tp_client_anim *a = find_client_anim(v, c->id);
	if (!a)
		a = alloc_client_anim(v);
	memset(a, 0, sizeof(*a));
	a->in_use = true;
	a->pending_geom = true;
	a->kind = TP_ANIM_OPEN;
	a->client_id = c->id;
	a->mon = c->mon;
	a->start_ms = now_ms;
	a->duration_ms = s->settings.animation_duration_ms;
}

void tp_visuals_notify_client_move(struct tp_server *s, struct tp_client *c,
								   struct tp_rect from, struct tp_rect to) {
	if (!s || !s->visuals || !c || !c->mon || s->stopping)
		return;
	struct tp_visuals *v = s->visuals;
	uint64_t now_ms = tp_visuals_now_ms();

	struct tp_client_anim *existing = find_client_anim(v, c->id);
	if (existing && existing->pending_geom) {
		if (to.w <= 0 || to.h <= 0 || c->fullscreen) {
			existing->in_use = false;
			return;
		}
		if (!tp_visuals_anim_is_active(s, v, now_ms)) {
			existing->in_use = false;
			return;
		}
		existing->pending_geom = false;
		existing->mon = c->mon;
		existing->from_geom = to;
		existing->to_geom = to;
		existing->start_ms = now_ms;
		existing->duration_ms = s->settings.animation_duration_ms;
		schedule_monitor_anim_frame(c->mon);
		return;
	}

	if (c->fullscreen || !tp_visuals_anim_is_active(s, v, now_ms))
		return;
	if (!s->renderer || !wlr_renderer_is_gles2(s->renderer))
		return;
	/* Never fight an interactive pointer drag/resize. */
	if (s->grab.mode != TP_GRAB_NONE && s->grab.client == c)
		return;
	/* Don't start move animations on top of a fresh tag slide on this monitor. */
	if (find_tag_anim(v, c->mon) != NULL)
		return;
	if (from.w <= 0 || from.h <= 0 || to.w <= 0 || to.h <= 0)
		return;
	if (from.x == to.x && from.y == to.y && from.w == to.w && from.h == to.h)
		return;

	struct tp_rect start_rect = from;
	if (existing && existing->in_use && existing->kind == TP_ANIM_MOVE) {
		float p = tp_visuals_anim_progress(existing->start_ms, now_ms,
										   existing->duration_ms);
		float e = tp_visuals_anim_ease(s->settings.animation_curve, p);
		start_rect = tp_visuals_rect_lerp(existing->from_geom,
										  existing->to_geom, e);
	}

	struct tp_client_anim *a = existing ? existing : alloc_client_anim(v);
	memset(a, 0, sizeof(*a));
	a->in_use = true;
	a->pending_geom = false;
	a->kind = TP_ANIM_MOVE;
	a->client_id = c->id;
	a->mon = c->mon;
	a->from_geom = start_rect;
	a->to_geom = to;
	a->start_ms = now_ms;
	a->duration_ms = s->settings.animation_duration_ms;
	schedule_monitor_anim_frame(c->mon);
}

void tp_visuals_notify_client_close(struct tp_server *s, struct tp_client *c) {
	if (!s || !s->visuals || !c)
		return;
	struct tp_visuals *v = s->visuals;

	struct tp_client_anim *existing = find_client_anim(v, c->id);
	if (existing)
		existing->in_use = false;

	if (s->stopping || !c->mapped || !tp_client_visible(c) || c->fullscreen ||
		!c->mon || c->geom.w <= 0 || c->geom.h <= 0 ||
		c->content_geom.w <= 0 || c->content_geom.h <= 0)
		return;

	uint64_t now_ms = tp_visuals_now_ms();
	if (!tp_visuals_anim_is_active(s, v, now_ms))
		return;
	if (!s->renderer || !wlr_renderer_is_gles2(s->renderer))
		return;

	struct wlr_surface *surf = tp_client_surface(c);
	struct wlr_texture *tex = surf ? wlr_surface_get_texture(surf) : NULL;
	if (!tex || !wlr_texture_is_gles2(tex))
		return;

	GLuint ghost_tex = 0;
	int tw = 0, th = 0;
	if (!capture_surface_texture(s, v, tex, &ghost_tex, &tw, &th))
		return;

	struct tp_close_ghost *slot = NULL;
	struct tp_close_ghost *oldest = &v->close_ghosts[0];
	for (int i = 0; i < TP_CLOSE_GHOST_MAX; i++) {
		if (!v->close_ghosts[i].in_use) {
			slot = &v->close_ghosts[i];
			break;
		}
		if (v->close_ghosts[i].start_ms < oldest->start_ms)
			oldest = &v->close_ghosts[i];
	}
	if (!slot) {
		delete_ghost_tex(s, oldest);
		slot = oldest;
	}

	memset(slot, 0, sizeof(*slot));
	bool active = (s->focused_client == c && !s->locked);
	int32_t border_w = c->content_geom.x - c->geom.x;
	if (border_w < 0)
		border_w = 0;
	slot->in_use = true;
	slot->mon = c->mon;
	slot->layer = c->floating ? TP_LAYER_FLOAT : TP_LAYER_TILE;
	slot->tex = ghost_tex;
	slot->tex_w = tw;
	slot->tex_h = th;
	slot->geom = c->geom;
	slot->content_geom = c->content_geom;
	slot->border_w = border_w;
	slot->outer_radius = tp_visuals_clamp_radius(s->settings.corner_radius,
												 c->geom.w, c->geom.h);
	const float *bcol = active ? s->settings.border_color_focused
							   : s->settings.border_color_unfocused;
	const float *scol = active ? s->settings.shadow_color
							   : s->settings.shadow_inactive_color;
	memcpy(slot->border_color, bcol, sizeof(slot->border_color));
	memcpy(slot->shadow_color, scol, sizeof(slot->shadow_color));
	slot->start_ms = now_ms;
	slot->duration_ms = s->settings.animation_duration_ms;

	schedule_monitor_anim_frame(c->mon);
}

void tp_visuals_notify_tag_change(struct tp_server *s, struct tp_monitor *m,
								  tp_tags_t prev_tags, tp_tags_t new_tags) {
	if (!s || !s->visuals || !m || prev_tags == new_tags || s->stopping)
		return;
	struct tp_visuals *v = s->visuals;
	uint64_t now_ms = tp_visuals_now_ms();
	if (!tp_visuals_anim_is_active(s, v, now_ms))
		return;
	if (!s->renderer || !wlr_renderer_is_gles2(s->renderer))
		return;

	int dir = tp_visuals_tag_slide_dir(prev_tags, new_tags);
	int32_t slide_px = m->usable.w / 6;
	if (slide_px > 160)
		slide_px = 160;
	if (slide_px < 24 && m->usable.w > 0)
		slide_px = 24;

	struct tp_tag_anim *ta = alloc_tag_anim(v, m);
	memset(ta, 0, sizeof(*ta));
	ta->in_use = true;
	ta->mon = m;
	ta->dir = dir;
	ta->slide_px = slide_px;
	ta->prev_tags = prev_tags;
	ta->new_tags = new_tags;
	ta->start_ms = now_ms;
	ta->duration_ms = s->settings.animation_duration_ms;

	schedule_monitor_anim_frame(m);
}

void tp_visuals_notify_monitor_destroy(struct tp_server *s,
									   struct tp_monitor *m) {
	if (!s || !s->visuals || !m)
		return;
	struct tp_visuals *v = s->visuals;
	for (int i = 0; i < TP_CLIENT_ANIM_MAX; i++) {
		if (v->client_anims[i].in_use && v->client_anims[i].mon == m)
			v->client_anims[i].in_use = false;
	}
	for (int i = 0; i < TP_CLOSE_GHOST_MAX; i++) {
		if (v->close_ghosts[i].in_use && v->close_ghosts[i].mon == m) {
			delete_ghost_tex(s, &v->close_ghosts[i]);
			v->close_ghosts[i].in_use = false;
		}
	}
	for (int i = 0; i < TP_TAG_ANIM_MAX; i++) {
		if (v->tag_anims[i].in_use && v->tag_anims[i].mon == m)
			v->tag_anims[i].in_use = false;
	}
}

void tp_visuals_on_config_reload(struct tp_server *s) {
	if (!s || !s->visuals)
		return;
	struct tp_visuals *v = s->visuals;
	uint64_t now_ms = tp_visuals_now_ms();
	bool on_batt = tp_visuals_is_on_battery(v, now_ms, true);
	if (!tp_visuals_anim_allowed(&s->settings, on_batt))
		tp_visuals_anim_clear_all(s, v);
	tp_visuals_damage_all(s);
}

void tp_visuals_set_battery_override(struct tp_server *s, int state) {
	if (!s || !s->visuals)
		return;
	struct tp_visuals *v = s->visuals;
	v->battery_override = state < 0 ? -1 : (state != 0 ? 1 : 0);
	uint64_t now_ms = tp_visuals_now_ms();
	bool on_batt = tp_visuals_is_on_battery(v, now_ms, true);
	if (!tp_visuals_anim_allowed(&s->settings, on_batt))
		tp_visuals_anim_clear_all(s, v);
}
