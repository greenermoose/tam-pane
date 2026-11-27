/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Mesa GLES2 visuals module (plans/P5-visuals.md).
 *
 * Renders rounded window corners, rounded window borders, and analytical SDF
 * drop shadows directly into wlroots' output swapchain FBO when any visual
 * effect is enabled and the active renderer is GLES2. When all effects are
 * disabled (the default), this module is bypassed completely.
 */
#include "tam-pane/visuals.h"
#include "tam-pane/client.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/visuals_math.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <tam.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

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

struct tp_visuals {
	bool gl_ready;
	bool gl_failed;
	bool fallback_warned;
	bool ext_oes_supported;
	struct tp_tex_prog tex_2d;
	struct tp_tex_prog tex_ext;
	struct tp_rect_prog rect;
	struct tp_shadow_prog shadow;
	uint64_t frames_visual;
	uint64_t frames_plain;
};

struct tp_egl_saved {
	EGLDisplay dpy;
	EGLSurface draw;
	EGLSurface read;
	EGLContext ctx;
};

struct tp_clip_box {
	bool enabled;
	float cx, cy;
	float hw, hh;
	float radius;
};

struct tp_frame_ctx {
	struct tp_server *s;
	struct tp_monitor *m;
	struct tp_visuals *v;
	GLuint fbo;
	int fb_w, fb_h;
	float scale;
};

/* ---- GLES2 Shaders ---- */

static const char *VERT_TEX_SRC =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"    v_uv = a_uv;\n"
	"    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const char *VERT_POS_SRC =
	"attribute vec2 a_pos;\n"
	"void main() {\n"
	"    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const char *FRAG_TEX_2D_SRC =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_alpha;\n"
	"uniform float u_clip_enabled;\n"
	"uniform vec2 u_clip_center;\n"
	"uniform vec2 u_clip_half;\n"
	"uniform float u_clip_radius;\n"
	"float sdRoundedBox(vec2 p, vec2 b, float r) {\n"
	"    vec2 q = abs(p) - b + vec2(r);\n"
	"    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
	"}\n"
	"void main() {\n"
	"    vec4 col = texture2D(u_tex, v_uv) * u_alpha;\n"
	"    if (u_clip_enabled > 0.5 && u_clip_radius > 0.0) {\n"
	"        float d = sdRoundedBox(gl_FragCoord.xy - u_clip_center,\n"
	"                               u_clip_half, u_clip_radius);\n"
	"        float cov = 1.0 - smoothstep(-0.5, 0.5, d);\n"
	"        if (cov <= 0.001) discard;\n"
	"        col *= cov;\n"
	"    }\n"
	"    gl_FragColor = col;\n"
	"}\n";

static const char *FRAG_TEX_EXT_SRC =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform samplerExternalOES u_tex;\n"
	"uniform float u_alpha;\n"
	"uniform float u_clip_enabled;\n"
	"uniform vec2 u_clip_center;\n"
	"uniform vec2 u_clip_half;\n"
	"uniform float u_clip_radius;\n"
	"float sdRoundedBox(vec2 p, vec2 b, float r) {\n"
	"    vec2 q = abs(p) - b + vec2(r);\n"
	"    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
	"}\n"
	"void main() {\n"
	"    vec4 col = texture2D(u_tex, v_uv) * u_alpha;\n"
	"    if (u_clip_enabled > 0.5 && u_clip_radius > 0.0) {\n"
	"        float d = sdRoundedBox(gl_FragCoord.xy - u_clip_center,\n"
	"                               u_clip_half, u_clip_radius);\n"
	"        float cov = 1.0 - smoothstep(-0.5, 0.5, d);\n"
	"        if (cov <= 0.001) discard;\n"
	"        col *= cov;\n"
	"    }\n"
	"    gl_FragColor = col;\n"
	"}\n";

static const char *FRAG_RECT_SRC =
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"uniform float u_ring_enabled;\n"
	"uniform vec2 u_center;\n"
	"uniform vec2 u_outer_half;\n"
	"uniform float u_outer_radius;\n"
	"uniform vec2 u_inner_half;\n"
	"uniform float u_inner_radius;\n"
	"float sdRoundedBox(vec2 p, vec2 b, float r) {\n"
	"    vec2 q = abs(p) - b + vec2(r);\n"
	"    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
	"}\n"
	"void main() {\n"
	"    if (u_ring_enabled < 0.5) {\n"
	"        gl_FragColor = u_color;\n"
	"        return;\n"
	"    }\n"
	"    vec2 p = gl_FragCoord.xy - u_center;\n"
	"    float dout = sdRoundedBox(p, u_outer_half, u_outer_radius);\n"
	"    float din = sdRoundedBox(p, u_inner_half, u_inner_radius);\n"
	"    float cov = (1.0 - smoothstep(-0.5, 0.5, dout)) *\n"
	"                smoothstep(-0.5, 0.5, din);\n"
	"    if (cov <= 0.001) discard;\n"
	"    gl_FragColor = u_color * cov;\n"
	"}\n";

static const char *FRAG_SHADOW_SRC =
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"uniform vec2 u_center;\n"
	"uniform vec2 u_half;\n"
	"uniform float u_radius;\n"
	"uniform float u_blur;\n"
	"float sdRoundedBox(vec2 p, vec2 b, float r) {\n"
	"    vec2 q = abs(p) - b + vec2(r);\n"
	"    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
	"}\n"
	"void main() {\n"
	"    float d = sdRoundedBox(gl_FragCoord.xy - u_center, u_half, u_radius);\n"
	"    float t = clamp(d / max(u_blur, 1.0), 0.0, 1.0);\n"
	"    float falloff = (1.0 - t) * (1.0 - t);\n"
	"    if (d < 0.0) falloff = 1.0;\n"
	"    if (falloff <= 0.001) discard;\n"
	"    gl_FragColor = u_color * falloff;\n"
	"}\n";

/* ---- EGL context helpers ---- */

static bool egl_enter(struct tp_server *s, struct tp_egl_saved *saved) {
	struct wlr_egl *wlr_egl = wlr_gles2_renderer_get_egl(s->renderer);
	if (!wlr_egl)
		return false;
	EGLDisplay dpy = wlr_egl_get_display(wlr_egl);
	EGLContext ctx = wlr_egl_get_context(wlr_egl);
	if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT)
		return false;

	saved->dpy = eglGetCurrentDisplay();
	saved->draw = eglGetCurrentSurface(EGL_DRAW);
	saved->read = eglGetCurrentSurface(EGL_READ);
	saved->ctx = eglGetCurrentContext();

	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return false;
	return true;
}

static void egl_leave(struct tp_server *s, const struct tp_egl_saved *saved) {
	if (saved->dpy != EGL_NO_DISPLAY && saved->ctx != EGL_NO_CONTEXT) {
		eglMakeCurrent(saved->dpy, saved->draw, saved->read, saved->ctx);
		return;
	}
	struct wlr_egl *wlr_egl = wlr_gles2_renderer_get_egl(s->renderer);
	if (wlr_egl) {
		EGLDisplay dpy = wlr_egl_get_display(wlr_egl);
		if (dpy != EGL_NO_DISPLAY)
			eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	}
}

/* ---- Shader compilation ---- */

static GLuint compile_shader(GLenum type, const char *src) {
	GLuint sh = glCreateShader(type);
	if (!sh)
		return 0;
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[256] = {0};
		glGetShaderInfoLog(sh, (GLsizei)sizeof(log) - 1, NULL, log);
		tam_log_error("visuals: shader compile failed: %s", log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static GLuint link_program(const char *vs_src, const char *fs_src) {
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
	if (!vs)
		return 0;
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
	if (!fs) {
		glDeleteShader(vs);
		return 0;
	}
	GLuint prog = glCreateProgram();
	if (!prog) {
		glDeleteShader(vs);
		glDeleteShader(fs);
		return 0;
	}
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[256] = {0};
		glGetProgramInfoLog(prog, (GLsizei)sizeof(log) - 1, NULL, log);
		tam_log_error("visuals: program link failed: %s", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static bool init_tex_prog(struct tp_tex_prog *p, const char *fs_src) {
	p->id = link_program(VERT_TEX_SRC, fs_src);
	if (!p->id)
		return false;
	p->a_pos = glGetAttribLocation(p->id, "a_pos");
	p->a_uv = glGetAttribLocation(p->id, "a_uv");
	p->u_tex = glGetUniformLocation(p->id, "u_tex");
	p->u_alpha = glGetUniformLocation(p->id, "u_alpha");
	p->u_clip_enabled = glGetUniformLocation(p->id, "u_clip_enabled");
	p->u_clip_center = glGetUniformLocation(p->id, "u_clip_center");
	p->u_clip_half = glGetUniformLocation(p->id, "u_clip_half");
	p->u_clip_radius = glGetUniformLocation(p->id, "u_clip_radius");
	return true;
}

static void destroy_gl_programs(struct tp_visuals *v) {
	if (v->tex_2d.id)
		glDeleteProgram(v->tex_2d.id);
	if (v->tex_ext.id)
		glDeleteProgram(v->tex_ext.id);
	if (v->rect.id)
		glDeleteProgram(v->rect.id);
	if (v->shadow.id)
		glDeleteProgram(v->shadow.id);
	v->tex_2d.id = 0;
	v->tex_ext.id = 0;
	v->rect.id = 0;
	v->shadow.id = 0;
	v->gl_ready = false;
}

static bool ensure_gl_ready(struct tp_server *s, struct tp_visuals *v) {
	if (v->gl_ready)
		return true;
	if (v->gl_failed)
		return false;

	if (!init_tex_prog(&v->tex_2d, FRAG_TEX_2D_SRC))
		goto fail;

	v->ext_oes_supported =
		wlr_gles2_renderer_check_ext(s->renderer, "GL_OES_EGL_image_external");
	if (v->ext_oes_supported) {
		if (!init_tex_prog(&v->tex_ext, FRAG_TEX_EXT_SRC))
			v->ext_oes_supported = false;
	}

	v->rect.id = link_program(VERT_POS_SRC, FRAG_RECT_SRC);
	if (!v->rect.id)
		goto fail;
	v->rect.a_pos = glGetAttribLocation(v->rect.id, "a_pos");
	v->rect.u_color = glGetUniformLocation(v->rect.id, "u_color");
	v->rect.u_ring_enabled = glGetUniformLocation(v->rect.id, "u_ring_enabled");
	v->rect.u_center = glGetUniformLocation(v->rect.id, "u_center");
	v->rect.u_outer_half = glGetUniformLocation(v->rect.id, "u_outer_half");
	v->rect.u_outer_radius = glGetUniformLocation(v->rect.id, "u_outer_radius");
	v->rect.u_inner_half = glGetUniformLocation(v->rect.id, "u_inner_half");
	v->rect.u_inner_radius = glGetUniformLocation(v->rect.id, "u_inner_radius");

	v->shadow.id = link_program(VERT_POS_SRC, FRAG_SHADOW_SRC);
	if (!v->shadow.id)
		goto fail;
	v->shadow.a_pos = glGetAttribLocation(v->shadow.id, "a_pos");
	v->shadow.u_color = glGetUniformLocation(v->shadow.id, "u_color");
	v->shadow.u_center = glGetUniformLocation(v->shadow.id, "u_center");
	v->shadow.u_half = glGetUniformLocation(v->shadow.id, "u_half");
	v->shadow.u_radius = glGetUniformLocation(v->shadow.id, "u_radius");
	v->shadow.u_blur = glGetUniformLocation(v->shadow.id, "u_blur");

	v->gl_ready = true;
	return true;

fail:
	destroy_gl_programs(v);
	v->gl_failed = true;
	return false;
}

/* ---- Coordinate helpers ---- */

static void logical_rect_to_fb(const struct tp_frame_ctx *fc, double lx,
							   double ly, double lw, double lh, float *x0,
							   float *y0, float *x1, float *y1) {
	*x0 = (float)((lx - (double)fc->m->area.x) * (double)fc->scale);
	*y0 = (float)((ly - (double)fc->m->area.y) * (double)fc->scale);
	*x1 = (float)((lx + lw - (double)fc->m->area.x) * (double)fc->scale);
	*y1 = (float)((ly + lh - (double)fc->m->area.y) * (double)fc->scale);
}

static void quad_ndc(const struct tp_frame_ctx *fc, float x0, float y0,
					 float x1, float y1, GLfloat out[8]) {
	float nx0 = (2.0f * x0 / (float)fc->fb_w) - 1.0f;
	float ny0 = (2.0f * y0 / (float)fc->fb_h) - 1.0f;
	float nx1 = (2.0f * x1 / (float)fc->fb_w) - 1.0f;
	float ny1 = (2.0f * y1 / (float)fc->fb_h) - 1.0f;
	out[0] = nx0;
	out[1] = ny0;
	out[2] = nx1;
	out[3] = ny0;
	out[4] = nx0;
	out[5] = ny1;
	out[6] = nx1;
	out[7] = ny1;
}

/* ---- Primitive draw calls ---- */

static void draw_solid_rect(const struct tp_frame_ctx *fc, float x0, float y0,
							float x1, float y1, const float col[4]) {
	if (x1 <= x0 || y1 <= y0 || col[3] <= 0.0f)
		return;
	const struct tp_rect_prog *p = &fc->v->rect;
	GLfloat pos[8];
	quad_ndc(fc, x0, y0, x1, y1, pos);

	glUseProgram(p->id);
	glUniform4fv(p->u_color, 1, col);
	glUniform1f(p->u_ring_enabled, 0.0f);
	glVertexAttribPointer((GLuint)p->a_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glEnableVertexAttribArray((GLuint)p->a_pos);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray((GLuint)p->a_pos);
}

static void draw_rounded_border(const struct tp_frame_ctx *fc,
								struct tp_rect geom, int32_t border_w,
								float outer_r_logical, const float col[4]) {
	if (border_w <= 0 || col[3] <= 0.0f || geom.w <= 0 || geom.h <= 0)
		return;
	float x0, y0, x1, y1;
	logical_rect_to_fb(fc, geom.x, geom.y, geom.w, geom.h, &x0, &y0, &x1, &y1);
	if (x1 <= 0.0f || y1 <= 0.0f || x0 >= (float)fc->fb_w ||
		y0 >= (float)fc->fb_h)
		return;

	float cx = 0.5f * (x0 + x1);
	float cy = 0.5f * (y0 + y1);
	float ohw = 0.5f * (x1 - x0);
	float ohh = 0.5f * (y1 - y0);
	float bw_px = (float)border_w * fc->scale;
	float ihw = ohw - bw_px;
	float ihh = ohh - bw_px;
	if (ihw < 0.0f)
		ihw = 0.0f;
	if (ihh < 0.0f)
		ihh = 0.0f;
	float or_px = outer_r_logical * fc->scale;
	float ir_px = tp_visuals_inner_radius(outer_r_logical, border_w) * fc->scale;

	const struct tp_rect_prog *p = &fc->v->rect;
	GLfloat pos[8];
	quad_ndc(fc, x0, y0, x1, y1, pos);

	glUseProgram(p->id);
	glUniform4fv(p->u_color, 1, col);
	glUniform1f(p->u_ring_enabled, 1.0f);
	glUniform2f(p->u_center, cx, cy);
	glUniform2f(p->u_outer_half, ohw, ohh);
	glUniform1f(p->u_outer_radius, or_px);
	glUniform2f(p->u_inner_half, ihw, ihh);
	glUniform1f(p->u_inner_radius, ir_px);
	glVertexAttribPointer((GLuint)p->a_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glEnableVertexAttribArray((GLuint)p->a_pos);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray((GLuint)p->a_pos);
}

static void draw_shadow(const struct tp_frame_ctx *fc, struct tp_rect win,
						float radius_logical, const float col[4]) {
	const struct tp_settings *cfg = &fc->s->settings;
	if (!cfg->shadow_enabled || cfg->shadow_radius <= 0 || col[3] <= 0.0f)
		return;
	struct tp_rect sbox = tp_visuals_shadow_box(
		win, cfg->shadow_radius, cfg->shadow_offset_x, cfg->shadow_offset_y);
	float x0, y0, x1, y1;
	logical_rect_to_fb(fc, sbox.x, sbox.y, sbox.w, sbox.h, &x0, &y0, &x1, &y1);
	if (x1 <= 0.0f || y1 <= 0.0f || x0 >= (float)fc->fb_w ||
		y0 >= (float)fc->fb_h)
		return;

	float wx0, wy0, wx1, wy1;
	logical_rect_to_fb(fc, (double)win.x + cfg->shadow_offset_x,
					   (double)win.y + cfg->shadow_offset_y, win.w, win.h,
					   &wx0, &wy0, &wx1, &wy1);

	const struct tp_shadow_prog *p = &fc->v->shadow;
	GLfloat pos[8];
	quad_ndc(fc, x0, y0, x1, y1, pos);

	glUseProgram(p->id);
	glUniform4fv(p->u_color, 1, col);
	glUniform2f(p->u_center, 0.5f * (wx0 + wx1), 0.5f * (wy0 + wy1));
	glUniform2f(p->u_half, 0.5f * (wx1 - wx0), 0.5f * (wy1 - wy0));
	glUniform1f(p->u_radius, radius_logical * fc->scale);
	glUniform1f(p->u_blur, (float)cfg->shadow_radius * fc->scale);
	glVertexAttribPointer((GLuint)p->a_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glEnableVertexAttribArray((GLuint)p->a_pos);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray((GLuint)p->a_pos);
}

static void draw_scene_buffer(const struct tp_frame_ctx *fc,
							  struct wlr_scene_buffer *sb, int lx, int ly,
							  const struct tp_clip_box *clip) {
	if (!sb)
		return;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	struct wlr_texture *tex = NULL;
	bool owned_tex = false;
	if (ss && ss->surface) {
		tex = wlr_surface_get_texture(ss->surface);
	} else if (sb->buffer) {
		tex = wlr_texture_from_buffer(fc->s->renderer, sb->buffer);
		owned_tex = true;
		/* wlr_texture_from_buffer restores EGL context; rebind our FBO/state. */
		glBindFramebuffer(GL_FRAMEBUFFER, fc->fbo);
		glViewport(0, 0, fc->fb_w, fc->fb_h);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}
	if (!tex || !wlr_texture_is_gles2(tex)) {
		if (owned_tex && tex)
			wlr_texture_destroy(tex);
		return;
	}

	int dw = sb->dst_width;
	int dh = sb->dst_height;
	if (dw <= 0 || dh <= 0) {
		if (ss && ss->surface) {
			dw = ss->surface->current.width;
			dh = ss->surface->current.height;
		} else if (sb->buffer) {
			dw = sb->buffer->width;
			dh = sb->buffer->height;
		}
	}
	if (dw <= 0 || dh <= 0) {
		if (owned_tex)
			wlr_texture_destroy(tex);
		return;
	}

	float x0, y0, x1, y1;
	logical_rect_to_fb(fc, lx, ly, dw, dh, &x0, &y0, &x1, &y1);
	if (x1 <= 0.0f || y1 <= 0.0f || x0 >= (float)fc->fb_w ||
		y0 >= (float)fc->fb_h) {
		if (owned_tex)
			wlr_texture_destroy(tex);
		return;
	}

	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(tex, &attribs);

	const struct tp_tex_prog *p = NULL;
	if (attribs.target == GL_TEXTURE_2D)
		p = &fc->v->tex_2d;
	else if (attribs.target == GL_TEXTURE_EXTERNAL_OES &&
			 fc->v->ext_oes_supported)
		p = &fc->v->tex_ext;
	if (!p || !p->id) {
		if (owned_tex)
			wlr_texture_destroy(tex);
		return;
	}

	float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
	if (sb->src_box.width > 0.0 && sb->src_box.height > 0.0 &&
		tex->width > 0 && tex->height > 0) {
		u0 = (float)(sb->src_box.x / (double)tex->width);
		v0 = (float)(sb->src_box.y / (double)tex->height);
		u1 = (float)((sb->src_box.x + sb->src_box.width) / (double)tex->width);
		v1 = (float)((sb->src_box.y + sb->src_box.height) / (double)tex->height);
	}

	GLfloat pos[8];
	quad_ndc(fc, x0, y0, x1, y1, pos);
	GLfloat uv[8] = {u0, v0, u1, v0, u0, v1, u1, v1};

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(attribs.target, attribs.tex);
	GLint filter = (sb->filter_mode == WLR_SCALE_FILTER_NEAREST) ? GL_NEAREST
																 : GL_LINEAR;
	glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, filter);

	glUseProgram(p->id);
	glUniform1i(p->u_tex, 0);
	glUniform1f(p->u_alpha, sb->opacity);
	bool apply_clip = clip && clip->enabled && clip->radius > 0.0f;
	if (apply_clip && ss && ss->surface &&
		wlr_xdg_popup_try_from_wlr_surface(ss->surface))
		apply_clip = false;
	if (apply_clip) {
		glUniform1f(p->u_clip_enabled, 1.0f);
		glUniform2f(p->u_clip_center, clip->cx, clip->cy);
		glUniform2f(p->u_clip_half, clip->hw, clip->hh);
		glUniform1f(p->u_clip_radius, clip->radius);
	} else {
		glUniform1f(p->u_clip_enabled, 0.0f);
	}

	glVertexAttribPointer((GLuint)p->a_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glEnableVertexAttribArray((GLuint)p->a_pos);
	glVertexAttribPointer((GLuint)p->a_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glEnableVertexAttribArray((GLuint)p->a_uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray((GLuint)p->a_uv);
	glDisableVertexAttribArray((GLuint)p->a_pos);
	glBindTexture(attribs.target, 0);

	if (owned_tex)
		wlr_texture_destroy(tex);
}

/* ---- Scene traversal ---- */

static void render_node_tree(const struct tp_frame_ctx *fc,
							 struct wlr_scene_node *node, int parent_x,
							 int parent_y, const struct tp_clip_box *clip,
							 bool skip_rects) {
	if (!node || !node->enabled)
		return;
	int nx = parent_x + node->x;
	int ny = parent_y + node->y;

	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			render_node_tree(fc, child, nx, ny, clip, skip_rects);
		}
		break;
	}
	case WLR_SCENE_NODE_RECT: {
		if (skip_rects)
			break;
		struct wlr_scene_rect *r = wlr_scene_rect_from_node(node);
		float x0, y0, x1, y1;
		logical_rect_to_fb(fc, nx, ny, r->width, r->height, &x0, &y0, &x1, &y1);
		draw_solid_rect(fc, x0, y0, x1, y1, r->color);
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		draw_scene_buffer(fc, sb, nx, ny, clip);
		break;
	}
	}
}

static void render_client_tree(const struct tp_frame_ctx *fc,
							   struct tp_client *c) {
	if (!c || !c->tree || !c->tree->node.enabled)
		return;
	if (c->fullscreen) {
		render_node_tree(fc, &c->tree->node, 0, 0, NULL, false);
		return;
	}

	const struct tp_settings *cfg = &fc->s->settings;
	bool active = (fc->s->focused_client == c && !fc->s->locked);
	float outer_r =
		tp_visuals_clamp_radius(cfg->corner_radius, c->geom.w, c->geom.h);
	int32_t border_w = c->content_geom.x - c->geom.x;
	if (border_w < 0)
		border_w = 0;

	/* 1. Drop shadow behind the window. */
	const float *scol = active ? cfg->shadow_color : cfg->shadow_inactive_color;
	draw_shadow(fc, c->geom, outer_r, scol);

	/* 2. Client content with rounded-corner clipping. */
	struct tp_clip_box clip = {0};
	float inner_r = tp_visuals_inner_radius(outer_r, border_w);
	if (inner_r > 0.0f && c->content_geom.w > 0 && c->content_geom.h > 0) {
		float cx0, cy0, cx1, cy1;
		logical_rect_to_fb(fc, c->content_geom.x, c->content_geom.y,
						   c->content_geom.w, c->content_geom.h, &cx0, &cy0,
						   &cx1, &cy1);
		clip.enabled = true;
		clip.cx = 0.5f * (cx0 + cx1);
		clip.cy = 0.5f * (cy0 + cy1);
		clip.hw = 0.5f * (cx1 - cx0);
		clip.hh = 0.5f * (cy1 - cy0);
		clip.radius = inner_r * fc->scale;
	}
	if (c->content) {
		render_node_tree(fc, &c->content->node, c->tree->node.x,
						 c->tree->node.y, clip.enabled ? &clip : NULL, false);
	}

	/* 3. Rounded (or sharp) border ring on top of content edges. */
	const float *bcol = active ? cfg->border_color_focused
							   : cfg->border_color_unfocused;
	if (border_w > 0 && bcol[3] > 0.0f) {
		draw_rounded_border(fc, c->geom, border_w, outer_r, bcol);
	}
}

static void render_layer(const struct tp_frame_ctx *fc, int layer_idx) {
	struct wlr_scene_tree *layer = fc->s->layer[layer_idx];
	if (!layer || !layer->node.enabled)
		return;
	if (layer_idx != TP_LAYER_TILE && layer_idx != TP_LAYER_FLOAT) {
		render_node_tree(fc, &layer->node, 0, 0, NULL, false);
		return;
	}
	struct wlr_scene_node *child;
	wl_list_for_each(child, &layer->children, link) {
		if (!child->enabled)
			continue;
		if (child->type == WLR_SCENE_NODE_TREE && child->data) {
			render_client_tree(fc, child->data);
		} else {
			render_node_tree(fc, child, layer->node.x, layer->node.y, NULL,
							 false);
		}
	}
}

/* ---- Public API ---- */

bool tp_visuals_init(struct tp_server *s) {
	if (!s)
		return false;
	struct tp_visuals *v = calloc(1, sizeof(*v));
	if (!v)
		return false;
	s->visuals = v;
	return true;
}

void tp_visuals_reset_renderer(struct tp_server *s) {
	if (!s || !s->visuals)
		return;
	struct tp_visuals *v = s->visuals;
	if (v->gl_ready && s->renderer && wlr_renderer_is_gles2(s->renderer)) {
		struct tp_egl_saved saved;
		if (egl_enter(s, &saved)) {
			destroy_gl_programs(v);
			egl_leave(s, &saved);
		}
	}
	v->tex_2d.id = 0;
	v->tex_ext.id = 0;
	v->rect.id = 0;
	v->shadow.id = 0;
	v->gl_ready = false;
	v->gl_failed = false;
	v->fallback_warned = false;
}

void tp_visuals_finish(struct tp_server *s) {
	if (!s || !s->visuals)
		return;
	tp_visuals_reset_renderer(s);
	free(s->visuals);
	s->visuals = NULL;
}

void tp_visuals_damage_all(struct tp_server *s) {
	if (!s)
		return;
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		if (m->output && m->output->enabled)
			wlr_output_schedule_frame(m->output);
	}
}

void tp_visuals_note_plain_frame(struct tp_server *s) {
	if (s && s->visuals)
		s->visuals->frames_plain++;
}

void tp_visuals_get_stats(const struct tp_server *s,
						  struct tp_visuals_stats *out) {
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (!s)
		return;
	out->active = tp_visuals_enabled(&s->settings);
	out->gles2 = s->renderer && wlr_renderer_is_gles2(s->renderer);
	if (s->visuals) {
		out->frames_visual = s->visuals->frames_visual;
		out->frames_plain = s->visuals->frames_plain;
	}
}

bool tp_visuals_commit_output(struct tp_server *s, struct tp_monitor *m) {
	if (!s || !m || !m->output || !s->visuals || !s->renderer)
		return false;
	struct tp_visuals *v = s->visuals;

	if (!wlr_renderer_is_gles2(s->renderer)) {
		if (!v->fallback_warned) {
			tam_log_info("visuals: renderer is not GLES2; using plain scene path");
			v->fallback_warned = true;
		}
		return false;
	}

	/* Idle desktop: draw no frames when the output has no pending damage. */
	if (!m->output->needs_frame)
		return true;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (!wlr_output_configure_primary_swapchain(m->output, &state,
												&m->output->swapchain)) {
		wlr_output_state_finish(&state);
		return false;
	}

	struct wlr_buffer *buf = wlr_swapchain_acquire(m->output->swapchain);
	if (!buf) {
		wlr_output_state_finish(&state);
		return false;
	}

	struct tp_egl_saved saved;
	if (!egl_enter(s, &saved)) {
		wlr_buffer_unlock(buf);
		wlr_output_state_finish(&state);
		return false;
	}

	if (!ensure_gl_ready(s, v)) {
		egl_leave(s, &saved);
		wlr_buffer_unlock(buf);
		wlr_output_state_finish(&state);
		return false;
	}

	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(s->renderer, buf);
	if (!fbo) {
		egl_leave(s, &saved);
		wlr_buffer_unlock(buf);
		wlr_output_state_finish(&state);
		return false;
	}

	struct tp_frame_ctx fc = {
		.s = s,
		.m = m,
		.v = v,
		.fbo = fbo,
		.fb_w = buf->width,
		.fb_h = buf->height,
		.scale = m->output->scale > 0.0f ? m->output->scale : 1.0f,
	};

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, fc.fb_w, fc.fb_h);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	for (int i = 0; i < TP_LAYER_COUNT; i++)
		render_layer(&fc, i);

	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFlush();

	egl_leave(s, &saved);

	/* Draw software cursors (if any) on top using wlroots' buffer pass. */
	struct wlr_buffer_pass_options pass_opts = {
		.color_transform = s->gamma_transform,
	};
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(s->renderer, buf, &pass_opts);
	if (pass) {
		wlr_output_add_software_cursors_to_render_pass(m->output, pass, NULL);
		wlr_render_pass_submit(pass);
	}

	wlr_output_state_set_buffer(&state, buf);
	bool ok = wlr_output_commit_state(m->output, &state);
	wlr_buffer_unlock(buf);
	wlr_output_state_finish(&state);

	if (ok)
		v->frames_visual++;
	return ok;
}
