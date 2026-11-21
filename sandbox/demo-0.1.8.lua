-- Demo configuration for tam-pane 0.1.8 (R1 finding 8: Lua memory is
-- capped, and pattern matching obeys the watchdog deadline).
-- Run with: sandbox/demo-0.1.8

tp.config{ gaps = 8 }

tp.on("startup", function()
	tp.spawn{"foot", "sh", "-c", 'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
end)

-- Meta+P: a pattern that would backtrack for hours in C. The watchdog
-- now stops it at the 100 ms callback deadline.
local subject = string.rep("a", 255)
tp.bind("Meta+p", function()
	print("[demo] runaway pattern: starting")
	string.find(subject, ".-.-.-.-.-.-b")
	print("[demo] runaway pattern: finished (should never print)")
end)

-- Meta+M: doubles a string until Lua runs out of its 64 MiB.
tp.bind("Meta+m", function()
	print("[demo] memory bomb: starting")
	local s = "x"
	while true do s = s .. s end
end)

-- Meta+G: long, legitimate matching within the deadline still works.
tp.bind("Meta+g", function()
	local n = select(2, string.rep("ab", 100000):gsub("a", "c"))
	print(string.format("[demo] gsub replaced %d letters", n))
end)

-- Meta+N opens a window titled with 300 a's and then TAIL. The first rule
-- is a runaway pattern: it is cut off and the window still opens. The
-- second rule would send the window to tag 2, but rules see only the
-- first 255 bytes of a title, so TAIL is never seen.
tp.rule{ match = { title = ".-.-.-.-.-.-b" }, floating = true }
tp.rule{ match = { title = "TAIL" }, tag = 2 }
tp.bind("Meta+n", tp.dsp.spawn("pane-long-title"))

print("demo-0.1.8.lua loaded")
