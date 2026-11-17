/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A minimal test harness: CHECK records failures, CHECK_DONE reports them. */
#ifndef TAM_PANE_TESTS_CHECK_H
#define TAM_PANE_TESTS_CHECK_H

#include <stdio.h>

static int check_failures;
static int check_count;

#define CHECK(cond)                                                            \
	do {                                                                       \
		check_count++;                                                         \
		if (!(cond)) {                                                         \
			check_failures++;                                                  \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
					#cond);                                                    \
		}                                                                      \
	} while (0)

#define CHECK_DONE(name)                                                       \
	do {                                                                       \
		printf("%s: %d checks, %d failed\n", name, check_count,                \
			   check_failures);                                                \
		return check_failures ? 1 : 0;                                         \
	} while (0)

#endif
