/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Text input v3 and input method v2 relay, with session-lock isolation so
 * an input method can never observe or intercept keystrokes while locked.
 */
#ifndef TAM_PANE_TEXT_INPUT_H
#define TAM_PANE_TEXT_INPUT_H

#include <stdbool.h>

struct tp_server;
struct wlr_keyboard;
struct wlr_keyboard_key_event;
struct wlr_surface;

bool tp_text_input_init(struct tp_server *s);
void tp_text_input_finish(struct tp_server *s);

/* Update the focused surface for text-input-v3 / input-method-v2.
 * Forces NULL whenever s->locked is true. */
void tp_text_input_set_focus(struct tp_server *s, struct wlr_surface *surface);

/* Forward key/modifier events to an active input-method keyboard grab.
 * Always returns false while s->locked is true so lock-screen input is never
 * intercepted by an input method daemon. */
bool tp_text_input_forward_key(struct tp_server *s, struct wlr_keyboard *kb,
							   const struct wlr_keyboard_key_event *ev);
bool tp_text_input_forward_modifiers(struct tp_server *s,
									 struct wlr_keyboard *kb);

#endif
