-- Demo configuration for tam-pane 0.1.9 (R1 finding 10: keys no config
-- can remove). Run with: tools/demo-0.1.9, which puts GUIDE above this.

tp.config{ gaps = 8 }

tp.on("startup", function()
	tp.spawn{"foot", "sh", "-c", 'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

-- A broken config: every binding is gone, including Meta+Shift+E (quit).
tp.clear_binds()

print("demo-0.1.9.lua loaded: no key bindings at all")
