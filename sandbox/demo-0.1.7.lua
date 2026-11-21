-- Demo configuration for tam-pane 0.1.7 (R1 findings 3 and 7: autostart
-- in a startup hook, and tp.spawn{...} with no shell).
-- Run with: sandbox/demo-0.1.7, which puts GUIDE and LOG above this text.

tp.config{ gaps = 8 }

-- Autostart: runs once per session, after tam-pane's socket is up. The
-- guide terminal you read now was opened here. A reload (Meta+Shift+R)
-- replaces this hook but does not run it again.
tp.on("startup", function()
	print("[demo] startup hook ran: opening the guide")
	tp.spawn{"foot", "sh", "-c", 'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

-- Every window title goes to a notification and to LOG as one plain
-- argument. A title full of shell syntax stays text.
tp.on("window.open", function(ev)
	tp.spawn{"notify-send", "tam-pane opened", ev.title}
	tp.spawn{"sh", "-c", 'printf "%s\n" "$1" >> "$2"', "sh", ev.title, LOG}
end)

-- Meta+N: a window whose title tries to run a command.
tp.bind("Meta+n", tp.dsp.spawn("pane-evil-title"))

print("demo-0.1.7.lua loaded")
