-- SPDX-License-Identifier: GPL-3.0-or-later
-- Runtime-only Hyprland adapter. Loaded by nested-input; never sourced by config.
return function(pid)
    local name = 'tam-pane-nested-input'
    if pid == 'undo' then
        if _G.tp_nested_input then _G.tp_nested_input.cleanup() end
        return 'removed'
    end
    assert(not _G.tp_nested_input, 'nested passthrough already installed')
    assert(hl.get_current_submap() == '', 'leave the current submap first')
    local target
    for _, w in ipairs(hl.get_windows()) do
        if w.pid == pid then
            assert(not target, 'ambiguous nested windows')
            target = w
        end
    end
    if not target then return 'waiting' end
    local s = { subscriptions = {}, escaped = false }
    _G.tp_nested_input = s
    local function submap(value) hl.dispatch(hl.dsp.submap(value)) end
    function s.cleanup()
        if hl.get_current_submap() == name then submap('reset') end
        if s.binding then s.binding:remove() end
        for _, event in ipairs(s.subscriptions) do event:remove() end
        _G.tp_nested_input = nil
    end
    local function focus(w)
        if w == target and not s.escaped then
            if hl.get_current_submap() == '' then submap(name) end
        elseif hl.get_current_submap() == name then submap('reset') end
    end
    local ok, err = pcall(function()
        hl.define_submap(name, function()
            s.binding = hl.bind('CTRL + ALT + Escape', function()
                s.escaped = true
                submap('reset')
            end, { description = 'Leave tam-pane input passthrough', locked = true })
        end)
        table.insert(s.subscriptions, hl.on('window.active', focus))
        table.insert(s.subscriptions, hl.on('window.close', function(w)
            if w == target then s.cleanup() end
        end))
        focus(hl.get_active_window())
    end)
    if not ok then s.cleanup(); error(err) end
    return 'installed'
end
