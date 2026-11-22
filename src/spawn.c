/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/spawn.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <tam.h>
#include <unistd.h>

/*
 * Double fork: the intermediate child exits at once and is reaped here, so
 * the program is adopted by init (or the session's subreaper) and the
 * compositor never has zombies to collect or a child to wait on.
 */
static void spawn_detached(const char *const argv[], int wayland_fd) {
	pid_t pid = fork();
	if (pid < 0) {
		tam_log_error("spawn: fork failed for '%s'", argv[0]);
		if (wayland_fd >= 0)
			close(wayland_fd);
		return;
	}
	if (pid == 0) {
		setsid();
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		if (fork() == 0) {
			if (wayland_fd >= 0) {
				int flags = fcntl(wayland_fd, F_GETFD);
				if (flags >= 0)
					fcntl(wayland_fd, F_SETFD, flags & ~FD_CLOEXEC);
				char fd_buf[32];
				snprintf(fd_buf, sizeof(fd_buf), "%d", wayland_fd);
				setenv("WAYLAND_SOCKET", fd_buf, 1);
			}
			execvp(argv[0], (char *const *)argv);
			dprintf(STDERR_FILENO, "tam-pane: cannot run %s: %s\n", argv[0],
					strerror(errno));
			_exit(127);
		}
		if (wayland_fd >= 0)
			close(wayland_fd);
		_exit(0);
	}
	if (wayland_fd >= 0)
		close(wayland_fd);
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
}

void tp_spawn(const char *cmd) {
	if (!cmd || !*cmd)
		return;
	const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
	spawn_detached(argv, -1);
}

void tp_spawn_argv(const char *const argv[]) {
	if (!argv || !argv[0] || !*argv[0])
		return;
	spawn_detached(argv, -1);
}

void tp_spawn_with_fd(const char *cmd, int wayland_fd) {
	if (!cmd || !*cmd) {
		if (wayland_fd >= 0)
			close(wayland_fd);
		return;
	}
	const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
	spawn_detached(argv, wayland_fd);
}

void tp_spawn_with_fd_argv(const char *const argv[], int wayland_fd) {
	if (!argv || !argv[0] || !*argv[0]) {
		if (wayland_fd >= 0)
			close(wayland_fd);
		return;
	}
	spawn_detached(argv, wayland_fd);
}
