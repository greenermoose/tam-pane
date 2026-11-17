/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_GEOM_H
#define TAM_PANE_GEOM_H

#include <stdbool.h>
#include <stdint.h>

/* An area in layout coordinates. Width and height are never negative. */
struct tp_rect {
	int32_t x, y, w, h;
};

static inline bool tp_rect_contains(struct tp_rect r, double x, double y) {
	return x >= r.x && y >= r.y && x < (double)r.x + r.w &&
		   y < (double)r.y + r.h;
}

static inline struct tp_rect tp_rect_inset(struct tp_rect r, int32_t by) {
	struct tp_rect o = {r.x + by, r.y + by, r.w - 2 * by, r.h - 2 * by};
	if (o.w < 1)
		o.w = 1;
	if (o.h < 1)
		o.h = 1;
	return o;
}

#endif
