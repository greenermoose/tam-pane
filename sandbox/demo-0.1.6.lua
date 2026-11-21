-- Demo configuration for tam-pane 0.1.6 (R1 finding 2: a hook that keeps
-- re-triggering itself no longer freezes the compositor).
-- Run with: sandbox/demo-0.1.6

tp.config{ gaps = 8 }

-- Tag ping-pong: viewing tag 8 or 9 makes this hook view the other one,
-- which raises tag.change again, forever. Up to 0.1.5 that froze tam-pane;
-- now the cascade stops after 16 rounds. Other tags behave normally.
local bounce = 0
tp.on("tag.change", function(ev)
	if ev.tag ~= 8 and ev.tag ~= 9 then
		bounce = 0
		return
	end
	bounce = bounce + 1
	print(string.format("[demo] bounce %d: on tag %d", bounce, ev.tag))
	tp.dsp.tag("view", ev.tag == 8 and 9 or 8)()
end)

-- Focus spin: while switched on (Meta+Y), every focus change moves focus
-- to the next window, which changes focus again.
local spin = false
local spins = 0
tp.bind("Meta+y", function()
	spin = not spin
	spins = 0
	print("[demo] focus spin " .. (spin and "ON: click a window" or "off"))
end)
tp.on("window.focus", function(ev)
	if not spin or not ev.focused then
		return
	end
	spins = spins + 1
	print(string.format("[demo] spin %d: %s", spins, ev.title))
	tp.dsp.focus("next")()
end)

print("demo-0.1.6.lua loaded: Meta+8 tag ping-pong, Meta+Y focus spin")
