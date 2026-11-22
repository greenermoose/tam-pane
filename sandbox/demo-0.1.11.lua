-- Demo configuration for tam-pane 0.1.11 (R1 finding 12: a new window
-- splits the focused window). Run with: sandbox/demo-0.1.11, which puts
-- GUIDE above this.

tp.config{ gaps = 8 }

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

-- Remember where the focused window was. When a window opens, it must lie
-- inside the space the focused window had just before: that is the
-- window it split. The verdict goes to the host terminal.
local last = nil

tp.on("window.focus", function(ev)
	if ev.focused and not ev.floating then
		last = { title = ev.title, x = ev.x, y = ev.y, w = ev.w, h = ev.h }
	end
end)

tp.on("window.open", function(ev)
	if ev.floating or not last then
		return
	end
	local inside = ev.x >= last.x - 1 and ev.y >= last.y - 1
		and ev.x + ev.w <= last.x + last.w + 1
		and ev.y + ev.h <= last.y + last.h + 1
	print(string.format("[demo] %s: opened inside the focused window '%s'",
		inside and "ok" or "FAIL", last.title))
	last = nil
end)

print("demo-0.1.11.lua loaded: default bindings, placement check")
