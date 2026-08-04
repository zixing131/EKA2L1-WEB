local cpu = require('eka2l1.cpu')
local events = require('eka2l1.events')

local function skipEmptyAvkonMenu()
    -- R1 is Count()-1. LuaJIT exposes uint32_t as an unsigned number, so test
    -- the sign bit through its numeric range.
    if tonumber(cpu.getReg(1)) >= 0x80000000 then
        cpu.setReg(7, 0)
        cpu.setReg(15, 0x814F5210)
    end
end

events.registerBreakpointHook('eikcoctl.dll', 0x814F511D, 0, 0x1000489E,
    skipEmptyAvkonMenu, 0x17EDD4DD)
