/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Key bindings: compiled in until Lua configuration arrives in 0.1.1. */
#ifndef TAM_PANE_BINDINGS_H
#define TAM_PANE_BINDINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct tp_server;

/* The modifier Tamlinux calls Meta (the logo key). */
#define TP_MOD_META WLR_MODIFIER_LOGO

/* Run the binding for (mods, sym). Returns true when one matched. */
bool tp_bindings_handle(struct tp_server *s, uint32_t mods, xkb_keysym_t sym);

#endif
