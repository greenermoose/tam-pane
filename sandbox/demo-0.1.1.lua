-- Interactive demo config for tam-pane 0.1.1 (Lua bindings & dispatchers).
-- Run with: sandbox/demo-0.1.1

tp.config{
	gap_inner = 12,
	gap_outer = 16,
	resize_step = 48,
}

-- Custom dispatcher binding (C fast path).
tp.bind("Meta+q", tp.dsp.close())

-- Plain Lua callback composing multiple dispatchers.
tp.bind("Meta+x", function()
	tp.dsp.tag("view", 2)()
	tp.dsp.terminal()()
end)

-- Plain Lua callback toggling floating on the focused window.
tp.bind("Meta+t", function()
	tp.dsp.toggle_float()()
end)

-- Runaway loop in a key callback: cut off by the 100ms / 1M-instruction
-- watchdog so the compositor stays responsive.
tp.bind("Meta+b", function()
	while true do end
end)

-- Unbind a default to prove tp.unbind works (Meta+v no longer floats;
-- use Meta+t instead).
tp.unbind("Meta+v")
