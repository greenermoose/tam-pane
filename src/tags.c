/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/tags.h"

void tp_tagview_init(struct tp_tagview *v, tp_tags_t initial) {
	initial &= TP_TAG_ALL;
	if (!initial)
		initial = tp_tag_mask(1);
	v->slot[0] = v->slot[1] = initial;
	v->cur = 0;
}

tp_tags_t tp_tagview_current(const struct tp_tagview *v) {
	return v->slot[v->cur];
}

bool tp_tagview_show(struct tp_tagview *v, tp_tags_t mask) {
	mask &= TP_TAG_ALL;
	if (!mask || mask == tp_tagview_current(v))
		return false;
	v->cur ^= 1u;
	v->slot[v->cur] = mask;
	return true;
}

bool tp_tagview_toggle(struct tp_tagview *v, tp_tags_t mask) {
	tp_tags_t next = (tp_tagview_current(v) ^ mask) & TP_TAG_ALL;
	if (!next || next == tp_tagview_current(v))
		return false;
	v->slot[v->cur] = next;
	return true;
}

void tp_tagview_back(struct tp_tagview *v) {
	v->cur ^= 1u;
}

bool tp_tags_visible(tp_tags_t t, const struct tp_tagview *v) {
	return (t & tp_tagview_current(v)) != 0;
}

tp_tags_t tp_tag_mask(unsigned int number) {
	if (number < 1 || number > TP_TAG_COUNT)
		return 0;
	return 1u << (number - 1);
}

unsigned int tp_tags_first(tp_tags_t t) {
	t &= TP_TAG_ALL;
	return t ? (unsigned int)__builtin_ctz(t) + 1u : 0u;
}

unsigned int tp_tags_count(tp_tags_t t) {
	return (unsigned int)__builtin_popcount(t & TP_TAG_ALL);
}

unsigned int tp_tags_slot(tp_tags_t view) {
	return tp_tags_count(view) == 1 ? tp_tags_first(view) : 0u;
}
