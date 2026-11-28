/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Mesa GLES2 visuals module (plans/P5-visuals.md): rounded corners, rounded
 * window borders, drop shadows, Dual-Kawase backdrop blur, and frame-clock
 * animations. Lives in one self-contained module; tp_client never grows effect
 * flags, and when all effects/animations are off or idle the compositor uses
 * the plain wlr_scene_output_commit path.
 */
#ifndef TAM_PANE_VISUALS_H
#define TAM_PANE_VISUALS_H

#include "tam-pane/geom.h"
#include "tam-pane/tags.h"
#include <stdbool.h>
#include <stdint.h>

struct tp_client;
struct tp_monitor;
struct tp_server;

struct tp_visuals_stats {
	bool active;
	bool gles2;
	bool animations_active;
	bool on_battery;
	uint32_t active_animations;
	uint64_t blur_ops;
	uint64_t frames_visual;
	uint64_t frames_plain;
};

/* Allocate visuals state on s->visuals (shaders compile lazily on GLES2). */
bool tp_visuals_init(struct tp_server *s);

/* Tear down GLES2 resources and free s->visuals (call before renderer destroy). */
void tp_visuals_finish(struct tp_server *s);

/* Drop GLES2 program and texture handles before destroying a lost renderer. */
void tp_visuals_reset_renderer(struct tp_server *s);

/* Schedule a redraw on all enabled monitors (e.g. after live config reload). */
void tp_visuals_damage_all(struct tp_server *s);

/* Refresh battery state and prune animations after a live config reload. */
void tp_visuals_on_config_reload(struct tp_server *s);

/*
 * True when the current frame must enter tp_visuals_commit_output(): either a
 * persistent visual effect (corners, shadow, blur) is enabled, or at least one
 * animation is currently running. When false, on_frame uses plain wlr_scene.
 */
bool tp_visuals_needs_render(const struct tp_server *s);

/*
 * Render and commit one output frame using Mesa GLES2 effects and animations.
 * Returns true if the frame was committed; returns false when the renderer is
 * not GLES2 or shaders are unavailable so the caller falls back cleanly to
 * plain wlr_scene_output_commit().
 */
bool tp_visuals_commit_output(struct tp_server *s, struct tp_monitor *m);

/* Record a plain scene_output_commit frame (for idle-frame verification). */
void tp_visuals_note_plain_frame(struct tp_server *s);

/* Populate current visuals status, animation state, and frame counters. */
void tp_visuals_get_stats(const struct tp_server *s,
						  struct tp_visuals_stats *out);

/* Narrow lifecycle hooks for frame-clock animations (tp_client stays clean). */
void tp_visuals_notify_client_open(struct tp_server *s, struct tp_client *c);
void tp_visuals_notify_client_move(struct tp_server *s, struct tp_client *c,
								   struct tp_rect from, struct tp_rect to);
void tp_visuals_notify_client_close(struct tp_server *s, struct tp_client *c);
void tp_visuals_notify_tag_change(struct tp_server *s, struct tp_monitor *m,
								  tp_tags_t prev_tags, tp_tags_t new_tags);
void tp_visuals_notify_monitor_destroy(struct tp_server *s,
									   struct tp_monitor *m);

/*
 * Override battery detection for automated tests:
 *   -1 = auto-detect via /sys/class/power_supply (default)
 *    0 = force AC power (not on battery)
 *    1 = force battery power
 */
void tp_visuals_set_battery_override(struct tp_server *s, int state);

#endif
