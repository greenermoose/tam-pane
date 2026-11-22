-- Demo configuration for tam-pane 0.1.19 (R1 finding 16: explicit focused
-- window). Run with: sandbox/demo-0.1.19, which puts GUIDE above this.

tp.config{ gaps = 8 }

tp.bind("Meta+g", function()
	tp.spawn{"nautilus", "--new-window"}
end)

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

print("demo-0.1.19.lua loaded: default bindings + Meta+G for Files")
