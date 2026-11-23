-- Demo configuration for tam-pane 0.2.6 (opt-in XWayland).
-- Run with: sandbox/demo-0.2.6, which puts GUIDE, TP_BIN, DEMO_X11, RES,
-- CHECK_SH above this.

tp.config{
	gaps = 8,
}

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
	tp.spawn{DEMO_X11, "--title", "tam-pane X11 demo"}
	tp.spawn{CHECK_SH, TP_BIN, RES}
end)

print("demo-0.2.6.lua loaded")
