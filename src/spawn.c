/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/spawn.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <tam.h>

/* tam_spawn_detached_opts (kept output, WAYLAND_SOCKET) needs 0.14.0. */
#if TAM_VERSION_MAJOR == 0 && TAM_VERSION_MINOR < 14
#error "tam-pane needs libtam 0.14.0 or later"
#endif
#include <unistd.h>

/*
 * tam_spawn_detached_opts double-forks, so the program is adopted by init
 * (or the session's subreaper) and the compositor never has zombies to
 * collect. It starts in a new session with default signal actions and
 * none of the compositor's descriptors; its output stays on the
 * compositor's log. A Wayland client given a socket gets it as
 * WAYLAND_SOCKET.
 */
static void spawn_detached(const char *const argv[], int wayland_fd) {
	char fd_env[48];
	const char *env[2] = {NULL, NULL};
	tam_proc_opts_t opts = {.keep_stdio = true};
	if (wayland_fd >= 0) {
		snprintf(fd_env, sizeof(fd_env), "WAYLAND_SOCKET=%d", wayland_fd);
		env[0] = fd_env;
		opts.keep_fd = wayland_fd;
		opts.env = env;
	}
	if (tam_spawn_detached_opts(argv, &opts) < 0)
		tam_log_error("spawn: cannot run '%s': %s", argv[0], strerror(errno));
	if (wayland_fd >= 0)
		close(wayland_fd);
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
