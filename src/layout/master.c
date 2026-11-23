/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/layout/master.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float clamp_mfact(float f) {
	if (isnan(f) || f < TP_MASTER_MFACT_MIN)
		return TP_MASTER_MFACT_MIN;
	if (f > TP_MASTER_MFACT_MAX)
		return TP_MASTER_MFACT_MAX;
	return f;
}

static int clamp_nmaster(int n) {
	if (n < TP_MASTER_COUNT_MIN)
		return TP_MASTER_COUNT_MIN;
	if (n > TP_MASTER_COUNT_MAX)
		return TP_MASTER_COUNT_MAX;
	return n;
}

static size_t find_index(const struct tp_master *m, const void *item,
						 bool *found) {
	for (size_t i = 0; i < m->len; i++) {
		if (m->items[i] == item) {
			if (found)
				*found = true;
			return i;
		}
	}
	if (found)
		*found = false;
	return 0;
}

void tp_master_init(struct tp_master *m) {
	*m = (struct tp_master){
		.mfact = TP_MASTER_MFACT_DEFAULT,
		.nmaster = TP_MASTER_COUNT_DEFAULT,
	};
}

void tp_master_clear(struct tp_master *m) {
	free(m->items);
	m->items = NULL;
	m->len = 0;
	m->cap = 0;
	m->gen++;
}

bool tp_master_insert(struct tp_master *m, void *item) {
	if (!m || !item)
		return false;
	if (tp_master_contains(m, item))
		return false;
	if (m->len == m->cap) {
		size_t next_cap = m->cap ? m->cap * 2 : 8;
		void **grown = realloc(m->items, next_cap * sizeof(*grown));
		if (!grown)
			return false;
		m->items = grown;
		m->cap = next_cap;
	}
	m->items[m->len++] = item;
	return true;
}

bool tp_master_remove(struct tp_master *m, void *item) {
	if (!m || !item)
		return false;
	bool found = false;
	size_t idx = find_index(m, item, &found);
	if (!found)
		return false;
	if (idx + 1 < m->len)
		memmove(&m->items[idx], &m->items[idx + 1],
				(m->len - idx - 1) * sizeof(*m->items));
	m->len--;
	m->gen++;
	return true;
}

bool tp_master_contains(const struct tp_master *m, const void *item) {
	if (!m || !item)
		return false;
	bool found = false;
	find_index(m, item, &found);
	return found;
}

size_t tp_master_count(const struct tp_master *m) {
	return m ? m->len : 0;
}

size_t tp_master_items(const struct tp_master *m, void **out, size_t max) {
	if (!m || !out || max == 0)
		return 0;
	size_t n = m->len < max ? m->len : max;
	if (n > 0)
		memcpy(out, m->items, n * sizeof(*out));
	return n;
}

static bool target_index(const struct tp_master *m, size_t idx, enum tp_dir dir,
						 size_t *out_target) {
	size_t nm = (size_t)clamp_nmaster(m->nmaster);
	switch (dir) {
	case TP_DIR_UP:
		if (idx == 0)
			return false;
		*out_target = idx - 1;
		return true;
	case TP_DIR_DOWN:
		if (idx + 1 >= m->len)
			return false;
		*out_target = idx + 1;
		return true;
	case TP_DIR_LEFT:
		if (idx < nm || m->len <= nm)
			return false;
		*out_target = 0;
		return true;
	case TP_DIR_RIGHT:
		if (idx >= nm || m->len <= nm)
			return false;
		*out_target = nm;
		return true;
	}
	return false;
}

bool tp_master_swap_dir(struct tp_master *m, void *item, enum tp_dir dir) {
	if (!m || !item || m->len < 2)
		return false;
	bool found = false;
	size_t idx = find_index(m, item, &found);
	if (!found)
		return false;
	size_t other = 0;
	if (!target_index(m, idx, dir, &other) || other == idx)
		return false;
	void *tmp = m->items[idx];
	m->items[idx] = m->items[other];
	m->items[other] = tmp;
	return true;
}

bool tp_master_move_dir(struct tp_master *m, void *item, enum tp_dir dir) {
	if (!m || !item || m->len < 2)
		return false;
	bool found = false;
	size_t idx = find_index(m, item, &found);
	if (!found)
		return false;
	size_t dst = 0;
	if (!target_index(m, idx, dir, &dst) || dst == idx)
		return false;
	void *val = m->items[idx];
	if (dst < idx) {
		memmove(&m->items[dst + 1], &m->items[dst],
				(idx - dst) * sizeof(*m->items));
	} else {
		memmove(&m->items[idx], &m->items[idx + 1],
				(dst - idx) * sizeof(*m->items));
	}
	m->items[dst] = val;
	return true;
}

bool tp_master_resize(struct tp_master *m, const void *item, enum tp_dir dir,
					  int32_t delta) {
	if (!m || !item || !delta)
		return false;
	if (dir != TP_DIR_LEFT && dir != TP_DIR_RIGHT)
		return false;
	size_t nm = (size_t)clamp_nmaster(m->nmaster);
	if (m->len <= nm || !tp_master_contains(m, item))
		return false;
	if (m->last_area.w <= 0)
		return false;

	float sign = (dir == TP_DIR_RIGHT) ? 1.0f : -1.0f;
	float next =
		clamp_mfact(m->mfact + sign * ((float)delta / (float)m->last_area.w));
	if (fabsf(next - m->mfact) < 1e-6f)
		return false;
	m->mfact = next;
	return true;
}

bool tp_master_adjust_nmaster(struct tp_master *m, int delta) {
	if (!m || !delta)
		return false;
	int next = clamp_nmaster(m->nmaster + delta);
	if (next == m->nmaster)
		return false;
	m->nmaster = next;
	return true;
}

bool tp_master_drag_begin(struct tp_master *m, const void *item,
						  struct tp_master_drag *drag) {
	if (!m || !item || !drag)
		return false;
	size_t nm = (size_t)clamp_nmaster(m->nmaster);
	if (m->len <= nm || m->last_area.w <= 0 || !tp_master_contains(m, item))
		return false;
	*drag = (struct tp_master_drag){
		.master = m,
		.gen = m->gen,
		.start_mfact = m->mfact,
	};
	return true;
}

bool tp_master_drag_valid(const struct tp_master_drag *drag,
						  const struct tp_master *m) {
	return drag && m && drag->master == m && drag->gen == m->gen;
}

bool tp_master_drag_update(struct tp_master_drag *drag, double dx) {
	if (!drag || !drag->master || drag->master->last_area.w <= 0)
		return false;
	float next = clamp_mfact(
		drag->start_mfact + (float)(dx / (double)drag->master->last_area.w));
	if (fabsf(next - drag->master->mfact) < 1e-6f)
		return false;
	drag->master->mfact = next;
	return true;
}

static void place_stack(void **items, size_t count, struct tp_rect pane,
						int32_t gap, tp_master_place_fn place, void *data) {
	if (count == 0)
		return;
	int32_t total_gap = count > 1 ? gap * (int32_t)(count - 1) : 0;
	int32_t avail_h = pane.h - total_gap;
	if (avail_h < (int32_t)count)
		avail_h = (int32_t)count;

	int32_t y = pane.y;
	for (size_t i = 0; i < count; i++) {
		int32_t h;
		if (i + 1 == count) {
			h = (pane.y + pane.h) - y;
			if (h < 1)
				h = 1;
		} else {
			h = avail_h / (int32_t)count;
			if (h < 1)
				h = 1;
		}
		place(items[i], (struct tp_rect){pane.x, y, pane.w, h}, true, data);
		y += h + gap;
	}
}

void tp_master_layout(struct tp_master *m, struct tp_rect area, int32_t gap,
					  tp_master_place_fn place, void *data) {
	if (!m || !place || m->len == 0)
		return;
	if (area.w < 1)
		area.w = 1;
	if (area.h < 1)
		area.h = 1;
	if (gap < 0)
		gap = 0;
	m->last_area = area;

	size_t nm = (size_t)clamp_nmaster(m->nmaster);
	if (m->len <= nm) {
		place_stack(m->items, m->len, area, gap, place, data);
		return;
	}

	int32_t avail_w = area.w - gap;
	if (avail_w < 2)
		avail_w = 2;
	int32_t mw = (int32_t)lroundf((float)avail_w * clamp_mfact(m->mfact));
	if (mw < 1)
		mw = 1;
	if (mw > avail_w - 1)
		mw = avail_w - 1;
	int32_t sw = avail_w - mw;
	if (sw < 1)
		sw = 1;

	struct tp_rect master_pane = {area.x, area.y, mw, area.h};
	struct tp_rect stack_pane = {area.x + mw + gap, area.y, sw, area.h};
	place_stack(m->items, nm, master_pane, gap, place, data);
	place_stack(m->items + nm, m->len - nm, stack_pane, gap, place, data);
}
