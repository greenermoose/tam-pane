-- Demo configuration for tam-pane 0.1.17 (B9: Meta+V floats a window
-- that opened tiled at half its monitor, centred, and later returns it to
-- its last floating place). Run with: sandbox/demo-0.1.17, which puts
-- GUIDE above this.

tp.config{ gaps = 8 }

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

print("demo-0.1.17.lua loaded: default bindings")
