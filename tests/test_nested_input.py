"""Exercise the real Lua adapter with a mock of Hyprland's runtime API."""
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NestedInput(unittest.TestCase):
    def test_runtime_lifecycle(self):
        script = r'''
local install = assert(loadfile('tools/nested-input.lua'))()
local current, active, callbacks, bindings = '', nil, {}, {}
local target, other = {pid=42}, {pid=43}
local windows = {target}
local removed = 0
hl = {
 get_current_submap=function() return current end,
 get_windows=function() return windows end,
 get_active_window=function() return active end,
 dispatch=function(value) current=value=='reset' and '' or value end,
 dsp={submap=function(value) return value end},
 define_submap=function(name, fn) fn() end,
 bind=function(keys, fn)
   local b={fn=fn, remove=function() removed=removed+1 end}
   table.insert(bindings,b); return b
 end,
 on=function(event, fn)
   callbacks[event]=fn
   return {remove=function() callbacks[event]=nil end}
 end,
}
assert(install(99)=='waiting')
current='other'; assert(not pcall(install,42)); current=''
windows={target,{pid=42}}; assert(not pcall(install,42)); windows={target}
active=target
assert(install(42)=='installed'); assert(current=='tam-pane-nested-input')
assert(not pcall(install,42))
callbacks['window.active'](other); assert(current=='')
callbacks['window.active'](target); assert(current=='tam-pane-nested-input')
bindings[#bindings].fn(); assert(current=='')
callbacks['window.active'](other); callbacks['window.active'](target)
assert(current=='') -- Escape latches until explicit reactivation.
assert(install('undo')=='removed'); assert(not _G.tp_nested_input)
assert(not next(callbacks)); assert(removed==1)
install('undo'); assert(removed==1)
install(42); callbacks['window.close'](target)
assert(current==''); assert(not next(callbacks)); assert(not _G.tp_nested_input)
local on=hl.on
hl.on=function() error('injected subscription failure') end
assert(not pcall(install,42)); assert(not _G.tp_nested_input)
assert(current==''); assert(removed==3)
hl.on=on
install(42); current='someone-else'; install('undo')
assert(current=='someone-else')
print('lifecycle checks passed')
'''
        subprocess.run(['lua', '-'], input=script, text=True, cwd=ROOT, check=True)


if __name__ == '__main__':
    unittest.main()
