-- Demo configuration for tam-pane 0.1.16 (R1 finding 14: merged code,
-- checked through the default floating size; B8: config errors show
-- their numbers). Run with: sandbox/demo-0.1.16, which puts GUIDE and
-- CFG above this.

tp.config{ gaps = 8 }

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

-- Meta+O breaks this session's config file (a copy in a temporary
-- folder): it appends a gaps value out of range. Meta+Shift+R then
-- reloads, fails, and keeps the previous configuration.
tp.bind("Meta+o", function()
	tp.spawn{"sh", "-c",
		'printf "%s\\n" "tp.config{ gaps = 999 }" >>"$1"', "sh", CFG}
	print("[demo] config broken: gaps = 999 appended; press Meta+Shift+R")
end)

print("demo-0.1.16.lua loaded: Meta+V float, Meta+O break, Meta+Shift+R")
