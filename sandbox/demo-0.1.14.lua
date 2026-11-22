-- Demo configuration for tam-pane 0.1.14 (B6: GTK 4 programs run nested;
-- R1 finding 5: menus near a screen edge stay on screen). Run with:
-- sandbox/demo-0.1.14, which puts GUIDE above this.

tp.config{ gaps = 8 }

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

-- Meta+O opens Files (nautilus) on a session bus of its own, so it opens
-- here and not as a window of a Files already running on the desktop.
-- Portals are off and the host's Hyprland is hidden, so nothing started
-- on that bus talks to the desktop's Hyprland.
tp.bind("Meta+o", function()
	tp.spawn{"env", "-u", "DBUS_SESSION_BUS_ADDRESS", "-u", "DISPLAY",
		"-u", "HYPRLAND_INSTANCE_SIGNATURE", "GDK_BACKEND=wayland",
		"GDK_DEBUG=no-portals", "dbus-run-session", "--",
		"nautilus", "--new-window"}
end)

print("demo-0.1.14.lua loaded: Meta+O opens Files for the menu check")
