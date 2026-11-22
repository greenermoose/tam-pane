-- Demo configuration for tam-pane 0.2.0 (P3 shell protocols: session-lock
-- via tp.lock, binding gate while locked, foreign-toplevel, ext-workspace,
-- idle notify/inhibit, output power).
-- Run with: sandbox/demo-0.2.0, which puts GUIDE and DEMO_LOCKER above this.

tp.config{ gaps = 8 }

tp.bind("Meta+l", function()
	tp.lock{DEMO_LOCKER}
end)

tp.bind("Meta+b", function()
	print("locked-allowed binding (Meta+B) fired")
end, { locked = true })

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

print("demo-0.2.0.lua loaded: Meta+L launches demo-locker via tp.lock")
