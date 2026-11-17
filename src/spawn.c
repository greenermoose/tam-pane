/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/spawn.h"
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <tam.h>
#include <unistd.h>

/*
 * Double fork: the intermediate child exits at once and is reaped here, so
 * the program is adopted by init (or the session's subreaper) and the
 * compositor never has zombies to collect or a child to wait on.
 */
void tp_spawn(const char *cmd) {
	if (!cmd || !*cmd)
		return;

	pid_t pid = fork();
	if (pid < 0) {
		tam_log_error("spawn: fork failed for '%s'", cmd);
		return;
	}
	if (pid == 0) {
		setsid();
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
}
