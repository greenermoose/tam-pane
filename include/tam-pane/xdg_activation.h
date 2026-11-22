/* SPDX-License-Identifier: GPL-3.0-or-later */
/* xdg-activation-v1: token-based window activation (ignored while locked). */
#ifndef TAM_PANE_XDG_ACTIVATION_H
#define TAM_PANE_XDG_ACTIVATION_H

#include <stdbool.h>

struct tp_server;

bool tp_xdg_activation_init(struct tp_server *s);
void tp_xdg_activation_finish(struct tp_server *s);

#endif
