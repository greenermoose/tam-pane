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
#include "visuals_internal.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <pixman.h>
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
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

struct tp_clip_box {
	bool enabled;
	float cx, cy;
	float hw, hh;
	float radius;
};

struct tp_node_xform {
	bool enabled;
	struct tp_rect src;
	struct tp_rect dst;
	float alpha;
};

struct tp_frame_ctx {
	struct tp_server *s;
	struct tp_monitor *m;
	struct tp_visuals *v;
	GLuint fbo;
	int fb_w, fb_h;
	float scale;
	struct tp_rect damage; /* framebuffer-space scissor box */
	bool has_damage;
	uint64_t now_ms;
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

static const char *FRAG_BLUR_DOWN_SRC =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_halfpixel;\n"
	"uniform float u_offset;\n"
	"uniform vec2 u_uv_max;\n"
	"vec4 tap(vec2 uv) {\n"
	"    return texture2D(u_tex, clamp(uv, vec2(0.0), u_uv_max));\n"
	"}\n"
	"void main() {\n"
	"    vec2 off = u_halfpixel * u_offset;\n"
	"    vec4 sum = tap(v_uv) * 4.0;\n"
	"    sum += tap(v_uv - off);\n"
	"    sum += tap(v_uv + off);\n"
	"    sum += tap(v_uv + vec2(off.x, -off.y));\n"
	"    sum += tap(v_uv - vec2(off.x, -off.y));\n"
	"    gl_FragColor = vec4((sum / 8.0).rgb, 1.0);\n"
	"}\n";

static const char *FRAG_BLUR_UP_SRC =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_halfpixel;\n"
	"uniform float u_offset;\n"
	"uniform vec2 u_uv_max;\n"
	"vec4 tap(vec2 uv) {\n"
	"    return texture2D(u_tex, clamp(uv, vec2(0.0), u_uv_max));\n"
	"}\n"
	"void main() {\n"
	"    vec2 off = u_halfpixel * u_offset;\n"
	"    vec4 sum = tap(v_uv + vec2(-off.x * 2.0, 0.0));\n"
	"    sum += tap(v_uv + vec2(-off.x, off.y)) * 2.0;\n"
	"    sum += tap(v_uv + vec2(0.0, off.y * 2.0));\n"
	"    sum += tap(v_uv + vec2(off.x, off.y)) * 2.0;\n"
	"    sum += tap(v_uv + vec2(off.x * 2.0, 0.0));\n"
	"    sum += tap(v_uv + vec2(off.x, -off.y)) * 2.0;\n"
	"    sum += tap(v_uv + vec2(0.0, -off.y * 2.0));\n"
	"    sum += tap(v_uv + vec2(-off.x, -off.y)) * 2.0;\n"
	"    gl_FragColor = vec4((sum / 12.0).rgb, 1.0);\n"
	"}\n";

/* ---- EGL context helpers ---- */

bool tp_visuals_egl_enter(struct tp_server *s, struct tp_egl_saved *saved) {
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

void tp_visuals_egl_leave(struct tp_server *s,
						  const struct tp_egl_saved *saved) {
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

static bool init_blur_prog(struct tp_blur_prog *p, const char *fs_src) {
	p->id = link_program(VERT_TEX_SRC, fs_src);
	if (!p->id)
		return false;
	p->a_pos = glGetAttribLocation(p->id, "a_pos");
	p->a_uv = glGetAttribLocation(p->id, "a_uv");
	p->u_tex = glGetUniformLocation(p->id, "u_tex");
	p->u_halfpixel = glGetUniformLocation(p->id, "u_halfpixel");
	p->u_offset = glGetUniformLocation(p->id, "u_offset");
	p->u_uv_max = glGetUniformLocation(p->id, "u_uv_max");
	return true;
}

static void destroy_blur_fbos(struct tp_visuals *v) {
	for (int i = 0; i < TP_BLUR_MAX_LEVELS; i++) {
		if (v->blur_fbo[i])
			glDeleteFramebuffers(1, &v->blur_fbo[i]);
		if (v->blur_tex[i])
			glDeleteTextures(1, &v->blur_tex[i]);
		v->blur_fbo[i] = 0;
		v->blur_tex[i] = 0;
		v->blur_w[i] = 0;
		v->blur_h[i] = 0;
	}
	v->blur_levels = 0;
	v->blur_fb_w = 0;
	v->blur_fb_h = 0;
}

static bool ensure_blur_fbos(struct tp_visuals *v, int fb_w, int fb_h,
							 int passes) {
	int need = passes + 1;
	if (need < 2)
		need = 2;
	if (need > TP_BLUR_MAX_LEVELS)
		need = TP_BLUR_MAX_LEVELS;
	if (v->blur_levels >= need && v->blur_fb_w == fb_w && v->blur_fb_h == fb_h)
		return true;

	destroy_blur_fbos(v);

	int w = fb_w > 0 ? fb_w : 1;
	int h = fb_h > 0 ? fb_h : 1;
	for (int i = 0; i < need; i++) {
		GLuint tex = 0, fbo = 0;
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
					 GL_UNSIGNED_BYTE, NULL);

		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
							   GL_TEXTURE_2D, tex, 0);
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		v->blur_tex[i] = tex;
		v->blur_fbo[i] = fbo;
		v->blur_w[i] = w;
		v->blur_h[i] = h;
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			tam_log_warn("visuals: blur FBO level %d incomplete (0x%x)", i,
						 (unsigned)status);
			glBindTexture(GL_TEXTURE_2D, 0);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			destroy_blur_fbos(v);
			return false;
		}
		w = w > 1 ? (w + 1) / 2 : 1;
		h = h > 1 ? (h + 1) / 2 : 1;
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	v->blur_levels = need;
	v->blur_fb_w = fb_w;
	v->blur_fb_h = fb_h;
	return true;
}

static void destroy_gl_programs(struct tp_visuals *v) {
	destroy_blur_fbos(v);
	tp_visuals_anim_clear_gl(v);
	if (v->tex_2d.id)
		glDeleteProgram(v->tex_2d.id);
	if (v->tex_ext.id)
		glDeleteProgram(v->tex_ext.id);
	if (v->rect.id)
		glDeleteProgram(v->rect.id);
	if (v->shadow.id)
		glDeleteProgram(v->shadow.id);
	if (v->blur_down.id)
		glDeleteProgram(v->blur_down.id);
	if (v->blur_up.id)
		glDeleteProgram(v->blur_up.id);
	v->tex_2d.id = 0;
	v->tex_ext.id = 0;
	v->rect.id = 0;
	v->shadow.id = 0;
	v->blur_down.id = 0;
	v->blur_up.id = 0;
	v->gl_ready = false;
}

bool tp_visuals_ensure_gl_ready(struct tp_server *s, struct tp_visuals *v) {
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

	if (!init_blur_prog(&v->blur_down, FRAG_BLUR_DOWN_SRC))
		goto fail;
	if (!init_blur_prog(&v->blur_up, FRAG_BLUR_UP_SRC))
		goto fail;

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

static bool fb_rect_visible(const struct tp_frame_ctx *fc, float x0, float y0,
							float x1, float y1) {
	if (x1 <= x0 || y1 <= y0)
		return false;
	if (x1 <= 0.0f || y1 <= 0.0f || x0 >= (float)fc->fb_w ||
		y0 >= (float)fc->fb_h)
		return false;
	if (fc->has_damage) {
		float dx0 = (float)fc->damage.x;
		float dy0 = (float)fc->damage.y;
		float dx1 = (float)(fc->damage.x + fc->damage.w);
		float dy1 = (float)(fc->damage.y + fc->damage.h);
		if (x1 <= dx0 || y1 <= dy0 || x0 >= dx1 || y0 >= dy1)
			return false;
	}
	return true;
}

static void restore_frame_state(const struct tp_frame_ctx *fc) {
	glBindFramebuffer(GL_FRAMEBUFFER, fc->fbo);
	glViewport(0, 0, fc->fb_w, fc->fb_h);
	if (fc->has_damage) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(fc->damage.x, fc->damage.y, fc->damage.w, fc->damage.h);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

static bool scene_buffer_is_transparent(struct wlr_scene_buffer *sb, int dw,
										int dh) {
	if (!sb || dw <= 0 || dh <= 0)
		return false;
	if (sb->opacity < 0.999f)
		return true;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	const pixman_region32_t *opaque =
		(ss && ss->surface) ? &ss->surface->opaque_region : &sb->opaque_region;
	pixman_box32_t box = {0, 0, dw, dh};
	return pixman_region32_contains_rectangle((pixman_region32_t *)opaque,
											  &box) != PIXMAN_REGION_IN;
}

static bool tree_has_transparent_buffer(struct wlr_scene_node *node) {
	if (!node || !node->enabled)
		return false;
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			if (tree_has_transparent_buffer(child))
				return true;
		}
		return false;
	}
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
		if (ss && ss->surface &&
			wlr_xdg_popup_try_from_wlr_surface(ss->surface))
			return false;
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
		return scene_buffer_is_transparent(sb, dw, dh);
	}
	return false;
}

static void run_blur_pass(const struct tp_blur_prog *prog, GLuint dst_fbo,
						  int dst_w, int dst_h, GLuint src_tex, int src_w,
						  int src_h, int src_cap_w, int src_cap_h,
						  float offset) {
	static const GLfloat full_pos[8] = {
		-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
	};
	float u1 = (float)src_w / (float)src_cap_w;
	float v1 = (float)src_h / (float)src_cap_h;
	GLfloat uv[8] = {0.0f, 0.0f, u1, 0.0f, 0.0f, v1, u1, v1};
	float uv_max_x = ((float)src_w - 0.5f) / (float)src_cap_w;
	float uv_max_y = ((float)src_h - 0.5f) / (float)src_cap_h;
	if (uv_max_x < 0.0f)
		uv_max_x = 0.0f;
	if (uv_max_y < 0.0f)
		uv_max_y = 0.0f;

	glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
	glViewport(0, 0, dst_w, dst_h);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src_tex);

	glUseProgram(prog->id);
	glUniform1i(prog->u_tex, 0);
	glUniform2f(prog->u_halfpixel, 0.5f / (float)src_cap_w,
				0.5f / (float)src_cap_h);
	glUniform1f(prog->u_offset, offset);
	glUniform2f(prog->u_uv_max, uv_max_x, uv_max_y);

	glVertexAttribPointer((GLuint)prog->a_pos, 2, GL_FLOAT, GL_FALSE, 0,
						  full_pos);
	glEnableVertexAttribArray((GLuint)prog->a_pos);
	glVertexAttribPointer((GLuint)prog->a_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glEnableVertexAttribArray((GLuint)prog->a_uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray((GLuint)prog->a_uv);
	glDisableVertexAttribArray((GLuint)prog->a_pos);
}

static void apply_backdrop_blur(const struct tp_frame_ctx *fc, float x0,
								float y0, float x1, float y1,
								const struct tp_clip_box *clip) {
	const struct tp_settings *cfg = &fc->s->settings;
	struct tp_visuals *v = fc->v;
	if (!tp_visuals_blur_active(cfg) || !v->blur_down.id || !v->blur_up.id)
		return;

	struct tp_rect bounds = {0, 0, fc->fb_w, fc->fb_h};
	int32_t rx0 = (int32_t)floorf(x0);
	int32_t ry0 = (int32_t)floorf(y0);
	int32_t rx1 = (int32_t)ceilf(x1);
	int32_t ry1 = (int32_t)ceilf(y1);
	struct tp_rect dst_rect = tp_visuals_rect_intersect(
		(struct tp_rect){rx0, ry0, rx1 - rx0, ry1 - ry0}, bounds);
	if (fc->has_damage)
		dst_rect = tp_visuals_rect_intersect(dst_rect, fc->damage);
	if (dst_rect.w <= 0 || dst_rect.h <= 0)
		return;

	int32_t pad_px = (int32_t)ceilf(
		(float)tp_visuals_blur_expand(cfg->blur_radius, cfg->blur_passes) *
		fc->scale);
	struct tp_rect src_rect =
		tp_visuals_damage_expand(dst_rect, pad_px, bounds);
	if (src_rect.w <= 0 || src_rect.h <= 0)
		return;

	int passes = cfg->blur_passes;
	if (passes < 1)
		passes = 1;
	if (passes >= TP_BLUR_MAX_LEVELS)
		passes = TP_BLUR_MAX_LEVELS - 1;

	if (!ensure_blur_fbos(v, fc->fb_w, fc->fb_h, passes)) {
		restore_frame_state(fc);
		return;
	}

	/* Copy the current backdrop region from fc->fbo into level 0. */
	glBindFramebuffer(GL_FRAMEBUFFER, fc->fbo);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, v->blur_tex[0]);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_rect.x, src_rect.y,
						src_rect.w, src_rect.h);

	int rw[TP_BLUR_MAX_LEVELS];
	int rh[TP_BLUR_MAX_LEVELS];
	rw[0] = src_rect.w;
	rh[0] = src_rect.h;
	for (int i = 1; i <= passes; i++) {
		rw[i] = rw[i - 1] > 1 ? (rw[i - 1] + 1) / 2 : 1;
		rh[i] = rh[i - 1] > 1 ? (rh[i - 1] + 1) / 2 : 1;
	}

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);

	float offset =
		tp_visuals_blur_offset(cfg->blur_radius, passes, fc->scale);

	/* Dual-Kawase downsample passes: 0 -> 1 -> ... -> passes. */
	for (int i = 0; i < passes; i++) {
		run_blur_pass(&v->blur_down, v->blur_fbo[i + 1], rw[i + 1], rh[i + 1],
					  v->blur_tex[i], rw[i], rh[i], v->blur_w[i], v->blur_h[i],
					  offset);
	}
	/* Dual-Kawase upsample passes: passes -> ... -> 1 -> 0. */
	for (int i = passes; i > 0; i--) {
		run_blur_pass(&v->blur_up, v->blur_fbo[i - 1], rw[i - 1], rh[i - 1],
					  v->blur_tex[i], rw[i], rh[i], v->blur_w[i], v->blur_h[i],
					  offset);
	}

	/* Composite blurred backdrop back onto fc->fbo over dst_rect. */
	restore_frame_state(fc);
	bool apply_clip = clip && clip->enabled && clip->radius > 0.0f;
	if (!apply_clip)
		glDisable(GL_BLEND);

	float bx0 = (float)dst_rect.x;
	float by0 = (float)dst_rect.y;
	float bx1 = (float)(dst_rect.x + dst_rect.w);
	float by1 = (float)(dst_rect.y + dst_rect.h);
	float u0 = (float)(dst_rect.x - src_rect.x) / (float)v->blur_w[0];
	float v0 = (float)(dst_rect.y - src_rect.y) / (float)v->blur_h[0];
	float u1 =
		(float)(dst_rect.x + dst_rect.w - src_rect.x) / (float)v->blur_w[0];
	float v1 =
		(float)(dst_rect.y + dst_rect.h - src_rect.y) / (float)v->blur_h[0];

	GLfloat pos[8];
	quad_ndc(fc, bx0, by0, bx1, by1, pos);
	GLfloat uv[8] = {u0, v0, u1, v0, u0, v1, u1, v1};

	const struct tp_tex_prog *p = &v->tex_2d;
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, v->blur_tex[0]);
	glUseProgram(p->id);
	glUniform1i(p->u_tex, 0);
	glUniform1f(p->u_alpha, 1.0f);
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
	glBindTexture(GL_TEXTURE_2D, 0);

	if (!apply_clip) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}
	v->blur_ops++;
}

static void draw_solid_rect(const struct tp_frame_ctx *fc, float x0, float y0,
							float x1, float y1, const float col[4]) {
	if (!fb_rect_visible(fc, x0, y0, x1, y1) || col[3] <= 0.0f)
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
	if (!fb_rect_visible(fc, x0, y0, x1, y1))
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
	if (!fb_rect_visible(fc, x0, y0, x1, y1))
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

static void xform_logical_rect(const struct tp_node_xform *xf, bool is_popup,
							   double lx, double ly, double lw, double lh,
							   double *ox, double *oy, double *ow, double *oh) {
	if (!xf || !xf->enabled || xf->src.w <= 0 || xf->src.h <= 0) {
		*ox = lx;
		*oy = ly;
		*ow = lw;
		*oh = lh;
		return;
	}
	if (is_popup) {
		*ox = lx + (double)(xf->dst.x - xf->src.x);
		*oy = ly + (double)(xf->dst.y - xf->src.y);
		*ow = lw;
		*oh = lh;
		return;
	}
	double sx = (double)xf->dst.w / (double)xf->src.w;
	double sy = (double)xf->dst.h / (double)xf->src.h;
	*ox = (double)xf->dst.x + (lx - (double)xf->src.x) * sx;
	*oy = (double)xf->dst.y + (ly - (double)xf->src.y) * sy;
	*ow = lw * sx;
	*oh = lh * sy;
}

static void draw_scene_buffer(const struct tp_frame_ctx *fc,
							  struct wlr_scene_buffer *sb, int lx, int ly,
							  const struct tp_clip_box *clip,
							  const struct tp_node_xform *xf,
							  bool blur_backdrop) {
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
		restore_frame_state(fc);
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

	bool is_popup =
		ss && ss->surface && wlr_xdg_popup_try_from_wlr_surface(ss->surface);
	double dlx = lx, dly = ly, dlw = dw, dlh = dh;
	xform_logical_rect(xf, is_popup, lx, ly, dw, dh, &dlx, &dly, &dlw, &dlh);
	float alpha = sb->opacity * ((xf && xf->enabled) ? xf->alpha : 1.0f);
	if (alpha <= 0.001f) {
		if (owned_tex)
			wlr_texture_destroy(tex);
		return;
	}

	float x0, y0, x1, y1;
	logical_rect_to_fb(fc, dlx, dly, dlw, dlh, &x0, &y0, &x1, &y1);
	if (!fb_rect_visible(fc, x0, y0, x1, y1)) {
		if (owned_tex)
			wlr_texture_destroy(tex);
		return;
	}

	if (blur_backdrop && !is_popup && scene_buffer_is_transparent(sb, dw, dh)) {
		apply_backdrop_blur(fc, x0, y0, x1, y1, clip);
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
	glUniform1f(p->u_alpha, alpha);
	bool apply_clip = clip && clip->enabled && clip->radius > 0.0f;
	if (apply_clip && is_popup)
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
							 const struct tp_node_xform *xf, bool skip_rects,
							 bool blur_backdrop) {
	if (!node || !node->enabled)
		return;
	int nx = parent_x + node->x;
	int ny = parent_y + node->y;

	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			render_node_tree(fc, child, nx, ny, clip, xf, skip_rects,
							 blur_backdrop);
		}
		break;
	}
	case WLR_SCENE_NODE_RECT: {
		if (skip_rects)
			break;
		struct wlr_scene_rect *r = wlr_scene_rect_from_node(node);
		double dlx = nx, dly = ny, dlw = r->width, dlh = r->height;
		xform_logical_rect(xf, false, nx, ny, r->width, r->height, &dlx, &dly,
						   &dlw, &dlh);
		float a = (xf && xf->enabled) ? xf->alpha : 1.0f;
		float col[4] = {
			r->color[0] * a,
			r->color[1] * a,
			r->color[2] * a,
			r->color[3] * a,
		};
		float x0, y0, x1, y1;
		logical_rect_to_fb(fc, dlx, dly, dlw, dlh, &x0, &y0, &x1, &y1);
		draw_solid_rect(fc, x0, y0, x1, y1, col);
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		draw_scene_buffer(fc, sb, nx, ny, clip, xf, blur_backdrop);
		break;
	}
	}
}

static void draw_close_ghost(const struct tp_frame_ctx *fc,
							 const struct tp_close_ghost *g) {
	if (!g || !g->in_use || !g->tex || g->mon != fc->m)
		return;
	const struct tp_settings *cfg = &fc->s->settings;
	float p = tp_visuals_anim_progress(g->start_ms, fc->now_ms, g->duration_ms);
	float e = tp_visuals_anim_ease(cfg->animation_curve, p);
	float alpha = 1.0f - e;
	if (alpha <= 0.001f)
		return;
	float scale = 1.0f - 0.10f * e;
	struct tp_rect geom = tp_visuals_rect_scale_center(g->geom, scale);
	struct tp_rect content = tp_rect_inset(geom, g->border_w);
	if (content.w <= 0 || content.h <= 0)
		return;

	float outer_r = tp_visuals_clamp_radius(cfg->corner_radius, geom.w, geom.h);
	float scol[4] = {
		g->shadow_color[0] * alpha,
		g->shadow_color[1] * alpha,
		g->shadow_color[2] * alpha,
		g->shadow_color[3] * alpha,
	};
	draw_shadow(fc, geom, outer_r, scol);

	float cx0, cy0, cx1, cy1;
	logical_rect_to_fb(fc, content.x, content.y, content.w, content.h, &cx0,
					   &cy0, &cx1, &cy1);
	if (fb_rect_visible(fc, cx0, cy0, cx1, cy1)) {
		float inner_r = tp_visuals_inner_radius(outer_r, g->border_w);
		const struct tp_tex_prog *prog = &fc->v->tex_2d;
		GLfloat pos[8];
		quad_ndc(fc, cx0, cy0, cx1, cy1, pos);
		static const GLfloat uv[8] = {
			0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
		};
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, g->tex);
		glUseProgram(prog->id);
		glUniform1i(prog->u_tex, 0);
		glUniform1f(prog->u_alpha, alpha);
		if (inner_r > 0.0f) {
			glUniform1f(prog->u_clip_enabled, 1.0f);
			glUniform2f(prog->u_clip_center, 0.5f * (cx0 + cx1),
						0.5f * (cy0 + cy1));
			glUniform2f(prog->u_clip_half, 0.5f * (cx1 - cx0),
						0.5f * (cy1 - cy0));
			glUniform1f(prog->u_clip_radius, inner_r * fc->scale);
		} else {
			glUniform1f(prog->u_clip_enabled, 0.0f);
		}
		glVertexAttribPointer((GLuint)prog->a_pos, 2, GL_FLOAT, GL_FALSE, 0,
							  pos);
		glEnableVertexAttribArray((GLuint)prog->a_pos);
		glVertexAttribPointer((GLuint)prog->a_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
		glEnableVertexAttribArray((GLuint)prog->a_uv);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glDisableVertexAttribArray((GLuint)prog->a_uv);
		glDisableVertexAttribArray((GLuint)prog->a_pos);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	if (g->border_w > 0 && g->border_color[3] > 0.0f) {
		float bcol[4] = {
			g->border_color[0] * alpha,
			g->border_color[1] * alpha,
			g->border_color[2] * alpha,
			g->border_color[3] * alpha,
		};
		draw_rounded_border(fc, geom, g->border_w, outer_r, bcol);
	}
}

static void render_client_tree(const struct tp_frame_ctx *fc,
							   struct tp_client *c) {
	if (!c || !c->tree || !c->tree->node.enabled)
		return;
	if (c->fullscreen) {
		render_node_tree(fc, &c->tree->node, 0, 0, NULL, NULL, false, false);
		return;
	}

	const struct tp_settings *cfg = &fc->s->settings;
	bool active = (fc->s->focused_client == c && !fc->s->locked);

	struct tp_anim_transform tr = {0};
	tp_visuals_anim_client_transform(fc->s, fc->v, c, fc->now_ms, &tr);
	struct tp_rect geom = tr.active ? tr.geom : c->geom;
	float alpha = tr.active ? tr.alpha : 1.0f;
	if (alpha <= 0.001f)
		return;

	float outer_r =
		tp_visuals_clamp_radius(cfg->corner_radius, geom.w, geom.h);
	int32_t border_w = c->content_geom.x - c->geom.x;
	if (border_w < 0)
		border_w = 0;
	struct tp_rect content_geom =
		tr.active ? tp_rect_inset(geom, border_w) : c->content_geom;

	/* 1. Drop shadow behind the window. */
	const float *base_scol =
		active ? cfg->shadow_color : cfg->shadow_inactive_color;
	float scol[4] = {
		base_scol[0] * alpha,
		base_scol[1] * alpha,
		base_scol[2] * alpha,
		base_scol[3] * alpha,
	};
	draw_shadow(fc, geom, outer_r, scol);

	/* 2. Backdrop blur (only for transparent windows) + client content. */
	struct tp_clip_box clip = {0};
	float inner_r = tp_visuals_inner_radius(outer_r, border_w);
	float cx0 = 0.0f, cy0 = 0.0f, cx1 = 0.0f, cy1 = 0.0f;
	if (content_geom.w > 0 && content_geom.h > 0) {
		logical_rect_to_fb(fc, content_geom.x, content_geom.y, content_geom.w,
						   content_geom.h, &cx0, &cy0, &cx1, &cy1);
		if (inner_r > 0.0f) {
			clip.enabled = true;
			clip.cx = 0.5f * (cx0 + cx1);
			clip.cy = 0.5f * (cy0 + cy1);
			clip.hw = 0.5f * (cx1 - cx0);
			clip.hh = 0.5f * (cy1 - cy0);
			clip.radius = inner_r * fc->scale;
		}
	}
	struct tp_node_xform xf = {
		.enabled = tr.active,
		.src = c->content_geom,
		.dst = content_geom,
		.alpha = alpha,
	};
	if (c->content) {
		if (tp_visuals_blur_active(cfg) && content_geom.w > 0 &&
			content_geom.h > 0 && fb_rect_visible(fc, cx0, cy0, cx1, cy1) &&
			tree_has_transparent_buffer(&c->content->node)) {
			apply_backdrop_blur(fc, cx0, cy0, cx1, cy1,
								clip.enabled ? &clip : NULL);
		}
		render_node_tree(fc, &c->content->node, c->tree->node.x,
						 c->tree->node.y, clip.enabled ? &clip : NULL,
						 tr.active ? &xf : NULL, false, false);
	}

	/* 3. Rounded (or sharp) border ring on top of content edges. */
	const float *base_bcol =
		active ? cfg->border_color_focused : cfg->border_color_unfocused;
	if (border_w > 0 && base_bcol[3] > 0.0f) {
		float bcol[4] = {
			base_bcol[0] * alpha,
			base_bcol[1] * alpha,
			base_bcol[2] * alpha,
			base_bcol[3] * alpha,
		};
		draw_rounded_border(fc, geom, border_w, outer_r, bcol);
	}
}

static void render_layer(const struct tp_frame_ctx *fc, int layer_idx) {
	struct wlr_scene_tree *layer = fc->s->layer[layer_idx];
	if (!layer || !layer->node.enabled)
		return;
	if (layer_idx != TP_LAYER_TILE && layer_idx != TP_LAYER_FLOAT) {
		bool blur_panel_layer =
			tp_visuals_blur_active(&fc->s->settings) &&
			fc->s->settings.blur_panels &&
			(layer_idx == TP_LAYER_BOTTOM || layer_idx == TP_LAYER_TOP ||
			 layer_idx == TP_LAYER_OVERLAY);
		render_node_tree(fc, &layer->node, 0, 0, NULL, NULL, false,
						 blur_panel_layer);
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
							 NULL, false, false);
		}
	}
	for (int i = 0; i < TP_CLOSE_GHOST_MAX; i++) {
		const struct tp_close_ghost *g = &fc->v->close_ghosts[i];
		if (g->in_use && g->mon == fc->m && g->layer == layer_idx)
			draw_close_ghost(fc, g);
	}
}

/* ---- Public API ---- */

bool tp_visuals_init(struct tp_server *s) {
	if (!s)
		return false;
	struct tp_visuals *v = calloc(1, sizeof(*v));
	if (!v)
		return false;
	v->battery_override = -1;
	s->visuals = v;
	return true;
}

void tp_visuals_reset_renderer(struct tp_server *s) {
	if (!s || !s->visuals)
		return;
	struct tp_visuals *v = s->visuals;
	if (v->gl_ready && s->renderer && wlr_renderer_is_gles2(s->renderer)) {
		struct tp_egl_saved saved;
		if (tp_visuals_egl_enter(s, &saved)) {
			destroy_gl_programs(v);
			tp_visuals_egl_leave(s, &saved);
		}
	}
	memset(v->client_anims, 0, sizeof(v->client_anims));
	memset(v->close_ghosts, 0, sizeof(v->close_ghosts));
	memset(v->tag_anims, 0, sizeof(v->tag_anims));
	v->tex_2d.id = 0;
	v->tex_ext.id = 0;
	v->rect.id = 0;
	v->shadow.id = 0;
	v->blur_down.id = 0;
	v->blur_up.id = 0;
	memset(v->blur_fbo, 0, sizeof(v->blur_fbo));
	memset(v->blur_tex, 0, sizeof(v->blur_tex));
	memset(v->blur_w, 0, sizeof(v->blur_w));
	memset(v->blur_h, 0, sizeof(v->blur_h));
	v->blur_levels = 0;
	v->blur_fb_w = 0;
	v->blur_fb_h = 0;
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
		if (m->scene_output)
			wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
		if (m->output && m->output->enabled)
			wlr_output_schedule_frame(m->output);
	}
}

bool tp_visuals_needs_render(const struct tp_server *s) {
	if (!s)
		return false;
	if (tp_visuals_enabled(&s->settings))
		return true;
	if (s->visuals && tp_visuals_anim_count(s->visuals) > 0)
		return true;
	return false;
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
	out->gles2 = s->renderer && wlr_renderer_is_gles2(s->renderer);
	if (s->visuals) {
		uint64_t now_ms = tp_visuals_now_ms();
		out->on_battery = tp_visuals_is_on_battery(s->visuals, now_ms, false);
		out->animations_active =
			tp_visuals_anim_allowed(&s->settings, out->on_battery);
		out->active_animations = tp_visuals_anim_count(s->visuals);
		out->blur_ops = s->visuals->blur_ops;
		out->frames_visual = s->visuals->frames_visual;
		out->frames_plain = s->visuals->frames_plain;
	} else {
		out->animations_active = tp_visuals_anim_allowed(&s->settings, false);
	}
	out->active =
		tp_visuals_enabled(&s->settings) || out->active_animations > 0;
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

	uint64_t now_ms = tp_visuals_now_ms();
	bool anim_on_m = tp_visuals_anim_has_active_on_monitor(v, m);

	/* Idle desktop: draw no frames when the output has no pending damage. */
	if (!m->output->needs_frame && !anim_on_m)
		return true;

	if (anim_on_m && m->scene_output)
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);

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

	float scale = m->output->scale > 0.0f ? m->output->scale : 1.0f;
	struct tp_rect fb_bounds = {0, 0, buf->width, buf->height};
	int32_t pad_px =
		(int32_t)ceilf((float)tp_visuals_effect_pad(&s->settings) * scale);

	pixman_region32_t buffer_damage;
	pixman_region32_t output_damage;
	pixman_region32_init(&buffer_damage);
	pixman_region32_init(&output_damage);

	bool have_ring = (m->scene_output != NULL);
	struct tp_rect render_damage = fb_bounds;
	bool need_draw = true;

	if (have_ring) {
		pixman_region32_copy(&output_damage,
							 &m->scene_output->damage_ring.current);
		pixman_region32_intersect_rect(&output_damage, &output_damage, 0, 0,
									   buf->width, buf->height);
		wlr_damage_ring_rotate_buffer(&m->scene_output->damage_ring, buf,
									  &buffer_damage);
		pixman_region32_intersect_rect(&buffer_damage, &buffer_damage, 0, 0,
									   buf->width, buf->height);
		if (!pixman_region32_not_empty(&buffer_damage)) {
			need_draw = false;
			render_damage = (struct tp_rect){0, 0, 0, 0};
		} else {
			pixman_box32_t *ext = pixman_region32_extents(&buffer_damage);
			struct tp_rect raw = {
				.x = ext->x1,
				.y = ext->y1,
				.w = ext->x2 - ext->x1,
				.h = ext->y2 - ext->y1,
			};
			render_damage = tp_visuals_damage_expand(raw, pad_px, fb_bounds);
			if (render_damage.w <= 0 || render_damage.h <= 0)
				need_draw = false;
		}
		if (pixman_region32_not_empty(&output_damage) && pad_px > 0) {
			pixman_box32_t *oext = pixman_region32_extents(&output_damage);
			struct tp_rect oexp = tp_visuals_damage_expand(
				(struct tp_rect){oext->x1, oext->y1, oext->x2 - oext->x1,
								 oext->y2 - oext->y1},
				pad_px, fb_bounds);
			pixman_region32_reset(&output_damage,
								  &(pixman_box32_t){oexp.x, oexp.y,
													oexp.x + oexp.w,
													oexp.y + oexp.h});
		}
	} else {
		pixman_region32_union_rect(&output_damage, &output_damage, 0, 0,
								   buf->width, buf->height);
	}

	if (need_draw) {
		struct tp_egl_saved saved;
		if (!tp_visuals_egl_enter(s, &saved)) {
			if (have_ring)
				wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
			pixman_region32_fini(&buffer_damage);
			pixman_region32_fini(&output_damage);
			wlr_buffer_unlock(buf);
			wlr_output_state_finish(&state);
			return false;
		}

		if (!tp_visuals_ensure_gl_ready(s, v)) {
			tp_visuals_egl_leave(s, &saved);
			if (have_ring)
				wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
			pixman_region32_fini(&buffer_damage);
			pixman_region32_fini(&output_damage);
			wlr_buffer_unlock(buf);
			wlr_output_state_finish(&state);
			return false;
		}

		if (!tp_visuals_blur_active(&s->settings) && v->blur_levels > 0)
			destroy_blur_fbos(v);

		GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(s->renderer, buf);
		if (!fbo) {
			tp_visuals_egl_leave(s, &saved);
			if (have_ring)
				wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
			pixman_region32_fini(&buffer_damage);
			pixman_region32_fini(&output_damage);
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
			.scale = scale,
			.damage = render_damage,
			.has_damage = have_ring,
			.now_ms = now_ms,
		};

		restore_frame_state(&fc);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		for (int i = 0; i < TP_LAYER_COUNT; i++)
			render_layer(&fc, i);

		glDisable(GL_SCISSOR_TEST);
		glUseProgram(0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glFlush();

		tp_visuals_egl_leave(s, &saved);
	}

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
	if (pixman_region32_not_empty(&output_damage))
		wlr_output_state_set_damage(&state, &output_damage);
	bool ok = wlr_output_commit_state(m->output, &state);
	if (!ok && have_ring)
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);

	pixman_region32_fini(&buffer_damage);
	pixman_region32_fini(&output_damage);
	wlr_buffer_unlock(buf);
	wlr_output_state_finish(&state);

	if (ok) {
		v->frames_visual++;
		if (anim_on_m && tp_visuals_anim_prune_monitor(s, v, m, now_ms))
			wlr_output_schedule_frame(m->output);
	}
	return ok;
}
