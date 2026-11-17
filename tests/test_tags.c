/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "check.h"
#include "tam-pane/tags.h"

int main(void) {
	struct tp_tagview v;

	tp_tagview_init(&v, 0);
	CHECK(tp_tagview_current(&v) == tp_tag_mask(1)); /* empty falls back */

	tp_tagview_init(&v, tp_tag_mask(2));
	CHECK(tp_tagview_current(&v) == tp_tag_mask(2));

	/* show: switches, remembers the previous view, ignores no-ops */
	CHECK(tp_tagview_show(&v, tp_tag_mask(5)));
	CHECK(tp_tagview_current(&v) == tp_tag_mask(5));
	CHECK(!tp_tagview_show(&v, tp_tag_mask(5)));
	CHECK(!tp_tagview_show(&v, 0));
	CHECK(!tp_tagview_show(&v, 1u << 20)); /* outside the tag range */
	tp_tagview_back(&v);
	CHECK(tp_tagview_current(&v) == tp_tag_mask(2));
	tp_tagview_back(&v);
	CHECK(tp_tagview_current(&v) == tp_tag_mask(5));

	/* toggle: adds and removes, never empties the view */
	CHECK(tp_tagview_toggle(&v, tp_tag_mask(7)));
	CHECK(tp_tagview_current(&v) == (tp_tag_mask(5) | tp_tag_mask(7)));
	CHECK(tp_tagview_toggle(&v, tp_tag_mask(5)));
	CHECK(tp_tagview_current(&v) == tp_tag_mask(7));
	CHECK(!tp_tagview_toggle(&v, tp_tag_mask(7)));
	CHECK(tp_tagview_current(&v) == tp_tag_mask(7));

	/* visibility */
	CHECK(tp_tags_visible(tp_tag_mask(7) | tp_tag_mask(1), &v));
	CHECK(!tp_tags_visible(tp_tag_mask(1), &v));
	CHECK(!tp_tags_visible(0, &v));

	/* numbers, masks, counts, slots */
	CHECK(tp_tag_mask(0) == 0);
	CHECK(tp_tag_mask(TP_TAG_COUNT + 1) == 0);
	CHECK(tp_tag_mask(TP_TAG_COUNT) == 1u << (TP_TAG_COUNT - 1));
	CHECK(tp_tags_first(0) == 0);
	CHECK(tp_tags_first(tp_tag_mask(3) | tp_tag_mask(6)) == 3);
	CHECK(tp_tags_count(TP_TAG_ALL) == TP_TAG_COUNT);
	CHECK(tp_tags_count(~0u) == TP_TAG_COUNT);
	CHECK(tp_tags_slot(tp_tag_mask(4)) == 4);
	CHECK(tp_tags_slot(tp_tag_mask(4) | tp_tag_mask(1)) == 0);
	CHECK(tp_tags_slot(0) == 0);

	CHECK_DONE("test_tags");
}
