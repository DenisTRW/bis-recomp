-- Measurement script for DeSmuME (Tools > Lua Scripting > New Lua Script Window).
-- Used only as a reference: logs, with frame numbers, every change of a few memory
-- locations while you play from the file select into Toad Town. Output:
--   build/logs/desmume_probe.txt
-- Compare with the port: docs/grafik.md, section "Nach „Start“".

local OUT = "C:\\Users\\denis\\Documents\\bis-decomp\\build\\logs\\desmume_probe.txt"
local f = io.open(OUT, "w")
f:write("DeSmuME probe for bis-recomp\n")

local function u8(a)  return memory.readbyte(a) end
local function u32(a) return memory.readdword(a) end

-- Values to watch. Pointers are followed only when they look like main RAM.
local function sample()
  local v = {}
  local g = u32(0x0210fda8)                     -- ov004 scene globals pointer
  v.G = g
  if g >= 0x02000000 and g < 0x02400000 then
    v.G68 = u32(g + 0x68)                       -- scene status word (bits 8-10: entry mode)
    v.G6c = u32(g + 0x6c)
    local top = u32(g + 0x34)                   -- top screen object
    local bot = u32(g + 0x38)                   -- bottom screen object
    v.top = top; v.bot = bot
    if top >= 0x02000000 and top < 0x02400000 then
      v.top_d0 = u8(top + 0xd0); v.top_c4 = u32(top + 0xc4); v.top_ce = memory.readword(top + 0xce)
    end
    if bot >= 0x02000000 and bot < 0x02400000 then
      v.bot_d0 = u8(bot + 0xd0); v.bot_c4 = u32(bot + 0xc4); v.bot_ce = memory.readword(bot + 0xce)
    end
  end
  for i = 0, 7 do v["req" .. i] = u32(0x020564e0 + i * 4) end   -- map transition request?
  v.bright = memory.readword(0x0400006c)
  return v
end

-- Who starts a fade? Log every execution of the fade start (func_ov003_0206ad4c) with the
-- return address (caller) and arguments. Other overlays share this address range, so only
-- calls on the two screen objects are logged.
local function reg(n) return memory.getregister(n) end
local function on_fade_start()
  local g = u32(0x0210fda8)
  local r0 = reg("r0")
  if g >= 0x02000000 and g < 0x02400000 and (r0 == u32(g + 0x34) or r0 == u32(g + 0x38)) then
    f:write(string.format("%6d  FADE r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x\n",
      emu.framecount(), r0, reg("r1"), reg("r2"), reg("r3"), reg("r14")))
    f:flush()
  end
end
memory.registerexec(0x0206ad4c, on_fade_start)

-- The fade is started by script command 277 (func_ov006_0211c43c). Log who runs it
-- (return address = interpreter) and the command data it gets in r3.
local function on_fade_cmd()
  local r3 = reg("r3")
  local words = {}
  for i = 0, 5 do words[#words + 1] = string.format("%08x", u32(r3 + i * 4)) end
  f:write(string.format("%6d  CMD277 r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x sp=%08x  data: %s\n",
    emu.framecount(), reg("r0"), reg("r1"), reg("r2"), r3, reg("r14"), reg("r13"), table.concat(words, " ")))
  -- a few return addresses from the stack, to see the interpreter chain
  local sp = reg("r13")
  local st = {}
  for i = 0, 15 do
    local w = u32(sp + i * 4)
    if w >= 0x02000000 and w < 0x02130000 then st[#st + 1] = string.format("%08x", w) end
  end
  f:write("        stack: " .. table.concat(st, " ") .. "\n")
  f:flush()
end
memory.registerexec(0x0211c43c, on_fade_cmd)

local keys = { "G", "G68", "G6c", "top", "bot", "top_d0", "top_c4", "top_ce", "bot_d0", "bot_c4", "bot_ce",
               "req0", "req1", "req2", "req3", "req4", "req5", "req6", "req7", "bright" }
local last = {}

while true do
  local v = sample()
  local changed = {}
  for _, k in ipairs(keys) do
    if v[k] ~= last[k] then
      changed[#changed + 1] = string.format("%s=%s", k, v[k] and string.format("%08x", v[k]) or "-")
    end
  end
  if #changed > 0 then
    f:write(string.format("%6d  %s\n", emu.framecount(), table.concat(changed, " ")))
    f:flush()
  end
  last = v
  emu.frameadvance()
end
