-- Demo configuration for tam-pane 0.1.5 (B1: a tiled resize drag
-- survives windows leaving the tree). Run with: tools/demo-0.1.5

tp.config{ gaps = 8 }

-- Short-lived windows that open and close by themselves, so the tree
-- changes while a drag is held. Both keys work mid-drag.
tp.bind("Meta+o", tp.dsp.spawn("pane-blip 3"))
tp.bind("Meta+i", tp.dsp.spawn("pane-burst"))

-- Log the tree changing in the host terminal.
tp.on("window.open", function(ev)
	print(string.format("[demo] open   %s", ev.title))
end)
tp.on("window.close", function(ev)
	print(string.format("[demo] close  %s", ev.title))
end)

print("demo-0.1.5.lua loaded: Meta+O blip, Meta+I burst")
