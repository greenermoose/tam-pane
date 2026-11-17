/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/server.h"
#include "tam-pane/spawn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tam.h>
#include <unistd.h>
#include <wlr/util/log.h>

static void usage(FILE *out) {
	fprintf(out, "usage: tam-pane [-v] [-d] [-s startup-command]\n"
				 "  -v  print the version and exit\n"
				 "  -d  log debug messages\n"
				 "  -s  run a command with /bin/sh -c once started\n");
}

int main(int argc, char *argv[]) {
	const char *startup = NULL;
	bool debug = false;
	int opt;

	while ((opt = getopt(argc, argv, "vds:h")) != -1) {
		switch (opt) {
		case 'v':
			printf("tam-pane %s\n", TP_VERSION);
			return 0;
		case 'd':
			debug = true;
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

	struct tp_server server = {0};
	tp_settings_defaults(&server.settings);

	int status = 1;
	if (tp_server_init(&server) && tp_server_start(&server)) {
		tam_log_info("tam-pane %s running on WAYLAND_DISPLAY=%s", TP_VERSION,
					 server.socket);
		if (startup)
			tp_spawn(startup);
		tp_server_run(&server);
		status = 0;
	}
	tp_server_finish(&server);
	return status;
}
