-- Interactive demo config for tam-pane 0.1.3 (Lua window rules).
-- Run with: sandbox/demo-0.1.3

tp.config{
	gap_inner = 12,
	gap_outer = 16,
}

-- Rule 1: match by app_id Lua pattern -> floating with explicit size & position.
tp.rule{
	match = { app_id = "^demo%-float$" },
	floating = true,
	size = { 640, 400 },
	position = { 80, 60 },
}

-- Rule 2: match by title Lua pattern -> send to tag 2 without stealing focus.
tp.rule{
	match = { title = "^Tag2 Demo" },
	tag = 2,
}

-- Demo shortcuts (keeps Meta+f free for default fullscreen).
tp.bind("Meta+u", tp.dsp.spawn("foot -a demo-float -T 'Floating Rule Demo'"))
tp.bind("Meta+t", tp.dsp.spawn("foot -T 'Tag2 Demo'"))

tp.on("window.open", function(ev)
	print(string.format("[rule] window.open app=%s title=%q mon=%s tag=%d float=%s geom=%dx%d+%d+%d",
		ev.app_id, ev.title, ev.monitor, ev.tag, tostring(ev.floating),
		ev.w, ev.h, ev.x, ev.y))
end)
