/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/ipc.h"
#include "tam-pane/lua.h"
#include "tam-pane/server.h"
#include "tam-pane/spawn.h"
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tam.h>
#include <time.h>
#include <unistd.h>
#include <wlr/util/log.h>

static double monotonic_ms(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0.0;
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void usage(FILE *out) {
	fprintf(out, "usage: tam-pane [-v | --version] [-d] [-c config-file] [-s startup-command]\n"
				 "       tam-pane msg [-S socket] <command> [args...]\n"
				 "  -v, --version  print the version and exit\n"
				 "  -d             log debug messages\n"
				 "  -c             load Lua configuration from config-file\n"
				 "  -s             run a command with /bin/sh -c once started\n");
}

int main(int argc, char *argv[]) {
	if (argc >= 2 && strcmp(argv[1], "msg") == 0)
		return tp_ipc_client_main(argc - 1, argv + 1);

	const char *startup = NULL;
	const char *config_path = NULL;
	bool debug = false;
	const struct option long_opts[] = {
		{"version", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0},
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "vdc:s:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'v':
			printf("tam-pane %s\n", TP_VERSION);
			return 0;
		case 'd':
			debug = true;
			break;
		case 'c':
			config_path = optarg;
			break;
		case 's':
			startup = optarg;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind < argc) {
		usage(stderr);
		return 2;
	}
	if (!getenv("XDG_RUNTIME_DIR")) {
		fprintf(stderr, "tam-pane: XDG_RUNTIME_DIR must be set\n");
		return 1;
	}

	tam_log_set_level(debug ? TAM_LOG_DEBUG : TAM_LOG_INFO);
	wlr_log_init(debug ? WLR_DEBUG : WLR_ERROR, NULL);

	double t0 = monotonic_ms();
	struct tp_server server = {0};
	tp_settings_defaults(&server.settings);

	server.lua = tp_lua_create();
	char default_path[PATH_MAX];
	if (config_path) {
		tp_server_set_config_path(&server, config_path);
	} else if (tp_lua_default_config_path(default_path, sizeof(default_path))) {
		tp_server_set_config_path(&server, default_path);
	}

	if (server.lua) {
		if (config_path) {
			tp_lua_load_file(server.lua, config_path, true, &server.settings,
							 &server.settings);
		} else if (server.config_path[0]) {
			tp_lua_load_file(server.lua, server.config_path, false,
							 &server.settings, &server.settings);
		}
	} else {
		tam_log_error("cannot create Lua state (using compiled-in defaults)");
	}
	double t_lua = monotonic_ms();

	int status = 1;
	if (tp_server_init(&server)) {
		double t_init = monotonic_ms();
		if (tp_server_start(&server)) {
			double t_start = monotonic_ms();
			tam_log_info("tam-pane %s running on WAYLAND_DISPLAY=%s", TP_VERSION,
						 server.socket);
			tam_log_info("tam-pane startup: lua=%.1fms init=%.1fms start=%.1fms total=%.1fms",
						 t_lua - t0, t_init - t_lua, t_start - t_init,
						 t_start - t0);
			if (startup)
				tp_spawn(startup);
			tp_server_run(&server);
			status = 0;
		}
	}
	tp_server_finish(&server);
	return status;
}
