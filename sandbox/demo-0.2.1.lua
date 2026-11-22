-- Demo configuration for tam-pane 0.2.1 (P3 screens and capture:
-- screencopy, ext-image-copy-capture + capture-source companions gated via
-- tp.capture, wlr-output-management, gamma control, cursor-shape, xdg-output).
-- Run with: sandbox/demo-0.2.1, which puts GUIDE, SHOT, DENIED, DENIED_LOG
-- above this.

tp.config{ gaps = 8 }

tp.bind("Meta+s", function()
	tp.capture{"grim", SHOT}
end)

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
	-- Ordinary spawn (not tp.capture): must be denied raw capture globals.
	tp.spawn{"sh", "-c", 'grim "$1" >"$2" 2>&1 || true', "sh", DENIED, DENIED_LOG}
end)

print("demo-0.2.1.lua loaded: Meta+S captures nested output via tp.capture")
