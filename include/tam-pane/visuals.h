/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Mesa GLES2 visuals module (plans/P5-visuals.md): rounded corners, rounded
 * window borders, and drop shadows. Lives in one self-contained module;
 * tp_client never grows effect flags, and when all effects are off the
 * compositor uses the plain wlr_scene_output_commit path.
 */
#ifndef TAM_PANE_VISUALS_H
#define TAM_PANE_VISUALS_H

#include <stdbool.h>
#include <stdint.h>

struct tp_monitor;
struct tp_server;

struct tp_visuals_stats {
	bool active;
	bool gles2;
	uint64_t frames_visual;
	uint64_t frames_plain;
};

/* Allocate visuals state on s->visuals (shaders compile lazily on GLES2). */
bool tp_visuals_init(struct tp_server *s);

/* Tear down GLES2 resources and free s->visuals (call before renderer destroy). */
void tp_visuals_finish(struct tp_server *s);

/* Drop GLES2 program handles before destroying a lost renderer on GPU reset. */
void tp_visuals_reset_renderer(struct tp_server *s);

/* Schedule a redraw on all enabled monitors (e.g. after live config reload). */
void tp_visuals_damage_all(struct tp_server *s);

/*
 * Render and commit one output frame using Mesa GLES2 rounded corners and
 * drop shadows. Returns true if the frame was committed; returns false when
 * the renderer is not GLES2 or shaders are unavailable so the caller falls
 * back cleanly to plain wlr_scene_output_commit().
 */
bool tp_visuals_commit_output(struct tp_server *s, struct tp_monitor *m);

/* Record a plain scene_output_commit frame (for idle-frame verification). */
void tp_visuals_note_plain_frame(struct tp_server *s);

/* Populate current visuals status and frame counters (for IPC queries). */
void tp_visuals_get_stats(const struct tp_server *s,
						  struct tp_visuals_stats *out);

#endif
