/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_SPAWN_H
#define TAM_PANE_SPAWN_H

/* Start `cmd` with /bin/sh -c in its own session, detached. Never waits on
 * the program, so the event loop never blocks on a child. */
void tp_spawn(const char *cmd);

#endif
