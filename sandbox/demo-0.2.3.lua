-- Demo configuration for tam-pane 0.2.3 (P3 IPC socket and `tam-pane msg`).
-- Run with: sandbox/demo-0.2.3, which puts GUIDE, TP_BIN, CFG_PATH, IPC_RES,
-- SUB_LOG, CHECK_SH above this.

tp.config{ gaps = 8 }

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
	tp.spawn{CHECK_SH, TP_BIN, CFG_PATH, IPC_RES, SUB_LOG}
end)

print("demo-0.2.3.lua loaded")
