/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Keyboard shortcuts inhibit protocol (zwp_keyboard_shortcuts_inhibit_manager_v1).
 *
 * Allows a focused Wayland surface (such as the Keybinding Explorer's
 * "Try a shortcut" mode, VMs, or remote desktop clients) to temporarily
 * inhibit compositor Lua key bindings while it holds keyboard focus.
 * Hard-wired compositor keys (VT switch, emergency quit) and session lock
 * are never inhibited.
 */
#ifndef TAM_PANE_SHORTCUTS_INHIBIT_H
#define TAM_PANE_SHORTCUTS_INHIBIT_H

#include <stdbool.h>

struct tp_server;

bool tp_shortcuts_inhibit_init(struct tp_server *s);
void tp_shortcuts_inhibit_finish(struct tp_server *s);

/* Reconcile the active shortcut inhibitor with current keyboard focus and
 * session-lock state. */
void tp_shortcuts_inhibit_update(struct tp_server *s);

/* True when compositor key bindings should be bypassed for the currently
 * focused surface. */
bool tp_shortcuts_inhibited(const struct tp_server *s);

#endif
