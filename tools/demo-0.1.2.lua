-- Interactive demo config for tam-pane 0.1.2 (Lua event hooks).
-- Run with: tools/demo-0.1.2

tp.config{
	gap_inner = 12,
	gap_outer = 16,
}

tp.on("window.open", function(ev)
	print(string.format("[hook] window.open   app=%s title=%q mon=%s tag=%d float=%s (%dx%d)",
		ev.app_id, ev.title, ev.monitor, ev.tag, tostring(ev.floating), ev.w, ev.h))
end)

tp.on("window.close", function(ev)
	print(string.format("[hook] window.close  app=%s title=%q mon=%s",
		ev.app_id, ev.title, ev.monitor))
end)

tp.on("window.focus", function(ev)
	if ev.focused then
		print(string.format("[hook] window.focus  app=%s title=%q mon=%s tag=%d",
			ev.app_id, ev.title, ev.monitor, ev.tag))
	else
		print(string.format("[hook] window.focus  (cleared) mon=%s", ev.monitor))
	end
end)

tp.on("tag.change", function(ev)
	print(string.format("[hook] tag.change    mon=%s prev=0x%x now=0x%x (tag %d)",
		ev.monitor, ev.prev_tags, ev.tags, ev.tag))
end)

tp.on("monitor.add", function(ev)
	print(string.format("[hook] monitor.add   %s %dx%d+%d+%d scale=%.2f",
		ev.name, ev.w, ev.h, ev.x, ev.y, ev.scale))
end)

tp.on("monitor.remove", function(ev)
	print(string.format("[hook] monitor.remove %s %dx%d", ev.name, ev.w, ev.h))
end)

-- Runaway hook test on tag 9: switching to tag 9 triggers an infinite loop
-- in a tag.change hook; the 100 ms / 1M-instruction watchdog cuts it off and
-- the second hook still prints.
tp.on("tag.change", function(ev)
	if ev.tag == 9 then
		while true do end
	end
end)

tp.on("tag.change", function(ev)
	if ev.tag == 9 then
		print("[hook] tag.change    survived runaway hook on tag 9!")
	end
end)
