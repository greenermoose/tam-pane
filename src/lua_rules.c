/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Lua window rule registry and pattern matcher (tp.rule, tp.remove_rule,
 * tp.clear_rules).
 *
 * Pure module (no wlroots, no Wayland). Rules register during config load,
 * validate Lua patterns immediately, stage and roll back atomically alongside
 * tp.config / tp.bind / tp.on, and evaluate via Lua's built-in string.find
 * under the runtime watchdog (no pcre2 dependency).
 */
#include "lua_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tam.h>

/* ---- Rule list helpers ---- */

static void rule_list_clear(struct tp_lua_rule_list *list) {
	free(list->items);
	list->items = NULL;
	list->len = 0;
	list->cap = 0;
}

static bool rule_list_append(struct tp_lua_rule_list *list,
							 const struct tp_lua_rule *entry) {
	if (list->len == list->cap) {
		size_t next_cap = list->cap ? list->cap * 2 : 16;
		struct tp_lua_rule *grown =
			realloc(list->items, next_cap * sizeof(*grown));
		if (!grown)
			return false;
		list->items = grown;
		list->cap = next_cap;
	}
	list->items[list->len++] = *entry;
	return true;
}

static bool rule_list_remove_id(struct tp_lua_rule_list *list, uint64_t id) {
	for (size_t i = 0; i < list->len; i++) {
		if (list->items[i].id == id) {
			if (i + 1 < list->len)
				memmove(&list->items[i], &list->items[i + 1],
						(list->len - i - 1) * sizeof(*list->items));
			list->len--;
			return true;
		}
	}
	return false;
}

/* ---- Pattern helpers ---- */

static void validate_lua_pattern(struct tp_lua *lua, lua_State *L,
								 const char *pat, const char *field) {
	if (lua->string_find_ref == LUA_NOREF)
		luaL_error(L, "tp.rule.match.%s: string.find unavailable", field);

	lua_rawgeti(L, LUA_REGISTRYINDEX, lua->string_find_ref);
	lua_pushliteral(L, "");
	lua_pushstring(L, pat);
	int rc = lua_pcall(L, 2, 0, 0);
	if (rc != LUA_OK) {
		char msg[256];
		const char *err = lua_tostring(L, -1);
		snprintf(msg, sizeof(msg), "%s", err ? err : "malformed pattern");
		lua_pop(L, 1);
		luaL_error(L, "tp.rule.match.%s: invalid Lua pattern: %s", field, msg);
	}
}

static bool match_lua_pattern(struct tp_lua *lua, const char *subject,
							  const char *pat) {
	if (lua->string_find_ref == LUA_NOREF)
		return false;

	lua_State *L = lua->L;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, lua->string_find_ref);
	lua_pushstring(L, subject ? subject : "");
	lua_pushstring(L, pat);
	int rc = tp_lua_guarded_pcall(lua, 2, 1, TP_LUA_CALLBACK_MAX_INSTRUCTIONS,
								  TP_LUA_CALLBACK_TIMEOUT_MS, "rule.match");
	if (rc != LUA_OK) {
		const char *err = lua_tostring(L, -1);
		tam_log_error("lua rule pattern '%s' failed: %s", pat,
					  err ? err : "unknown error");
		lua_settop(L, top);
		return false;
	}
	bool matched = !lua_isnil(L, -1);
	lua_settop(L, top);
	return matched;
}

/* ---- Parsing helpers ---- */

static void copy_nonempty_str(lua_State *L, int idx, const char *ctx,
							  char *dst, size_t dst_size) {
	if (lua_type(L, idx) != LUA_TSTRING)
		luaL_error(L, "%s: expected string", ctx);
	size_t len = 0;
	const char *s = lua_tolstring(L, idx, &len);
	if (len == 0)
		luaL_error(L, "%s: must not be empty", ctx);
	if (len >= dst_size)
		luaL_error(L, "%s: string too long (max %zu bytes)", ctx, dst_size - 1);
	memcpy(dst, s, len + 1);
}

static int32_t check_rule_int(lua_State *L, int idx, const char *ctx,
							  int32_t lo, int32_t hi) {
	if (lua_type(L, idx) != LUA_TNUMBER || !lua_isinteger(L, idx))
		luaL_error(L, "%s: expected integer", ctx);
	lua_Integer v = lua_tointeger(L, idx);
	if (v < lo || v > hi)
		luaL_error(L, "%s: %lld out of range [%d, %d]", ctx, (long long)v, lo,
				   hi);
	return (int32_t)v;
}

static void parse_match_table(struct tp_lua *lua, lua_State *L, int idx,
							  struct tp_lua_rule *rule) {
	if (lua_type(L, idx) != LUA_TTABLE)
		luaL_error(L, "tp.rule.match: expected table");
	int abs_idx = lua_absindex(L, idx);

	lua_pushnil(L);
	while (lua_next(L, abs_idx) != 0) {
		if (lua_type(L, -2) != LUA_TSTRING)
			luaL_error(L, "tp.rule.match: keys must be strings");
		const char *key = lua_tostring(L, -2);
		if (strcmp(key, "app_id") == 0) {
			copy_nonempty_str(L, -1, "tp.rule.match.app_id", rule->app_id,
							  sizeof(rule->app_id));
			validate_lua_pattern(lua, L, rule->app_id, "app_id");
			rule->has_app_id = true;
		} else if (strcmp(key, "title") == 0) {
			copy_nonempty_str(L, -1, "tp.rule.match.title", rule->title,
							  sizeof(rule->title));
			validate_lua_pattern(lua, L, rule->title, "title");
			rule->has_title = true;
		} else {
			luaL_error(L, "tp.rule.match: unknown key '%s'", key);
		}
		lua_pop(L, 1);
	}

	if (!rule->has_app_id && !rule->has_title)
		luaL_error(L, "tp.rule.match: must specify app_id and/or title");
}

static tp_tags_t parse_tags_table(lua_State *L, int idx) {
	if (lua_type(L, idx) != LUA_TTABLE)
		luaL_error(L, "tp.rule.tags: expected table of tag numbers 1..%d "
					  "(use 'tag = n' for a single tag)",
				   TP_TAG_COUNT);
	int abs_idx = lua_absindex(L, idx);
	tp_tags_t mask = 0;
	size_t count = 0;

	lua_pushnil(L);
	while (lua_next(L, abs_idx) != 0) {
		if (lua_type(L, -2) != LUA_TNUMBER || !lua_isinteger(L, -2) ||
			lua_tointeger(L, -2) < 1)
			luaL_error(L, "tp.rule.tags: expected array of tag numbers 1..%d",
					   TP_TAG_COUNT);
		int32_t t = check_rule_int(L, -1, "tp.rule.tags", 1, TP_TAG_COUNT);
		mask |= tp_tag_mask((unsigned int)t);
		count++;
		lua_pop(L, 1);
	}

	if (count == 0)
		luaL_error(L, "tp.rule.tags: must not be empty");
	return mask;
}

/* Parse a 2-element integer pair: either { a, b } or { k1 = a, k2 = b }. */
static void parse_int_pair(lua_State *L, int idx, const char *ctx,
						   const char *k1, const char *k2, int32_t lo,
						   int32_t hi, int32_t *out1, int32_t *out2) {
	if (lua_type(L, idx) != LUA_TTABLE)
		luaL_error(L, "%s: expected table {%s, %s}", ctx, k1, k2);
	int abs_idx = lua_absindex(L, idx);

	bool has1 = false, has2 = false;
	bool saw_index = false, saw_named = false;
	int32_t v1 = 0, v2 = 0;

	lua_pushnil(L);
	while (lua_next(L, abs_idx) != 0) {
		int kt = lua_type(L, -2);
		if (kt == LUA_TNUMBER && lua_isinteger(L, -2)) {
			lua_Integer k = lua_tointeger(L, -2);
			saw_index = true;
			if (k == 1) {
				v1 = check_rule_int(L, -1, ctx, lo, hi);
				has1 = true;
			} else if (k == 2) {
				v2 = check_rule_int(L, -1, ctx, lo, hi);
				has2 = true;
			} else {
				luaL_error(L, "%s: unexpected index %lld", ctx, (long long)k);
			}
		} else if (kt == LUA_TSTRING) {
			const char *k = lua_tostring(L, -2);
			saw_named = true;
			if (strcmp(k, k1) == 0) {
				v1 = check_rule_int(L, -1, ctx, lo, hi);
				has1 = true;
			} else if (strcmp(k, k2) == 0) {
				v2 = check_rule_int(L, -1, ctx, lo, hi);
				has2 = true;
			} else {
				luaL_error(L, "%s: unknown key '%s'", ctx, k);
			}
		} else {
			luaL_error(L, "%s: invalid key type", ctx);
		}
		lua_pop(L, 1);
	}

	if (saw_index && saw_named)
		luaL_error(L, "%s: do not mix array indices and named keys", ctx);
	if (!has1 || !has2)
		luaL_error(L, "%s: requires both %s and %s", ctx, k1, k2);

	*out1 = v1;
	*out2 = v2;
}

/* ---- tp.rule / tp.remove_rule / tp.clear_rules ---- */

static int l_tp_rule(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_rules)
		return luaL_error(L, "tp.rule can only be called while loading config");

	luaL_checktype(L, 1, LUA_TTABLE);

	struct tp_lua_rule entry = {0};
	bool has_match = false;
	bool has_tag_single = false;
	bool has_tags_table = false;

	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		if (lua_type(L, -2) != LUA_TSTRING)
			return luaL_error(L, "tp.rule: keys must be strings");
		const char *key = lua_tostring(L, -2);

		if (strcmp(key, "match") == 0) {
			parse_match_table(lua, L, -1, &entry);
			has_match = true;
		} else if (strcmp(key, "tag") == 0) {
			int32_t t = check_rule_int(L, -1, "tp.rule.tag", 1, TP_TAG_COUNT);
			entry.tags = tp_tag_mask((unsigned int)t);
			entry.has_tags = true;
			has_tag_single = true;
		} else if (strcmp(key, "tags") == 0) {
			entry.tags = parse_tags_table(L, -1);
			entry.has_tags = true;
			has_tags_table = true;
		} else if (strcmp(key, "floating") == 0) {
			if (lua_type(L, -1) != LUA_TBOOLEAN)
				return luaL_error(L, "tp.rule.floating: expected boolean");
			entry.floating = lua_toboolean(L, -1) != 0;
			entry.has_floating = true;
		} else if (strcmp(key, "size") == 0) {
			parse_int_pair(L, -1, "tp.rule.size", "w", "h", TP_RULE_MIN_DIM,
						   TP_RULE_MAX_DIM, &entry.width, &entry.height);
			entry.has_size = true;
		} else if (strcmp(key, "position") == 0) {
			if (lua_type(L, -1) == LUA_TSTRING) {
				const char *s = lua_tostring(L, -1);
				if (strcmp(s, "center") != 0)
					return luaL_error(
						L, "tp.rule.position: expected 'center' or {x, y}");
				entry.pos_mode = TP_RULE_POS_CENTER;
			} else if (lua_type(L, -1) == LUA_TTABLE) {
				parse_int_pair(L, -1, "tp.rule.position", "x", "y",
							   TP_RULE_MIN_COORD, TP_RULE_MAX_COORD, &entry.x,
							   &entry.y);
				entry.pos_mode = TP_RULE_POS_COORDS;
			} else {
				return luaL_error(
					L, "tp.rule.position: expected 'center' or {x, y}");
			}
		} else if (strcmp(key, "monitor") == 0) {
			copy_nonempty_str(L, -1, "tp.rule.monitor", entry.monitor,
							  sizeof(entry.monitor));
			entry.has_monitor = true;
		} else {
			return luaL_error(L, "tp.rule: unknown key '%s'", key);
		}
		lua_pop(L, 1);
	}

	if (!has_match)
		return luaL_error(L, "tp.rule: missing 'match' table");
	if (has_tag_single && has_tags_table)
		return luaL_error(L, "tp.rule: cannot specify both 'tag' and 'tags'");
	if (!entry.has_tags && !entry.has_floating && !entry.has_size &&
		entry.pos_mode == TP_RULE_POS_NONE && !entry.has_monitor)
		return luaL_error(L, "tp.rule: must set at least one property "
							 "(tag, tags, floating, size, position, monitor)");

	entry.id = ++lua->next_rule_id;
	if (!rule_list_append(&lua->staged_rules, &entry))
		return luaL_error(L, "tp.rule: out of memory");

	lua_pushinteger(L, (lua_Integer)entry.id);
	return 1;
}

static int l_tp_remove_rule(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_rules)
		return luaL_error(
			L, "tp.remove_rule can only be called while loading config");

	if (lua_type(L, 1) != LUA_TNUMBER || !lua_isinteger(L, 1))
		return luaL_error(L, "tp.remove_rule: expected integer rule id");
	lua_Integer id = lua_tointeger(L, 1);
	bool removed =
		id > 0 && rule_list_remove_id(&lua->staged_rules, (uint64_t)id);
	lua_pushboolean(L, removed);
	return 1;
}

static int l_tp_clear_rules(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_rules)
		return luaL_error(
			L, "tp.clear_rules can only be called while loading config");
	rule_list_clear(&lua->staged_rules);
	return 0;
}

void tp_lua_rules_open(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (lua) {
		lua->string_find_ref = LUA_NOREF;
		lua_getglobal(L, "string");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "find");
			if (lua_isfunction(L, -1))
				lua->string_find_ref = luaL_ref(L, LUA_REGISTRYINDEX);
			else
				lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}

	lua_pushcfunction(L, l_tp_rule);
	lua_setfield(L, -2, "rule");
	lua_pushcfunction(L, l_tp_remove_rule);
	lua_setfield(L, -2, "remove_rule");
	lua_pushcfunction(L, l_tp_clear_rules);
	lua_setfield(L, -2, "clear_rules");
}

void tp_lua_rules_stage_begin(struct tp_lua *lua) {
	rule_list_clear(&lua->staged_rules);
	lua->staging_rules = true;
}

void tp_lua_rules_stage_commit(struct tp_lua *lua) {
	lua->staging_rules = false;
	rule_list_clear(&lua->active_rules);
	lua->active_rules = lua->staged_rules;
	lua->staged_rules = (struct tp_lua_rule_list){0};
}

void tp_lua_rules_stage_abort(struct tp_lua *lua) {
	lua->staging_rules = false;
	rule_list_clear(&lua->staged_rules);
}

void tp_lua_rules_finish(struct tp_lua *lua) {
	if (!lua)
		return;
	if (lua->L && lua->string_find_ref != LUA_NOREF) {
		luaL_unref(lua->L, LUA_REGISTRYINDEX, lua->string_find_ref);
		lua->string_find_ref = LUA_NOREF;
	}
	rule_list_clear(&lua->staged_rules);
	rule_list_clear(&lua->active_rules);
	lua->staging_rules = false;
}

size_t tp_lua_rule_count(const struct tp_lua *lua) {
	return lua ? lua->active_rules.len : 0;
}

bool tp_lua_eval_rules(struct tp_lua *lua, const char *app_id,
					   const char *title, struct tp_rule_result *out) {
	if (!out)
		return false;
	*out = (struct tp_rule_result){0};
	if (!lua || !lua->L || lua->active_rules.len == 0)
		return false;

	for (size_t i = 0; i < lua->active_rules.len; i++) {
		const struct tp_lua_rule *r = &lua->active_rules.items[i];
		if (r->has_app_id && !match_lua_pattern(lua, app_id, r->app_id))
			continue;
		if (r->has_title && !match_lua_pattern(lua, title, r->title))
			continue;

		out->matched = true;
		if (r->has_tags) {
			out->has_tags = true;
			out->tags = r->tags;
		}
		if (r->has_floating) {
			out->has_floating = true;
			out->floating = r->floating;
		}
		if (r->has_size) {
			out->has_size = true;
			out->width = r->width;
			out->height = r->height;
		}
		if (r->pos_mode != TP_RULE_POS_NONE) {
			out->pos_mode = r->pos_mode;
			out->x = r->x;
			out->y = r->y;
		}
		if (r->has_monitor) {
			out->has_monitor = true;
			memcpy(out->monitor, r->monitor, sizeof(out->monitor));
		}
	}

	return out->matched;
}
