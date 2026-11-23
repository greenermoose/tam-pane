/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/layout/monocle.h"
#include <stdlib.h>
#include <string.h>

static size_t find_index(const struct tp_monocle *mo, const void *item,
						 bool *found) {
	for (size_t i = 0; i < mo->len; i++) {
		if (mo->items[i] == item) {
			if (found)
				*found = true;
			return i;
		}
	}
	if (found)
		*found = false;
	return 0;
}

void tp_monocle_init(struct tp_monocle *mo) {
	*mo = (struct tp_monocle){0};
}

void tp_monocle_clear(struct tp_monocle *mo) {
	free(mo->items);
	mo->items = NULL;
	mo->len = 0;
	mo->cap = 0;
	mo->active = NULL;
	mo->gen++;
}

bool tp_monocle_insert(struct tp_monocle *mo, void *item) {
	if (!mo || !item)
		return false;
	if (tp_monocle_contains(mo, item))
		return false;
	if (mo->len == mo->cap) {
		size_t next_cap = mo->cap ? mo->cap * 2 : 8;
		void **grown = realloc(mo->items, next_cap * sizeof(*grown));
		if (!grown)
			return false;
		mo->items = grown;
		mo->cap = next_cap;
	}
	mo->items[mo->len++] = item;
	if (!mo->active)
		mo->active = item;
	return true;
}

bool tp_monocle_remove(struct tp_monocle *mo, void *item) {
	if (!mo || !item)
		return false;
	bool found = false;
	size_t idx = find_index(mo, item, &found);
	if (!found)
		return false;
	if (idx + 1 < mo->len)
		memmove(&mo->items[idx], &mo->items[idx + 1],
				(mo->len - idx - 1) * sizeof(*mo->items));
	mo->len--;
	if (mo->active == item)
		mo->active = mo->len > 0 ? mo->items[idx < mo->len ? idx : mo->len - 1]
								 : NULL;
	mo->gen++;
	return true;
}

bool tp_monocle_contains(const struct tp_monocle *mo, const void *item) {
	if (!mo || !item)
		return false;
	bool found = false;
	find_index(mo, item, &found);
	return found;
}

size_t tp_monocle_count(const struct tp_monocle *mo) {
	return mo ? mo->len : 0;
}

size_t tp_monocle_items(const struct tp_monocle *mo, void **out, size_t max) {
	if (!mo || !out || max == 0)
		return 0;
	size_t n = mo->len < max ? mo->len : max;
	if (n > 0)
		memcpy(out, mo->items, n * sizeof(*out));
	return n;
}

bool tp_monocle_swap_dir(struct tp_monocle *mo, void *item, enum tp_dir dir) {
	if (!mo || !item || mo->len < 2)
		return false;
	bool found = false;
	size_t idx = find_index(mo, item, &found);
	if (!found)
		return false;
	size_t other;
	if (dir == TP_DIR_LEFT || dir == TP_DIR_UP) {
		if (idx == 0)
			return false;
		other = idx - 1;
	} else if (dir == TP_DIR_RIGHT || dir == TP_DIR_DOWN) {
		if (idx + 1 >= mo->len)
			return false;
		other = idx + 1;
	} else {
		return false;
	}
	void *tmp = mo->items[idx];
	mo->items[idx] = mo->items[other];
	mo->items[other] = tmp;
	return true;
}

bool tp_monocle_move_dir(struct tp_monocle *mo, void *item, enum tp_dir dir) {
	return tp_monocle_swap_dir(mo, item, dir);
}

void tp_monocle_layout(struct tp_monocle *mo, struct tp_rect area,
					   const void *focused, tp_monocle_place_fn place,
					   void *data) {
	if (!mo || !place || mo->len == 0)
		return;
	if (area.w < 1)
		area.w = 1;
	if (area.h < 1)
		area.h = 1;

	if (focused && tp_monocle_contains(mo, focused))
		mo->active = (void *)focused;
	else if (!mo->active || !tp_monocle_contains(mo, mo->active))
		mo->active = mo->items[0];

	for (size_t i = 0; i < mo->len; i++) {
		place(mo->items[i], area, mo->items[i] == mo->active, data);
	}
}
