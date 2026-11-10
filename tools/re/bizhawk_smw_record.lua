-- Plays a SNES movie in BizHawk and logs SMW scene changes, sprites and screenshots.
-- Output goes to $S3RE_ROOT/tas/biz/ (S3RE_ROOT defaults to %USERPROFILE%/s3re), outside any repository.
local root = (os.getenv("S3RE_ROOT") or ((os.getenv("USERPROFILE") or ".") .. "/s3re")):gsub("\\", "/")
local out = root .. "/tas/biz/"
local log = io.open(out .. "progress.csv", "w")
local oam = io.open(out .. "oam.csv", "w")
local objects = io.open(out .. "objects.csv", "w")
local info = io.open(out .. "info.txt", "w")

info:write("core=", tostring(emu.getsystemid()), " movie_loaded=", tostring(movie.isloaded()),
    " mode=", tostring(movie.mode()), " length=", tostring(movie.length()), "\n")
for _, d in pairs(memory.getmemorydomainlist()) do
    info:write("domain ", tostring(d), " size=", tostring(memory.getmemorydomainsize(d)), "\n")
end
info:close()

client.SetSoundOn(false)
client.speedmode(6400)
client.invisibleemulation(true)

log:write("frame,mode,level,lives,exits\n")
oam:write("frame,mode,level,oam,x,y,tile,prop\n")

local function dump_oam(frame, mode, level)
    -- SMW's own sprite table in WRAM, then BizHawk's decoded per-sprite records for other games.
    for i = 0, 127 do
        local y = memory.readbyte(0x200 + i * 4 + 1, "WRAM")
        if y < 224 then
            oam:write(frame, ",", mode, ",", level, ",", i, ",", memory.readbyte(0x200 + i * 4, "WRAM"), ",", y, ",",
                memory.readbyte(0x200 + i * 4 + 2, "WRAM"), ",", memory.readbyte(0x200 + i * 4 + 3, "WRAM"), "\n")
        end
    end
    local raw = {}
    for i = 0, 1279 do raw[#raw + 1] = string.format("%02X", memory.readbyte(i, "OBJECTS")) end
    objects:write(frame, ",", table.concat(raw), "\n")
end

local last, seen, pending = nil, {}, -1
local total = movie.length()
while emu.framecount() < total do
    emu.frameadvance()
    local frame = emu.framecount()
    local mode = memory.readbyte(0x100, "WRAM")
    local level = memory.readbyte(0x13BF, "WRAM")
    local lives = memory.readbyte(0xDBE, "WRAM")
    local exits = memory.readbyte(0x1F2E, "WRAM")
    local key = mode .. "," .. level .. "," .. lives .. "," .. exits
    if key ~= last then
        log:write(frame, ",", key, "\n")
        log:flush()
        last = key
    end
    -- First stable frame of each map or level scene.
    local scene = mode .. "," .. level
    if (mode == 0x0E or mode == 0x14) and not seen[scene] then
        seen[scene] = true
        pending = frame + 90
    end
    if frame == pending then
        client.invisibleemulation(false)
        emu.frameadvance()
        frame = emu.framecount()
        client.screenshot(out .. "shot_" .. frame .. "_m" .. mode .. "_l" .. level .. ".png")
        client.invisibleemulation(true)
        dump_oam(frame, memory.readbyte(0x100, "WRAM"), memory.readbyte(0x13BF, "WRAM"))
        oam:flush()
    end
end

log:write("END,", emu.framecount(), ",", total, ",", last or "", "\n")
log:close()
oam:close()
objects:close()
client.exit()
