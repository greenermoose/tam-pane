/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/layout/scroller.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float clamp_width(float w) {
	if (isnan(w) || w < TP_SCROLLER_WIDTH_MIN)
		return TP_SCROLLER_WIDTH_MIN;
	if (w > TP_SCROLLER_WIDTH_MAX)
		return TP_SCROLLER_WIDTH_MAX;
	return w;
}

static size_t find_index(const struct tp_scroller *sc, const void *item,
						 bool *found) {
	for (size_t i = 0; i < sc->len; i++) {
		if (sc->cols[i].item == item) {
			if (found)
				*found = true;
			return i;
		}
	}
	if (found)
		*found = false;
	return 0;
}

void tp_scroller_init(struct tp_scroller *sc) {
	*sc = (struct tp_scroller){
		.default_width = TP_SCROLLER_WIDTH_DEFAULT,
	};
}

void tp_scroller_clear(struct tp_scroller *sc) {
	free(sc->cols);
	sc->cols = NULL;
	sc->len = 0;
	sc->cap = 0;
	sc->scroll_x = 0;
	sc->gen++;
}

bool tp_scroller_insert(struct tp_scroller *sc, void *item, void *near) {
	if (!sc || !item)
		return false;
	if (tp_scroller_contains(sc, item))
		return false;
	if (sc->len == sc->cap) {
		size_t next_cap = sc->cap ? sc->cap * 2 : 8;
		struct tp_scroller_col *grown =
			realloc(sc->cols, next_cap * sizeof(*grown));
		if (!grown)
			return false;
		sc->cols = grown;
		sc->cap = next_cap;
	}

	size_t pos = sc->len;
	if (near) {
		bool found = false;
		size_t idx = find_index(sc, near, &found);
		if (found)
			pos = idx + 1;
	}
	if (pos < sc->len) {
		memmove(&sc->cols[pos + 1], &sc->cols[pos],
				(sc->len - pos) * sizeof(*sc->cols));
		sc->gen++;
	}
	sc->cols[pos] = (struct tp_scroller_col){
		.item = item,
		.width_frac = clamp_width(sc->default_width),
	};
	sc->len++;
	return true;
}

bool tp_scroller_remove(struct tp_scroller *sc, void *item) {
	if (!sc || !item)
		return false;
	bool found = false;
	size_t idx = find_index(sc, item, &found);
	if (!found)
		return false;
	if (idx + 1 < sc->len)
		memmove(&sc->cols[idx], &sc->cols[idx + 1],
				(sc->len - idx - 1) * sizeof(*sc->cols));
	sc->len--;
	sc->gen++;
	return true;
}

bool tp_scroller_contains(const struct tp_scroller *sc, const void *item) {
	if (!sc || !item)
		return false;
	bool found = false;
	find_index(sc, item, &found);
	return found;
}

size_t tp_scroller_count(const struct tp_scroller *sc) {
	return sc ? sc->len : 0;
}

size_t tp_scroller_items(const struct tp_scroller *sc, void **out, size_t max) {
	if (!sc || !out || max == 0)
		return 0;
	size_t n = sc->len < max ? sc->len : max;
	for (size_t i = 0; i < n; i++)
		out[i] = sc->cols[i].item;
	return n;
}

static bool adjacent_index(const struct tp_scroller *sc, size_t idx,
						   enum tp_dir dir, size_t *out) {
	if (dir == TP_DIR_LEFT || dir == TP_DIR_UP) {
		if (idx == 0)
			return false;
		*out = idx - 1;
		return true;
	}
	if (dir == TP_DIR_RIGHT || dir == TP_DIR_DOWN) {
		if (idx + 1 >= sc->len)
			return false;
		*out = idx + 1;
		return true;
	}
	return false;
}

bool tp_scroller_swap_dir(struct tp_scroller *sc, void *item, enum tp_dir dir) {
	if (!sc || !item || sc->len < 2)
		return false;
	bool found = false;
	size_t idx = find_index(sc, item, &found);
	if (!found)
		return false;
	size_t other = 0;
	if (!adjacent_index(sc, idx, dir, &other))
		return false;
	struct tp_scroller_col tmp = sc->cols[idx];
	sc->cols[idx] = sc->cols[other];
	sc->cols[other] = tmp;
	sc->gen++;
	return true;
}

bool tp_scroller_move_dir(struct tp_scroller *sc, void *item, enum tp_dir dir) {
	return tp_scroller_swap_dir(sc, item, dir);
}

bool tp_scroller_resize(struct tp_scroller *sc, const void *item,
						enum tp_dir dir, int32_t delta) {
	if (!sc || !item || !delta || sc->last_area.w <= 0)
		return false;
	if (dir != TP_DIR_LEFT && dir != TP_DIR_RIGHT)
		return false;
	bool found = false;
	size_t idx = find_index(sc, item, &found);
	if (!found)
		return false;

	float sign = (dir == TP_DIR_RIGHT) ? 1.0f : -1.0f;
	float cur = sc->cols[idx].width_frac;
	float next = clamp_width(cur + sign * ((float)delta / (float)sc->last_area.w));
	if (fabsf(next - cur) < 1e-6f)
		return false;
	sc->cols[idx].width_frac = next;
	return true;
}

bool tp_scroller_drag_begin(struct tp_scroller *sc, const void *item,
							struct tp_scroller_drag *drag) {
	if (!sc || !item || !drag || sc->last_area.w <= 0)
		return false;
	bool found = false;
	size_t idx = find_index(sc, item, &found);
	if (!found)
		return false;
	*drag = (struct tp_scroller_drag){
		.scroller = sc,
		.gen = sc->gen,
		.index = idx,
		.start_width = sc->cols[idx].width_frac,
	};
	return true;
}

bool tp_scroller_drag_valid(const struct tp_scroller_drag *drag,
							const struct tp_scroller *sc) {
	return drag && sc && drag->scroller == sc && drag->gen == sc->gen &&
		   drag->index < sc->len;
}

bool tp_scroller_drag_update(struct tp_scroller_drag *drag, double dx) {
	if (!drag || !drag->scroller || drag->scroller->last_area.w <= 0 ||
		drag->index >= drag->scroller->len)
		return false;
	float next = clamp_width(
		drag->start_width + (float)(dx / (double)drag->scroller->last_area.w));
	if (fabsf(next - drag->scroller->cols[drag->index].width_frac) < 1e-6f)
		return false;
	drag->scroller->cols[drag->index].width_frac = next;
	return true;
}

static int32_t col_pixel_width(float frac, int32_t area_w) {
	int32_t w = (int32_t)lroundf(clamp_width(frac) * (float)area_w);
	if (w < 1)
		w = 1;
	if (w > area_w)
		w = area_w;
	return w;
}

void tp_scroller_layout(struct tp_scroller *sc, struct tp_rect area,
						int32_t gap, const void *focused,
						tp_scroller_place_fn place, void *data) {
	if (!sc || !place || sc->len == 0)
		return;
	if (area.w < 1)
		area.w = 1;
	if (area.h < 1)
		area.h = 1;
	if (gap < 0)
		gap = 0;
	sc->last_area = area;

	/* First pass: compute strip total width and focused column span. */
	int32_t cursor_x = 0;
	int32_t foc_x = 0, foc_w = 0;
	bool have_foc = false;
	for (size_t i = 0; i < sc->len; i++) {
		int32_t w = col_pixel_width(sc->cols[i].width_frac, area.w);
		if (focused && sc->cols[i].item == focused) {
			foc_x = cursor_x;
			foc_w = w;
			have_foc = true;
		}
		cursor_x += w;
		if (i + 1 < sc->len)
			cursor_x += gap;
	}
	int32_t total_w = cursor_x;

	if (have_foc) {
		if (foc_x < sc->scroll_x)
			sc->scroll_x = foc_x;
		else if (foc_x + foc_w > sc->scroll_x + area.w)
			sc->scroll_x = foc_x + foc_w - area.w;
	}
	int32_t max_scroll = total_w > area.w ? total_w - area.w : 0;
	if (sc->scroll_x < 0)
		sc->scroll_x = 0;
	if (sc->scroll_x > max_scroll)
		sc->scroll_x = max_scroll;

	/* Second pass: emit column rectangles with viewport visibility. */
	cursor_x = 0;
	for (size_t i = 0; i < sc->len; i++) {
		int32_t w = col_pixel_width(sc->cols[i].width_frac, area.w);
		struct tp_rect box = {
			.x = area.x + cursor_x - sc->scroll_x,
			.y = area.y,
			.w = w,
			.h = area.h,
		};
		bool visible =
			(box.x + box.w > area.x) && (box.x < area.x + area.w);
		place(sc->cols[i].item, box, visible, data);
		cursor_x += w + gap;
	}
}
