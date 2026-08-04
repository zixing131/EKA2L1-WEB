local cpu = require('eka2l1.cpu')
local events = require('eka2l1.events')

local function skipEmptyAvkonMenu(cleanupAddress)
    -- R1 is Count()-1. LuaJIT exposes uint32_t as an unsigned number, so test
    -- the sign bit through its numeric range.
    if tonumber(cpu.getReg(1)) >= 0x80000000 then
        cpu.setReg(7, 0)
        cpu.setReg(15, cleanupAddress)
    end
end

local function skipRm409EmptyAvkonMenu()
    skipEmptyAvkonMenu(0x814F5210)
end

local function skipRm320EmptyAvkonMenu()
    skipEmptyAvkonMenu(0x82ED3068)
end

events.registerBreakpointHook('eikcoctl.dll', 0x814F511D, 0, 0x1000489E,
    skipRm409EmptyAvkonMenu, 0x17EDD4DD)
events.registerBreakpointHook('eikcoctl.dll', 0x82ED2F3D, 0, 0x1000489E,
    skipRm320EmptyAvkonMenu, 0xF2CDB190)
