/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal XCB client for tam-pane 0.2.6 (XWayland) tests and sandbox/demo-0.2.6.
 * Opens one managed X11 window (WM_CLASS, WM_NAME, WM_DELETE_WINDOW) with a
 * solid slate-teal fill and optional override-redirect popup (--popup or 'p').
 * Exits cleanly on WM_DELETE_WINDOW, Escape, or 'q'.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name) {
	xcb_intern_atom_cookie_t ck =
		xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
	xcb_intern_atom_reply_t *rep = xcb_intern_atom_reply(conn, ck, NULL);
	xcb_atom_t atom = rep ? rep->atom : XCB_ATOM_NONE;
	free(rep);
	return atom;
}

static void draw_rect(xcb_connection_t *conn, xcb_window_t win, xcb_gcontext_t gc,
					  uint16_t w, uint16_t h, uint32_t bg, uint32_t border) {
	uint32_t fg = bg;
	xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &fg);
	xcb_rectangle_t full = {0, 0, w, h};
	xcb_poly_fill_rectangle(conn, win, gc, 1, &full);
	if (w > 12 && h > 12) {
		fg = border;
		xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &fg);
		xcb_rectangle_t frame = {6, 6, (uint16_t)(w - 12), (uint16_t)(h - 12)};
		xcb_poly_rectangle(conn, win, gc, 1, &frame);
	}
	xcb_flush(conn);
}

int main(int argc, char *argv[]) {
	const char *title = "tam-pane X11 demo";
	const char *wclass = "demo-x11";
	bool show_popup = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
			title = argv[++i];
		} else if (strcmp(argv[i], "--class") == 0 && i + 1 < argc) {
			wclass = argv[++i];
		} else if (strcmp(argv[i], "--popup") == 0) {
			show_popup = true;
		} else {
			fprintf(stderr,
					"usage: demo-x11 [--title text] [--class name] [--popup]\n");
			return 2;
		}
	}

	int screen_num = 0;
	xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
	if (!conn || xcb_connection_has_error(conn)) {
		fprintf(stderr, "demo-x11: cannot connect to DISPLAY=%s\n",
				getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
		if (conn)
			xcb_disconnect(conn);
		return 1;
	}

	const xcb_setup_t *setup = xcb_get_setup(conn);
	xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < screen_num; i++)
		xcb_screen_next(&iter);
	xcb_screen_t *screen = iter.data;

	xcb_atom_t wm_protocols = intern_atom(conn, "WM_PROTOCOLS");
	xcb_atom_t wm_delete = intern_atom(conn, "WM_DELETE_WINDOW");
	xcb_atom_t net_wm_name = intern_atom(conn, "_NET_WM_NAME");
	xcb_atom_t utf8_string = intern_atom(conn, "UTF8_STRING");

	xcb_window_t win = xcb_generate_id(conn);
	uint16_t win_w = 480, win_h = 320;
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	uint32_t vals[2] = {
		0x1b4965,
		XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
			XCB_EVENT_MASK_KEY_PRESS,
	};
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 40, 40,
					  win_w, win_h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
					  screen->root_visual, mask, vals);

	/* WM_NAME + _NET_WM_NAME */
	xcb_icccm_set_wm_name(conn, win, XCB_ATOM_STRING, 8,
						  (uint32_t)strlen(title), title);
	if (net_wm_name != XCB_ATOM_NONE && utf8_string != XCB_ATOM_NONE) {
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_name,
							utf8_string, 8, (uint32_t)strlen(title), title);
	}

	/* WM_CLASS: "instance\0class\0" */
	size_t clen = strlen(wclass);
	size_t prop_len = (clen + 1) * 2;
	char *class_buf = malloc(prop_len);
	if (class_buf) {
		memcpy(class_buf, wclass, clen + 1);
		memcpy(class_buf + clen + 1, wclass, clen + 1);
		xcb_icccm_set_wm_class(conn, win, (uint32_t)prop_len, class_buf);
		free(class_buf);
	}

	if (wm_protocols != XCB_ATOM_NONE && wm_delete != XCB_ATOM_NONE) {
		xcb_icccm_set_wm_protocols(conn, win, wm_protocols, 1, &wm_delete);
	}

	xcb_gcontext_t gc = xcb_generate_id(conn);
	uint32_t gc_val = 0x1b4965;
	xcb_create_gc(conn, gc, win, XCB_GC_FOREGROUND, &gc_val);

	xcb_map_window(conn, win);

	/* Optional override-redirect popup window (created on demand or --popup). */
	xcb_window_t popup = XCB_WINDOW_NONE;
	bool popup_mapped = false;
	if (show_popup) {
		popup = xcb_generate_id(conn);
		uint32_t pmask =
			XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
		uint32_t pvals[3] = {0xe09f3e, 1, XCB_EVENT_MASK_EXPOSURE};
		xcb_create_window(conn, XCB_COPY_FROM_PARENT, popup, screen->root, 80,
						  80, 180, 72, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
						  screen->root_visual, pmask, pvals);
		xcb_map_window(conn, popup);
		popup_mapped = true;
	}
	xcb_flush(conn);

	bool running = true;
	xcb_generic_event_t *ev;
	while (running && (ev = xcb_wait_for_event(conn)) != NULL) {
		uint8_t type = ev->response_type & ~0x80;
		switch (type) {
		case XCB_EXPOSE: {
			xcb_expose_event_t *ex = (xcb_expose_event_t *)ev;
			if (ex->window == win)
				draw_rect(conn, win, gc, win_w, win_h, 0x1b4965, 0xbee9e8);
			else if (ex->window == popup && popup_mapped)
				draw_rect(conn, popup, gc, 180, 72, 0xe09f3e, 0xfff3b0);
			break;
		}
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t *cfg =
				(xcb_configure_notify_event_t *)ev;
			if (cfg->window == win) {
				win_w = cfg->width;
				win_h = cfg->height;
				draw_rect(conn, win, gc, win_w, win_h, 0x1b4965, 0xbee9e8);
			}
			break;
		}
		case XCB_CLIENT_MESSAGE: {
			xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
			if (cm->type == wm_protocols && cm->data.data32[0] == wm_delete)
				running = false;
			break;
		}
		case XCB_KEY_PRESS: {
			xcb_key_press_event_t *kp = (xcb_key_press_event_t *)ev;
			/* Evdev/X11 keycodes: Esc=9, q=24, p=33 */
			if (kp->detail == 9 || kp->detail == 24) {
				running = false;
			} else if (kp->detail == 33) {
				if (popup == XCB_WINDOW_NONE) {
					popup = xcb_generate_id(conn);
					uint32_t pmask = XCB_CW_BACK_PIXEL |
									 XCB_CW_OVERRIDE_REDIRECT |
									 XCB_CW_EVENT_MASK;
					uint32_t pvals[3] = {0xe09f3e, 1, XCB_EVENT_MASK_EXPOSURE};
					xcb_create_window(conn, XCB_COPY_FROM_PARENT, popup,
									  screen->root, 80, 80, 180, 72, 0,
									  XCB_WINDOW_CLASS_INPUT_OUTPUT,
									  screen->root_visual, pmask, pvals);
				}
				if (popup_mapped) {
					xcb_unmap_window(conn, popup);
					popup_mapped = false;
				} else {
					xcb_map_window(conn, popup);
					popup_mapped = true;
				}
				xcb_flush(conn);
			}
			break;
		}
		default:
			break;
		}
		free(ev);
	}

	if (popup != XCB_WINDOW_NONE)
		xcb_destroy_window(conn, popup);
	xcb_free_gc(conn, gc);
	xcb_destroy_window(conn, win);
	xcb_flush(conn);
	xcb_disconnect(conn);
	return 0;
}
