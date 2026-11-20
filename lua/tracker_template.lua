-- LLM fight tracker: RAM polling plus the ROM's telemetry ring buffer.
-- Generated from tracker_template.lua by gen_tracker.py. Do not edit the
-- generated file: re-run the generator after every ROM build, because symbol
-- addresses move between builds.
--
-- Load in mGBA 0.10+: Tools > Scripting > File > Load script.
-- Start the bridge first (python3 -m bridge.server) so the script can connect.

local ADDR = {
    gMain = {{gMain}},
    gBattleTypeFlags = {{gBattleTypeFlags}},
    gTrainerBattleParameter = {{gTrainerBattleParameter}},
    gBattleOutcome = {{gBattleOutcome}},
    gBattleTurnCounter = {{gBattleTurnCounter}},
    gSaveBlock1Ptr = {{gSaveBlock1Ptr}},
    gSaveBlock3Ptr = {{gSaveBlock3Ptr}},
    gTelemetry = {{gTelemetry}},  -- nil when the ROM was built with TELEMETRY_ENABLED FALSE
    gMailbox = {{gMailbox}},      -- same
}

local MAIN_IN_BATTLE_OFFSET = 0x439 -- struct Main: u8 oamLoadDisabled:1, inBattle:1
local MAIN_IN_BATTLE_MASK = 0x02
local TRAINER_OPPONENT_A_OFFSET = 2   -- packed struct _TrainerBattleParameter
local TRAINER_OPPONENT_B_OFFSET = 17
local SAVEBLOCK1_LOCATION_OFFSET = 4  -- struct SaveBlock1: Coords16 pos, then WarpData location

local PROFILE_MAGIC = {{PROFILE_MAGIC}}
local PROFILE_VERSION = {{PROFILE_VERSION}}
local PROFILE_SIZE = {{PROFILE_SIZE}}
local SAVEBLOCK3_MAX_SIZE = 1624

-- struct TelemetryBuffer: u32 magic, u32 head, then CAPACITY events of 8 bytes
-- (u8 type, u8 arg, u16 a, u16 b, u16 c). See include/telemetry.h.
local TELEMETRY_MAGIC = {{TELEMETRY_MAGIC}}
local TELEMETRY_CAPACITY = {{TELEMETRY_CAPACITY}}
local TELEMETRY_EVENTS_OFFSET = 8
local TELEMETRY_EVENT_SIZE = 8

-- struct Mailbox: u32 magic, then the texts. The bridge sends them already encoded
-- for the game; the script only copies bytes and then sets the magic.
local MAILBOX_MAGIC = {{MAILBOX_MAGIC}}
local MAILBOX_SLOTS = {
    report = {offset = {{MAILBOX_REPORT_OFFSET}}, size = {{MAILBOX_REPORT_SIZE}}},
    champion = {offset = {{MAILBOX_LINE_OFFSET}}, size = {{MAILBOX_LINE_SIZE}}},
}

local BRIDGE_HOST = "127.0.0.1"
local BRIDGE_PORT = 8888
local RECONNECT_FRAMES = 300  -- connect() blocks, so only retry every ~5 seconds
local MAX_QUEUED_EVENTS = 4096
local BUILD_ID = "{{BUILD_ID}}"

-- ---------------------------------------------------------------------------
-- JSON (flat tables of strings, numbers and booleans are all we send)
-- ---------------------------------------------------------------------------

local function jsonValue(value)
    local kind = type(value)
    if kind == "number" or kind == "boolean" then
        return tostring(value)
    elseif value == nil then
        return "null"
    end
    return '"' .. tostring(value):gsub('[%c"\\]', function(c)
        return string.format("\\u%04x", c:byte())
    end) .. '"'
end

local function toJson(event)
    local parts = {}
    for key, value in pairs(event) do
        parts[#parts + 1] = jsonValue(key) .. ":" .. jsonValue(value)
    end
    return "{" .. table.concat(parts, ",") .. "}"
end

-- ---------------------------------------------------------------------------
-- Connection and event queue
-- ---------------------------------------------------------------------------

local sock = nil
local queue = {}
local seq = 0
local dropped = 0
local framesUntilRetry = 0

local function emit(event)
    seq = seq + 1
    event.seq = seq
    event.frame = emu:currentFrame()
    if #queue >= MAX_QUEUED_EVENTS then
        table.remove(queue, 1)
        dropped = dropped + 1
    end
    queue[#queue + 1] = toJson(event) .. "\n"
end

local function tryConnect()
    local newSock, err = socket.connect(BRIDGE_HOST, BRIDGE_PORT)
    if newSock == nil then
        console:log("[tracker] bridge not reachable (" .. tostring(err) .. "), retrying")
        return
    end
    sock = newSock
    console:log("[tracker] connected to bridge")
    emit({type = "hello", build = BUILD_ID, dropped = dropped})
end

local function flush()
    if sock == nil then
        framesUntilRetry = framesUntilRetry - 1
        if framesUntilRetry <= 0 then
            framesUntilRetry = RECONNECT_FRAMES
            tryConnect()
        end
        if sock == nil then
            return
        end
    end
    while #queue > 0 do
        local sent, err = sock:send(queue[1])
        if sent == nil then
            console:log("[tracker] connection lost (" .. tostring(err) .. ")")
            sock = nil
            return
        end
        table.remove(queue, 1)
    end
end

-- ---------------------------------------------------------------------------
-- Game state reads
-- ---------------------------------------------------------------------------

local function read16Unaligned(address)
    return emu:read8(address) | (emu:read8(address + 1) << 8)
end

local function toSigned8(value)
    return value >= 0x80 and value - 0x100 or value
end

local function isInBattle()
    return (emu:read8(ADDR.gMain + MAIN_IN_BATTLE_OFFSET) & MAIN_IN_BATTLE_MASK) ~= 0
end

local function readLocation()
    local saveBlock1 = emu:read32(ADDR.gSaveBlock1Ptr)
    if saveBlock1 == 0 then
        return nil, nil
    end
    local location = saveBlock1 + SAVEBLOCK1_LOCATION_OFFSET
    return toSigned8(emu:read8(location)), toSigned8(emu:read8(location + 1))
end

-- The Player Profile lives inside SaveBlock3, whose layout depends on config,
-- so find it by its magic and version instead of a fixed offset.
local function findProfile()
    local saveBlock3 = emu:read32(ADDR.gSaveBlock3Ptr)
    if saveBlock3 == 0 then
        return nil
    end
    for offset = 0, SAVEBLOCK3_MAX_SIZE - PROFILE_SIZE, 4 do
        local address = saveBlock3 + offset
        if emu:read16(address) == PROFILE_MAGIC and emu:read8(address + 2) == PROFILE_VERSION then
            return address
        end
    end
    return nil
end

local function readProfileHex()
    local address = findProfile()
    if address == nil then
        return nil
    end
    local bytes = {}
    for i = 0, PROFILE_SIZE - 1 do
        bytes[#bytes + 1] = string.format("%02x", emu:read8(address + i))
    end
    return table.concat(bytes)
end

-- ---------------------------------------------------------------------------
-- Telemetry ring buffer
-- ---------------------------------------------------------------------------

local telemetryRead = nil  -- events read so far (the ROM's head when we last looked)

-- Send every event the ROM wrote since the last frame. The ROM never waits for us:
-- if more than CAPACITY were written in between, the oldest were overwritten.
local function pollTelemetry()
    local base = ADDR.gTelemetry
    if base == nil or emu:read32(base) ~= TELEMETRY_MAGIC then
        return
    end
    local head = emu:read32(base + 4)
    if telemetryRead == nil then
        telemetryRead = math.max(0, head - TELEMETRY_CAPACITY)  -- first look: send what the buffer still holds
    elseif head < telemetryRead then
        telemetryRead = head  -- a save state or reset moved the buffer back: those events were already sent
    end
    local lost = 0
    if head - telemetryRead > TELEMETRY_CAPACITY then
        lost = head - telemetryRead - TELEMETRY_CAPACITY
        telemetryRead = head - TELEMETRY_CAPACITY
    end
    for index = telemetryRead, head - 1 do
        local event = base + TELEMETRY_EVENTS_OFFSET + (index % TELEMETRY_CAPACITY) * TELEMETRY_EVENT_SIZE
        emit({
            type = "telemetry",
            index = index,
            kind = emu:read8(event),
            arg = emu:read8(event + 1),
            a = emu:read16(event + 2),
            b = emu:read16(event + 4),
            c = emu:read16(event + 6),
            lost = lost > 0 and lost or nil,
        })
        lost = 0
    end
    telemetryRead = head
end

-- ---------------------------------------------------------------------------
-- Mailbox: texts from the bridge, written into the ROM's gMailbox
-- ---------------------------------------------------------------------------

local inbox = ""         -- received bytes not yet split into lines
local mailboxTexts = {}  -- slot -> hex of the newest text, kept to rewrite after a reset

local function writeMailbox()
    local base = ADDR.gMailbox
    if base == nil or next(mailboxTexts) == nil then
        return
    end
    for slot, hex in pairs(mailboxTexts) do
        local layout = MAILBOX_SLOTS[slot]
        local length = math.min(#hex // 2, layout.size)
        for i = 0, length - 1 do
            emu:write8(base + layout.offset + i, tonumber(hex:sub(2 * i + 1, 2 * i + 2), 16))
        end
    end
    emu:write32(base, MAILBOX_MAGIC)  -- last: the game only reads texts once this is set
end

-- Read whatever the bridge sent without blocking, one "MAILBOX <slot> <hex>" line per text.
local function receive()
    if sock == nil then
        return
    end
    while sock:hasdata() do
        local chunk, err = sock:receive(4096)
        if chunk == nil then
            console:log("[tracker] connection lost (" .. tostring(err) .. ")")
            sock = nil
            inbox = ""
            return
        end
        inbox = inbox .. chunk
    end
    local updated = false
    while true do
        local newline = inbox:find("\n", 1, true)
        if newline == nil then
            break
        end
        local line = inbox:sub(1, newline - 1)
        inbox = inbox:sub(newline + 1)
        local slot, hex = line:match("^MAILBOX (%a+) (%x*)$")
        if slot ~= nil and MAILBOX_SLOTS[slot] ~= nil then
            mailboxTexts[slot] = hex
            updated = true
        end
    end
    if updated then
        writeMailbox()
        console:log("[tracker] mailbox updated")
    end
end

-- ---------------------------------------------------------------------------
-- Per-frame polling
-- ---------------------------------------------------------------------------

local wasInBattle = false
local lastMapGroup, lastMapNum = nil, nil
local lastFrame = -1

local function onFrame()
    local frame = emu:currentFrame()
    if frame < lastFrame then
        emit({type = "rewind", fromFrame = lastFrame})
        writeMailbox()  -- a reset clears RAM and a save state brings back old texts
    end
    lastFrame = frame

    -- Before battle_end, so a battle's last events (the final faint) arrive inside it.
    pollTelemetry()

    local inBattle = isInBattle()
    if inBattle and not wasInBattle then
        local params = ADDR.gTrainerBattleParameter
        emit({
            type = "battle_start",
            battleTypeFlags = emu:read32(ADDR.gBattleTypeFlags),
            opponentA = read16Unaligned(params + TRAINER_OPPONENT_A_OFFSET),
            opponentB = read16Unaligned(params + TRAINER_OPPONENT_B_OFFSET),
            mapGroup = lastMapGroup,
            mapNum = lastMapNum,
            profile = readProfileHex(),
        })
    elseif wasInBattle and not inBattle then
        emit({
            type = "battle_end",
            outcome = emu:read8(ADDR.gBattleOutcome),
            turns = emu:read16(ADDR.gBattleTurnCounter),
            profile = readProfileHex(),
        })
    end
    wasInBattle = inBattle

    if not inBattle then
        local mapGroup, mapNum = readLocation()
        if mapGroup ~= nil and (mapGroup ~= lastMapGroup or mapNum ~= lastMapNum) then
            lastMapGroup, lastMapNum = mapGroup, mapNum
            emit({type = "map_change", mapGroup = mapGroup, mapNum = mapNum})
        end
    end

    flush()
    receive()
    if ADDR.gMailbox ~= nil and next(mailboxTexts) ~= nil and emu:read32(ADDR.gMailbox) ~= MAILBOX_MAGIC then
        writeMailbox()  -- the game cleared its RAM (reset, boot): put the texts back
    end
end

callbacks:add("frame", onFrame)
console:log("[tracker] loaded, build " .. BUILD_ID)
