-- Demo configuration for tam-pane 0.1.12 (R1 finding 4: a program that
-- starts fullscreen opens fullscreen). Run with: sandbox/demo-0.1.12,
-- which puts GUIDE above this.

tp.config{ gaps = 8 }

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

-- Meta+O starts a terminal that asks for fullscreen before it maps.
tp.bind("Meta+o", tp.dsp.spawn(
	"foot --fullscreen --title=starts-fullscreen sh -c " ..
	"'echo This terminal started fullscreen.; " ..
	"echo Press Meta+F to leave fullscreen, then type exit.; " ..
	"exec ${SHELL:-/bin/sh}'"))

-- The verdict goes to the host terminal.
tp.on("window.open", function(ev)
	if ev.title == "starts-fullscreen" then
		print(string.format("[demo] %s: the terminal opened %s",
			ev.fullscreen and "ok" or "FAIL",
			ev.fullscreen and "fullscreen" or "tiled"))
	end
end)

print("demo-0.1.12.lua loaded: Meta+O starts a fullscreen terminal")
