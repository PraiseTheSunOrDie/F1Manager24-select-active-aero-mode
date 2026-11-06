local MOD_NAME = "ActiveAeroMod"
local FALLBACK_MOD_DIR = "Mods/ActiveAeroMod"
local logFilePath = nil

local function log(message)
    local line = "[" .. MOD_NAME .. "] " .. tostring(message)
    print(line)
    if logFilePath then
        local f = io.open(logFilePath, "a")
        if f then
            f:write(line, "\n")
            f:close()
        end
    end
end

local function fileExists(path)
    local f = io.open(path, "rb")
    if f then f:close(); return true end
    return false
end

local function parentDir(path)
    return path and path:match("^(.*)[/\\][^/\\]+$") or nil
end

local function join(dir, leaf)
    if not dir or dir == "" then return leaf end
    local last = dir:sub(-1)
    if last == "/" or last == "\\" then return dir .. leaf end
    return dir .. "\\" .. leaf
end

local function getModDir()
    local source = debug.getinfo(1, "S").source or ""
    if source:sub(1, 1) == "@" then source = source:sub(2) end
    local scriptDir = parentDir(source)
    if not scriptDir then return FALLBACK_MOD_DIR end

    local leaf = scriptDir:match("([^/\\]+)$")
    if leaf and leaf:lower() == "scripts" then
        return parentDir(scriptDir) or FALLBACK_MOD_DIR
    end
    return scriptDir
end

local modDir = getModDir()
logFilePath = join(modDir, "track_detection.log")
local freshLog = io.open(logFilePath, "w")
if freshLog then freshLog:close() end

local nativeModulePath = join(modDir, "ActiveAeroMod.dll")
local currentTrackPath = join(modDir, "current_track.txt")
local currentTrackTempPath = join(modDir, "current_track.tmp")
local nativeModuleLoaded = false
local lastCircuitName = nil
local lastNoCircuitSummary = nil
local lastSelectedComponentSummary = nil
local loggedComponentCandidates = {}
local lastFindFirstCandidatePath = nil

pcall(os.remove, currentTrackPath)

local UNKNOWN_CIRCUIT = "__UNKNOWN__"

local function publishCircuitName(name, reason)
    if type(name) ~= "string" or name == "" then name = UNKNOWN_CIRCUIT end
    if name == lastCircuitName and fileExists(currentTrackPath) then return end

    local f, err = io.open(currentTrackTempPath, "w")
    if not f then
        log("Could not write current circuit marker: " .. tostring(err))
        return
    end
    f:write(name, "\n")
    f:close()

    pcall(os.remove, currentTrackPath)
    local renamed, renameErr = os.rename(currentTrackTempPath, currentTrackPath)
    if not renamed then
        local out, openErr = io.open(currentTrackPath, "w")
        if not out then
            log("Could not publish current circuit marker: " .. tostring(renameErr or openErr))
            return
        end
        out:write(name, "\n")
        out:close()
        pcall(os.remove, currentTrackTempPath)
    end

    lastCircuitName = name
    if name == UNKNOWN_CIRCUIT then
        log("Circuit is not identified; default overtake-assistance distance will be used." ..
            (reason and (" (" .. reason .. ")") or ""))
    else
        log("Current circuit detected: " .. name .. (reason and (" (" .. reason .. ")") or ""))
    end
end

local function extractCircuitName(fullName)
    if type(fullName) ~= "string" then return nil end

    local normalized = fullName:gsub("\\", "/")
    local lower = normalized:lower()
    local markerStart, markerEnd = lower:find("/circuits/", 1, true)
    if not markerStart then
        markerStart, markerEnd = lower:find("circuits/", 1, true)
    end
    if not markerEnd then return nil end

    local nameStart = markerEnd + 1
    local nameEnd = normalized:find("[/%.:]", nameStart)
    if not nameEnd then nameEnd = #normalized + 1 end
    local circuitName = normalized:sub(nameStart, nameEnd - 1)
    if circuitName == "" or not circuitName:match("^[%w_%-]+$") then return nil end
    return circuitName
end

local function getFullName(object)
    local ok, fullName = pcall(function() return object:GetFullName() end)
    if ok and type(fullName) == "string" then return fullName end
    return nil
end

local function inspectTrackComponent(component, reason)
    local fullName = getFullName(component)
    if not fullName then return false end

    local circuitName = extractCircuitName(fullName)
    if circuitName then
        publishCircuitName(circuitName, reason)
        return true
    end

    return false
end

local function refreshCurrentCircuit(reason, scanAll)
    local inspected = 0
    local selectedCircuit = nil
    local selectedPath = nil

    if scanAll ~= false and type(FindAllOf) == "function" then
        local allOk, components = pcall(function() return FindAllOf("RaceSimTrackComponent") end)
        if allOk and type(components) == "table" then
            for _, component in ipairs(components) do
                local validOk, valid = pcall(function() return component:IsValid() end)
                if validOk and valid then
                    inspected = inspected + 1
                    local name = getFullName(component)
                    local circuitName = extractCircuitName(name)
                    if circuitName then
                        selectedCircuit = circuitName
                        selectedPath = name
                    elseif inspected <= 4 and name and not loggedComponentCandidates[name] then
                        loggedComponentCandidates[name] = true
                        log("Track component candidate (no circuit folder parsed): " .. name)
                    end
                end
            end
        elseif not allOk then
            log("FindAllOf(RaceSimTrackComponent) failed: " .. tostring(components))
        end
    end

    if selectedCircuit then
        publishCircuitName(selectedCircuit, (reason or "lookup") .. " / FindAllOf")
        local selectedSummary = selectedCircuit .. "|" .. tostring(selectedPath)
        if selectedSummary ~= lastSelectedComponentSummary then
            log("Selected track component path: " .. tostring(selectedPath))
            lastSelectedComponentSummary = selectedSummary
        end
        lastNoCircuitSummary = nil
        return true
    end

    local ok, component = pcall(function() return FindFirstOf("RaceSimTrackComponent") end)
    if ok and component then
        local validOk, valid = pcall(function() return component:IsValid() end)
        if validOk and valid then
            local found = inspectTrackComponent(component, reason or "FindFirstOf lookup")
            if found then
                lastNoCircuitSummary = nil
                return true
            end
            local name = getFullName(component)
            if name and name ~= lastFindFirstCandidatePath then
                log("FindFirstOf track component path did not contain a /Circuits/<name>/ folder: " .. name)
                lastFindFirstCandidatePath = name
            end
        end
    end

    local summary = "No circuit folder could be extracted from " .. tostring(inspected) ..
        " RaceSimTrackComponent instance(s)."
    if summary ~= lastNoCircuitSummary then
        log(summary)
        lastNoCircuitSummary = summary
    end
    lastSelectedComponentSummary = nil
    return false
end

local automaticHookCount = 0
local actorPathDiagnostics = 0
local firstCircuitPathLogged = false
local actorBeginPlayCount = 0

local function detectCircuitFromActor(actor)
    local current = actor
    for depth = 1, 4 do
        if not current then break end
        local validOk, valid = pcall(function() return current:IsValid() end)
        if not validOk or not valid then break end

        local fullName = getFullName(current)
        if fullName then
            local circuitName = extractCircuitName(fullName)
            if circuitName then
                if not firstCircuitPathLogged then
                    firstCircuitPathLogged = true
                    log("Automatic circuit path found on actor/outer: " .. fullName)
                end
                publishCircuitName(circuitName, "automatic actor BeginPlayPostHook")
                return true
            end

            if actorPathDiagnostics < 8 then
                actorPathDiagnostics = actorPathDiagnostics + 1
                log("BeginPlay actor path sample " .. actorPathDiagnostics .. ": " .. fullName)
            end
        end

        local outerOk, outer = pcall(function() return current:GetOuter() end)
        if not outerOk then break end
        current = outer
    end
    return false
end

if type(RegisterBeginPlayPostHook) == "function" then
    local ok, err = pcall(function()
        RegisterBeginPlayPostHook(function(actor)
            local callbackOk, callbackErr = pcall(function()
                actorBeginPlayCount = actorBeginPlayCount + 1
                local foundOnActor = detectCircuitFromActor(actor)

                if not foundOnActor and (not lastCircuitName or lastCircuitName == UNKNOWN_CIRCUIT) then
                    if actorBeginPlayCount == 8 then
                        refreshCurrentCircuit("automatic BeginPlay component retry", false)
                    elseif actorBeginPlayCount == 64 then
                        refreshCurrentCircuit("automatic delayed component retry", true)
                    end
                end
            end)
            if not callbackOk then
                log("Automatic circuit detection actor callback failed: " .. tostring(callbackErr))
            end
        end)
    end)
    if ok then
        automaticHookCount = automaticHookCount + 1
        log("Automatic circuit detection registered: post-BeginPlay actor path hook.")
    else
        log("Could not register post-BeginPlay actor hook: " .. tostring(err))
    end
else
    log("UE4SS does not expose RegisterBeginPlayPostHook; component lookup and F8 fallback remain available.")
end

if type(RegisterInitGameStatePostHook) == "function" then
    local ok, err = pcall(function()
        RegisterInitGameStatePostHook(function(gameState)
            local callbackOk, callbackErr = pcall(function()
                actorBeginPlayCount = 0
                local stateName = getFullName(gameState)
                if stateName then log("GameState initialized: " .. stateName) end
                local found = refreshCurrentCircuit("automatic game-state initialization")
                if not found then
                    publishCircuitName(UNKNOWN_CIRCUIT, "map transition; waiting for track actors")
                end
            end)
            if not callbackOk then
                log("Automatic lookup after game-state initialization failed: " .. tostring(callbackErr))
            end
        end)
    end)
    if ok then
        automaticHookCount = automaticHookCount + 1
        log("Automatic circuit detection registered: post-game-state initialization fallback.")
    else
        log("Could not register game-state initialization hook: " .. tostring(err))
    end
else
    log("UE4SS does not expose RegisterInitGameStatePostHook; actor-path hook and F8 fallback remain available.")
end

refreshCurrentCircuit("startup lookup")

local periodicDetectionStarted = false
local function startPeriodicCircuitDetection()
    if type(LoopAsync) ~= "function" then
        log("Periodic circuit detection unavailable: UE4SS does not expose LoopAsync; F8 remains available.")
        return
    end
    if type(ExecuteInGameThread) ~= "function" then
        log("Periodic circuit detection unavailable: UE4SS does not expose ExecuteInGameThread; F8 remains available.")
        return
    end

    local scanQueued = false
    local loopOk, loopErr = pcall(function()
        LoopAsync(2000, function()
            if not scanQueued then
                scanQueued = true
                local queueOk, queueErr = pcall(function()
                    ExecuteInGameThread(function()
                        scanQueued = false
                        local scanOk, scanErr = pcall(function()
                            refreshCurrentCircuit("periodic 2-second game-thread scan", true)
                        end)
                        if not scanOk then
                            log("Periodic circuit scan failed: " .. tostring(scanErr))
                        end
                    end)
                end)
                if not queueOk then
                    scanQueued = false
                    log("Could not queue periodic circuit scan on the game thread: " .. tostring(queueErr))
                end
            end
            return false -- continue until UE4SS stops this Lua mod
        end)
    end)

    if loopOk then
        periodicDetectionStarted = true
        log("Periodic circuit detection started (FindAllOf every 2 seconds, executed on the game thread).")
    else
        log("Could not start periodic circuit detection: " .. tostring(loopErr) .. "; F8 remains available.")
    end
end

startPeriodicCircuitDetection()

local f8Ok, f8Err = pcall(function()
    RegisterKeyBind(Key.F8, {}, function()
        refreshCurrentCircuit("F8 manual refresh")
    end)
end)
if f8Ok then
    log("F8 keybind registered as a manual fallback for circuit detection.")
else
    log("Could not register F8 keybind: " .. tostring(f8Err))
end

if automaticHookCount == 0 then
    if periodicDetectionStarted then
        log("No actor/GameState hooks registered; periodic 2-second circuit scanning remains active, with F8 as a manual fallback.")
    else
        log("WARNING: no automatic circuit hooks or periodic scanner registered; use F8 after the circuit has loaded.")
    end
end

local function loadNativeModule()
    local candidates = { nativeModulePath }
    local fallback = join(FALLBACK_MOD_DIR, "ActiveAeroMod.dll")
    if fallback ~= nativeModulePath then candidates[#candidates + 1] = fallback end

    for _, path in ipairs(candidates) do
        if fileExists(path) then
            local loader, err = package.loadlib(path, "*")
            if loader then
                nativeModuleLoaded = true
                log("ActiveAeroMod load requested: " .. path)
                log("Check ActiveAeroMod.log beside the DLL for DRS signature status and whether overtake probability assistance was installed.")
                return
            end
            log("ActiveAeroMod load failed: " .. path .. " | " .. tostring(err))
            return
        end
    end

    log("ActiveAeroMod.dll not found. Expected: " .. nativeModulePath)
end

log("DRS mode and overtake-assist settings are controlled through settings.ini.")

loadNativeModule()

if not nativeModuleLoaded then
    log("Lua controls are loaded, but ActiveAeroMod is not running until ActiveAeroMod.dll loads successfully.")
end
