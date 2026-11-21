/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure window-rule result types for tam-pane (no wlroots, no Wayland).
 */
#ifndef TAM_PANE_RULES_H
#define TAM_PANE_RULES_H

#include "tam-pane/tags.h"
#include <stdbool.h>
#include <stdint.h>

enum {
	TP_RULE_PATTERN_MAX = 256,
	TP_RULE_MONITOR_MAX = 128,
	TP_RULE_MIN_DIM = 64,
	TP_RULE_MAX_DIM = 16384,
	TP_RULE_MIN_COORD = -16384,
	TP_RULE_MAX_COORD = 16384,
};

enum tp_rule_pos_mode {
	TP_RULE_POS_NONE = 0,
	TP_RULE_POS_COORDS,
	TP_RULE_POS_CENTER,
};

struct tp_rule_result {
	bool matched;
	bool has_tags;
	tp_tags_t tags;
	bool has_floating;
	bool floating;
	bool has_size;
	int32_t width;
	int32_t height;
	enum tp_rule_pos_mode pos_mode;
	int32_t x;
	int32_t y;
	bool has_monitor;
	char monitor[TP_RULE_MONITOR_MAX];
};

#endif
