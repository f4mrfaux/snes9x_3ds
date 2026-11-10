-- Plays a SNES movie in BizHawk and samples sprites and screenshots at a fixed interval, for any game.
-- Output goes to $S3RE_ROOT/tas/batch/<movie name>/ (S3RE_ROOT defaults to %USERPROFILE%/s3re) and stays
-- outside any repository.
local name = movie.filename():match("([^/\\]+)%.bk2$")
local root = (os.getenv("S3RE_ROOT") or ((os.getenv("USERPROFILE") or ".") .. "/s3re")):gsub("\\", "/")
local out = root .. "/tas/batch/" .. name .. "/"
local SPRITES_EVERY, SHOT_EVERY = 300, 600

local domains = {}
local info = io.open(out .. "info.txt", "w")
info:write("system=", tostring(emu.getsystemid()), " mode=", tostring(movie.mode()),
    " length=", tostring(movie.length()), " core=", tostring(movie.getheader()["Core"]), "\n")
for _, d in pairs(memory.getmemorydomainlist()) do
    domains[d] = true
    info:write("domain ", tostring(d), " size=", tostring(memory.getmemorydomainsize(d)), "\n")
end
info:close()

-- BSNESv115+ has decoded sprites, the older core has raw OAM.
local domain, size
if domains["OBJECTS"] then domain, size = "OBJECTS", 1280
elseif domains["OAM"] then domain, size = "OAM", 544
end

local sprites = io.open(out .. "sprites.csv", "w")
sprites:write("frame,domain,hex\n")

client.SetSoundOn(false)
client.speedmode(6400)
client.invisibleemulation(true)

local function shot(frame)
    client.invisibleemulation(false)
    emu.frameadvance()
    client.screenshot(out .. "shot_" .. emu.framecount() .. ".png")
    client.invisibleemulation(true)
end

local total = movie.length()
while emu.framecount() < total - 2 do
    emu.frameadvance()
    local frame = emu.framecount()
    if domain and frame % SPRITES_EVERY == 0 then
        local raw = {}
        for i = 0, size - 1 do raw[#raw + 1] = string.format("%02X", memory.readbyte(i, domain)) end
        sprites:write(frame, ",", domain, ",", table.concat(raw), "\n")
        sprites:flush()
    end
    if frame % SHOT_EVERY == 0 then shot(frame) end
end

shot(emu.framecount())
local done = io.open(out .. "done.txt", "w")
done:write("frame=", emu.framecount(), " total=", total, "\n")
done:close()
sprites:close()
client.exit()
