/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/spawn.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <tam.h>
#include <unistd.h>

/*
 * Double fork: the intermediate child exits at once and is reaped here, so
 * the program is adopted by init (or the session's subreaper) and the
 * compositor never has zombies to collect or a child to wait on.
 */
static void spawn_detached(const char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0) {
		tam_log_error("spawn: fork failed for '%s'", argv[0]);
		return;
	}
	if (pid == 0) {
		setsid();
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		if (fork() == 0) {
			execvp(argv[0], (char *const *)argv);
			dprintf(STDERR_FILENO, "tam-pane: cannot run %s: %s\n", argv[0],
					strerror(errno));
			_exit(127);
		}
		_exit(0);
	}
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
}

void tp_spawn(const char *cmd) {
	if (!cmd || !*cmd)
		return;
	const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
	spawn_detached(argv);
}

void tp_spawn_argv(const char *const argv[]) {
	if (!argv || !argv[0] || !*argv[0])
		return;
	spawn_detached(argv);
}
