-- Demo configuration for tam-pane 0.2.4 (R1 finding 15: deferred arrange).
-- Run with: sandbox/demo-0.2.4, which puts GUIDE, TP_BIN, RES, CHECK_SH
-- above this.

tp.config{ gaps = 8 }

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
	tp.spawn{"foot", "--title=peer", "sh", "-c",
		'echo "Peer window for Meta+Right-drag split resize"; exec "${SHELL:-/bin/sh}"'}
	tp.spawn{CHECK_SH, TP_BIN, RES}
end)

print("demo-0.2.4.lua loaded")
