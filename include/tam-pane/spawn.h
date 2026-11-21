/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_SPAWN_H
#define TAM_PANE_SPAWN_H

/* Start `cmd` with /bin/sh -c in its own session, detached. Never waits on
 * the program, so the event loop never blocks on a child. */
void tp_spawn(const char *cmd);

/* Start argv[0] (found on PATH) with the NULL-terminated argv, detached as
 * tp_spawn does, with no shell: arguments reach the program unchanged. */
void tp_spawn_argv(const char *const argv[]);

#endif
