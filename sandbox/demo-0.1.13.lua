-- Demo configuration for tam-pane 0.1.13 (R1 finding 5: menus near a
-- screen edge stay on screen). Run with: sandbox/demo-0.1.13, which puts
-- GUIDE above this.

tp.config{ gaps = 8 }

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

-- Meta+O opens Files (nautilus) on a session bus of its own, so it opens
-- here and not as a window of a Files already running on the desktop.
tp.bind("Meta+o", function()
	tp.spawn{"env", "-u", "DBUS_SESSION_BUS_ADDRESS", "-u", "DISPLAY",
		"GDK_BACKEND=wayland", "dbus-run-session", "--",
		"nautilus", "--new-window"}
end)

print("demo-0.1.13.lua loaded: Meta+O opens Files for the menu check")
