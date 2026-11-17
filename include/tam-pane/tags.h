/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tags: a window carries a bitmask of tags; a monitor shows every window
 * whose mask meets the monitor's current tag set. The two-slot tag set
 * (current and previous) comes from dwm by way of dwl and mango; see
 * UPSTREAM.md. Everything here is plain data with no compositor state, so
 * it is tested without a display.
 */
#ifndef TAM_PANE_TAGS_H
#define TAM_PANE_TAGS_H

#include <stdbool.h>
#include <stdint.h>

#define TP_TAG_COUNT 9u
#define TP_TAG_ALL ((1u << TP_TAG_COUNT) - 1u)

typedef uint32_t tp_tags_t;

/* The tag set a monitor shows, with the previous one kept for "go back". */
struct tp_tagview {
	tp_tags_t slot[2];
	unsigned int cur; /* index of the current slot: 0 or 1 */
};

void tp_tagview_init(struct tp_tagview *v, tp_tags_t initial);
tp_tags_t tp_tagview_current(const struct tp_tagview *v);

/* Show exactly `mask` (bits outside TP_TAG_ALL are dropped). Showing the
 * set already shown, or an empty set, changes nothing. Returns true when
 * the view changed. */
bool tp_tagview_show(struct tp_tagview *v, tp_tags_t mask);

/* Add or remove the tags in `mask` from the current view. The view may
 * never become empty; such a request changes nothing. */
bool tp_tagview_toggle(struct tp_tagview *v, tp_tags_t mask);

/* Swap back to the previous view. */
void tp_tagview_back(struct tp_tagview *v);

/* True when a window with tags `t` is visible in view `v`. */
bool tp_tags_visible(tp_tags_t t, const struct tp_tagview *v);

/* Tag number (1..TP_TAG_COUNT) to mask; 0 for out-of-range numbers. */
tp_tags_t tp_tag_mask(unsigned int number);

/* Lowest tag number set in `t`, or 0 when `t` is empty. */
unsigned int tp_tags_first(tp_tags_t t);

/* Number of tags set in `t`. */
unsigned int tp_tags_count(tp_tags_t t);

/*
 * Each monitor keeps one layout tree per single tag plus one shared tree
 * for views that show several tags at once. Slot 0 is the shared tree;
 * slot N is tag N.
 */
#define TP_TAG_SLOTS (TP_TAG_COUNT + 1u)
unsigned int tp_tags_slot(tp_tags_t view);

#endif
