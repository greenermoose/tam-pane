/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Screen capture protocols (0.2.1):
 *   - zwlr_screencopy_manager_v1
 *   - ext_image_copy_capture_manager_v1
 *   - ext_output_image_capture_source_manager_v1
 *   - ext_foreign_toplevel_image_capture_source_manager_v1
 *
 * All four globals are hidden from ordinary clients and granted only to
 * connections registered via tp_security_add_capture_client() (such as
 * tools or portal backends launched with tp_capture_launch*).
 */
#ifndef TAM_PANE_CAPTURE_H
#define TAM_PANE_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tp_client;
struct tp_server;

bool tp_capture_init(struct tp_server *s);
void tp_capture_finish(struct tp_server *s);

/* Detach any cached toplevel capture source when a client unmaps/destroys. */
void tp_capture_client_finish(struct tp_client *c);

/* Detach all cached toplevel capture sources on renderer loss so subsequent
 * requests recreate them against the new renderer and allocator. */
void tp_capture_reset_sources(struct tp_server *s);

/* Freeze/unfreeze all enabled outputs onto TP_LAYER_FREEZE (below OVERLAY). */
bool tp_capture_freeze(struct tp_server *s);
void tp_capture_unfreeze(struct tp_server *s);

/* Sample the RGB pixel at layout coordinates (lx, ly). Refused when locked. */
bool tp_capture_sample_pixel(struct tp_server *s, int32_t lx, int32_t ly,
							 uint8_t rgb[3], char *err, size_t err_size);

/* Launch a capture tool or portal backend over a pre-created WAYLAND_SOCKET
 * and grant that exact connection access to the raw capture globals. */
void tp_capture_launch(struct tp_server *s, const char *cmd);
void tp_capture_launch_argv(struct tp_server *s, const char *const argv[]);

#endif
