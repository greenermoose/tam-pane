-- Demo configuration for tam-pane 0.2.5 (layouts and scratchpad).
-- Run with: sandbox/demo-0.2.5, which puts GUIDE, TP_BIN, RES, CHECK_SH
-- above this.

tp.config{
	gaps = 8,
	master_ratio = 0.55,
	scroller_width = 0.5,
}

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
	tp.spawn{"foot", "--title=peer-1", "sh", "-c",
		'echo "Peer 1: try Meta+Space (layouts) and Meta+Shift+- / Meta+- (scratchpad)"; exec "${SHELL:-/bin/sh}"'}
	tp.spawn{"foot", "--title=peer-2", "sh", "-c",
		'echo "Peer 2: try Meta+J/K in scroller and monocle"; exec "${SHELL:-/bin/sh}"'}
	tp.spawn{CHECK_SH, TP_BIN, RES}
end)

print("demo-0.2.5.lua loaded")
