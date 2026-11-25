/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/chord_overlay.h"
#include "font8x16.h"
#include "tam-pane/chord.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

struct tp_chord_overlay {
	struct wlr_scene_buffer *scene_buf;
};

struct tp_pixel_buf {
	struct wlr_buffer base;
	uint32_t *pixels;
	size_t stride;
};

static void pixel_buf_destroy(struct wlr_buffer *wlr_buf) {
	struct tp_pixel_buf *pb = wl_container_of(wlr_buf, pb, base);
	free(pb->pixels);
	free(pb);
}

static bool pixel_buf_begin_data_ptr_access(struct wlr_buffer *wlr_buf,
											uint32_t flags, void **data,
											uint32_t *format, size_t *stride) {
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	struct tp_pixel_buf *pb = wl_container_of(wlr_buf, pb, base);
	*data = pb->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = pb->stride;
	return true;
}

static void pixel_buf_end_data_ptr_access(struct wlr_buffer *wlr_buf) {
	(void)wlr_buf;
}

static const struct wlr_buffer_impl pixel_buf_impl = {
	.destroy = pixel_buf_destroy,
	.begin_data_ptr_access = pixel_buf_begin_data_ptr_access,
	.end_data_ptr_access = pixel_buf_end_data_ptr_access,
};

static struct tp_pixel_buf *pixel_buf_create(int w, int h) {
	if (w <= 0 || h <= 0)
		return NULL;
	struct tp_pixel_buf *pb = calloc(1, sizeof(*pb));
	if (!pb)
		return NULL;
	pb->stride = (size_t)w * 4;
	pb->pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
	if (!pb->pixels) {
		free(pb);
		return NULL;
	}
	wlr_buffer_init(&pb->base, &pixel_buf_impl, w, h);
	return pb;
}

static void fill_rect(uint32_t *px, int bw, int bh, int x, int y, int w, int h,
					  uint32_t argb) {
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + w > bw ? bw : x + w;
	int y1 = y + h > bh ? bh : y + h;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *row = px + (size_t)yy * (size_t)bw;
		for (int xx = x0; xx < x1; xx++)
			row[xx] = argb;
	}
}

static void draw_glyph(uint32_t *px, int bw, int bh, int x, int y, char ch,
					   uint32_t argb) {
	unsigned char c = (unsigned char)ch;
	if (c < 0x20 || c > 0x7E)
		c = '?';
	const uint8_t *rows = tp_font8x8[c - 0x20];
	for (int gy = 0; gy < 8; gy++) {
		uint8_t bits = rows[gy];
		for (int dy = 0; dy < 2; dy++) {
			int py = y + gy * 2 + dy;
			if (py < 0 || py >= bh)
				continue;
			uint32_t *row = px + (size_t)py * (size_t)bw;
			for (int gx = 0; gx < 8; gx++) {
				if (!(bits & (0x80u >> gx)))
					continue;
				int px_x = x + gx;
				if (px_x >= 0 && px_x < bw)
					row[px_x] = argb;
			}
		}
	}
}

static void draw_text(uint32_t *px, int bw, int bh, int x, int y, int max_chars,
					  const char *s, uint32_t argb) {
	if (!s)
		return;
	int col = 0;
	for (; *s && col < max_chars; s++, col++)
		draw_glyph(px, bw, bh, x + col * 8, y, *s, argb);
}

bool tp_chord_overlay_init(struct tp_server *s) {
	if (!s || !s->layer[TP_LAYER_OVERLAY])
		return false;
	struct tp_chord_overlay *ov = calloc(1, sizeof(*ov));
	if (!ov)
		return false;
	ov->scene_buf = wlr_scene_buffer_create(s->layer[TP_LAYER_OVERLAY], NULL);
	if (!ov->scene_buf) {
		free(ov);
		return false;
	}
	wlr_scene_node_set_enabled(&ov->scene_buf->node, false);
	s->chord_overlay = ov;
	return true;
}

void tp_chord_overlay_finish(struct tp_server *s) {
	if (!s || !s->chord_overlay)
		return;
	if (s->chord_overlay->scene_buf)
		wlr_scene_node_destroy(&s->chord_overlay->scene_buf->node);
	free(s->chord_overlay);
	s->chord_overlay = NULL;
}

void tp_chord_overlay_update(struct tp_server *s) {
	if (!s || !s->chord_overlay || !s->chord_overlay->scene_buf)
		return;
	struct tp_chord_overlay *ov = s->chord_overlay;

	if (s->locked || !s->settings.chord_overlay_builtin ||
		!tp_chord_is_composing(&s->chord) || !s->selmon) {
		wlr_scene_node_set_enabled(&ov->scene_buf->node, false);
		return;
	}

	const struct tp_chord_state *st = &s->chord;
	struct tp_rect area = s->selmon->usable;
	if (area.w <= 0 || area.h <= 0)
		area = s->selmon->area;

	int panel_w = 560;
	int panel_h = 340;
	if (area.w > 40 && panel_w > area.w - 32)
		panel_w = area.w - 32;
	if (area.h > 40 && panel_h > area.h - 32)
		panel_h = area.h - 32;
	if (panel_w < 240)
		panel_w = 240;
	if (panel_h < 140)
		panel_h = 140;

	struct tp_pixel_buf *pb = pixel_buf_create(panel_w, panel_h);
	if (!pb)
		return;

	/* Colors (premultiplied ARGB8888) */
	const uint32_t bg = 0xF0181A20u;
	const uint32_t hdr_bg = 0xFF21252Bu;
	const uint32_t sel_bg = 0xFF2C3646u;
	const uint32_t border =
		(st->mode == TP_CHORD_PLURIPOTENT) ? 0xFFE5C07Bu : 0xFF56B6C2u;
	const uint32_t fg = 0xFFE6EDF3u;
	const uint32_t fg_dim = 0xFF9DA5B4u;
	const uint32_t fg_accent = border;

	fill_rect(pb->pixels, panel_w, panel_h, 0, 0, panel_w, panel_h, bg);
	/* 2px border */
	fill_rect(pb->pixels, panel_w, panel_h, 0, 0, panel_w, 2, border);
	fill_rect(pb->pixels, panel_w, panel_h, 0, panel_h - 2, panel_w, 2, border);
	fill_rect(pb->pixels, panel_w, panel_h, 0, 0, 2, panel_h, border);
	fill_rect(pb->pixels, panel_w, panel_h, panel_w - 2, 0, 2, panel_h, border);

	/* Header bar */
	fill_rect(pb->pixels, panel_w, panel_h, 2, 2, panel_w - 4, 34, hdr_bg);
	int max_cols = (panel_w - 24) / 8;

	char header[128];
	if (st->mods != 0 || st->sym != XKB_KEY_NoSymbol) {
		char cbuf[64];
		tp_chord_format(st->mods, st->sym, cbuf, sizeof(cbuf));
		snprintf(header, sizeof(header), "CHORD: %s%s", cbuf,
				 st->sym == XKB_KEY_NoSymbol ? "_" : "");
	} else if (st->filter_len > 0) {
		snprintf(header, sizeof(header), "COMMAND FILTER: %s_", st->filter);
	} else if (st->mode == TP_CHORD_PLURIPOTENT) {
		snprintf(header, sizeof(header), "CHORD COMPOSER / CAPS LOCK");
	} else {
		snprintf(header, sizeof(header), "CHORD COMPOSER");
	}
	draw_text(pb->pixels, panel_w, panel_h, 12, 11, max_cols, header,
			  fg_accent);

	/* Command rows */
	int list_top = 44;
	int row_h = 20;
	int footer_h = 30;
	int avail_rows = (panel_h - list_top - footer_h) / row_h;
	if (avail_rows > TP_CHORD_VISIBLE_ROWS)
		avail_rows = TP_CHORD_VISIBLE_ROWS;
	if (avail_rows < 1)
		avail_rows = 1;

	if (st->filtered_len == 0) {
		const char *msg =
			(st->mods != 0 || st->sym != XKB_KEY_NoSymbol)
				? "  (no compositor binding -- Chord key will send to app)"
				: "  (no matching commands)";
		draw_text(pb->pixels, panel_w, panel_h, 12, list_top + 4, max_cols, msg,
				  fg_dim);
	} else {
		for (int r = 0; r < avail_rows; r++) {
			size_t fi = st->scroll + (size_t)r;
			if (fi >= st->filtered_len)
				break;
			const struct tp_chord_item *it = &st->items[st->filtered[fi]];
			int ry = list_top + r * row_h;
			bool is_sel = (fi == st->selected);
			if (is_sel) {
				fill_rect(pb->pixels, panel_w, panel_h, 6, ry, panel_w - 12,
						  row_h, sel_bg);
			}
			char line[160];
			snprintf(line, sizeof(line), "%c %-20s %s", is_sel ? '>' : ' ',
					 it->chord_str, it->desc);
			draw_text(pb->pixels, panel_w, panel_h, 12, ry + 2, max_cols, line,
					  is_sel ? fg : fg_dim);
		}
	}

	/* Footer hint bar */
	int fy = panel_h - footer_h;
	fill_rect(pb->pixels, panel_w, panel_h, 2, fy, panel_w - 4, footer_h - 2,
			  hdr_bg);
	const char *hint =
		(st->mode == TP_CHORD_PLURIPOTENT)
			? "Mod: chord | A-Z: CAPS | Up/Dn+Enter: run | Esc: cancel"
			: "ChordKey: send | Up/Dn+Enter: run | Esc: cancel";
	draw_text(pb->pixels, panel_w, panel_h, 12, fy + 6, max_cols, hint, fg_dim);

	wlr_scene_buffer_set_buffer(ov->scene_buf, &pb->base);
	wlr_buffer_drop(&pb->base);

	int px = area.x + (area.w - panel_w) / 2;
	int py = area.y + (area.h - panel_h) / 2;
	wlr_scene_node_set_position(&ov->scene_buf->node, px, py);
	wlr_scene_node_set_enabled(&ov->scene_buf->node, true);
}
