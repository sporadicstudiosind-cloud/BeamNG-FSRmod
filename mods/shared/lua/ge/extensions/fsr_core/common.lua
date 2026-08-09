-- SPDX-License-Identifier: MIT
-- Part of BeamNG FSR Bridge. See LICENSE at the repository root.
--
-- Shared core for both FSR products.
--
-- This file is byte-identical in the frame generation and the upscaling mod
-- ZIPs. BeamNG mounts mod archives as a virtual filesystem overlay, so when
-- both products are installed one copy simply shadows the other — harmless,
-- because they are the same file. Everything in here is written to be safe
-- under that: the beacon is reference-counted so whichever product loads first
-- creates it and whichever unloads last removes it.
--
-- What this cannot do is not a design choice. BeamNG's Lua sandbox denies
-- `ffi.load` and `package.loadlib`, so nothing here calls the FSR DLLs. It
-- gathers what Lua *can* see — game state, camera parameters, and the PostFX
-- beacon that publishes depth and motion vectors — and hands it to the native
-- bridge over files.

local M = {}

M.PROTOCOL_VERSION = 2
M.VERSION = '0.2.0'

-- The bridge writes its files beside its own DLL, in the BeamNG install
-- directory. Lua reaches them through the game-directory mount.
M.IPC_DIR = 'beamng_fsr/'

-- ~10 Hz. Nothing latency-critical travels this way: the render buffers are
-- GPU-side and every field below changes on the order of seconds. Polling
-- faster would only add file I/O to the frame loop.
M.PUSH_INTERVAL = 0.1
M.STATUS_POLL_INTERVAL = 0.25

-- ---------------------------------------------------------------------------
-- Safe accessors
-- ---------------------------------------------------------------------------

--- Every BeamNG API touched here is wrapped: the exact call differs across
--- versions, and an unexpected nil must not take the extension down with it.
local function safeCall(fn, fallback)
  local ok, result = pcall(fn)
  if ok and result ~= nil then return result end
  return fallback
end
M.safeCall = safeCall

function M.beamngVersion()
  return safeCall(function() return beamng_versionb or beamng_version end, 'unknown')
end

-- ---------------------------------------------------------------------------
-- Shared frame state
-- ---------------------------------------------------------------------------

local cameraCutFrames = 0

--- Anything that breaks temporal continuity has to reach the native side, or
--- the first frames after it are interpolated against a stale predecessor and
--- visibly smear.
function M.markCameraCut(reason)
  cameraCutFrames = 4
  log('D', 'fsr', 'camera cut: ' .. tostring(reason))
end

function M.tickCameraCut()
  if cameraCutFrames > 0 then cameraCutFrames = cameraCutFrames - 1 end
end

local function readGameState()
  return {
    paused = safeCall(function() return simTimeAuthority.getPause() end, false),
    loading = safeCall(function() return core_gamestate.state == nil end, false),
    uiFocused = safeCall(function()
      return core_gamestate.state ~= nil and core_gamestate.state.appLayout == 'menu'
    end, false),
    photoMode = safeCall(function()
      return core_camera.getActiveCamName() == 'photoCam'
    end, false),
    replay = safeCall(function()
      return core_replay ~= nil and core_replay.getState().state ~= 'inactive'
    end, false),
    cameraCut = cameraCutFrames > 0,
  }
end

--- FSR wants the vertical field of view in radians plus the near and far
--- planes. BeamNG reports a horizontal FOV in degrees, so it is converted
--- through the real viewport aspect rather than an assumed 16:9.
local function readCamera()
  local params = { valid = false, near = 0.1, far = 10000.0, fovVerticalRadians = 1.2 }

  local fovDegrees = safeCall(function() return core_camera.getFovDeg() end, nil)
  local nearPlane = safeCall(function() return core_camera.getNearClip() end, nil)
  local farPlane = safeCall(function() return core_camera.getFarClip() end, nil)
  if not (fovDegrees and nearPlane and farPlane and fovDegrees > 0) then return params end

  local aspect = 16 / 9
  local res = safeCall(function() return getScreenResolution() end, nil)
  if res and res.x and res.y and res.y > 0 then aspect = res.x / res.y end

  local horizontalRad = math.rad(fovDegrees)
  params.fovVerticalRadians = 2 * math.atan(math.tan(horizontalRad / 2) / aspect)
  params.near = nearPlane
  params.far = farPlane
  params.valid = true
  return params
end

--- The block both products send. Identical content on purpose: it is what lets
--- either run without the other's mod half being installed.
function M.sharedFrameState(beacon)
  return {
    gameState = readGameState(),
    camera = readCamera(),
    beacon = beacon and beacon:describe() or { valid = false },
  }
end

-- ---------------------------------------------------------------------------
-- Beacon reference counting
-- ---------------------------------------------------------------------------

local beaconModule = nil
local beaconRefs = 0

--- Both products need the same depth/motion-vector beacon, and creating it
--- twice would leave a duplicate PostEffect running every frame. First caller
--- creates it, last caller removes it.
function M.acquireBeacon()
  if beaconRefs == 0 then
    beaconModule = require('fsr_core/beacon')
    beaconModule:setup()
  end
  beaconRefs = beaconRefs + 1
  return beaconModule
end

function M.releaseBeacon()
  beaconRefs = math.max(0, beaconRefs - 1)
  if beaconRefs == 0 and beaconModule then
    beaconModule:teardown()
    beaconModule = nil
  end
end

function M.beacon()
  return beaconModule
end

function M.refreshBeacon()
  if beaconModule then beaconModule:refresh() end
end

-- ---------------------------------------------------------------------------
-- IPC
-- ---------------------------------------------------------------------------

--- Writes one request document. `moduleId` is 'framegen' or 'upscaling'; each
--- product owns its own pair of files so neither can block or confuse the other.
function M.writeRequest(moduleId, sequence, settings, beacon, clearSafeMode)
  jsonWriteFile(M.IPC_DIR .. moduleId .. '.request.json', {
    protocolVersion = M.PROTOCOL_VERSION,
    module = moduleId,
    -- 64-bit counters travel as strings: JSON numbers are doubles and would
    -- lose precision over a long session.
    sequence = tostring(sequence),
    modVersion = M.VERSION,
    beamngVersion = M.beamngVersion(),
    settings = settings,
    shared = M.sharedFrameState(beacon),
    clearSafeMode = clearSafeMode and true or false,
  }, false)
end

--- Reads one status document. Returns nil when absent, unreadable, unchanged,
--- or written by a mismatched protocol version.
function M.readStatus(moduleId, lastSequence)
  local status = jsonReadFile(M.IPC_DIR .. moduleId .. '.status.json')
  if type(status) ~= 'table' then return nil end

  if status.protocolVersion ~= M.PROTOCOL_VERSION then
    return nil, string.format(
      'the native FSR bridge speaks IPC protocol %s but this mod speaks %d. ' ..
      'Install matching versions of both halves.',
      tostring(status.protocolVersion), M.PROTOCOL_VERSION)
  end

  local sequence = tonumber(status.sequence or '0') or 0
  if sequence == lastSequence then return nil end
  return status, nil, sequence
end

--- Shown verbatim in the UI when no status document exists at all.
function M.missingBridgeStatus(productName)
  return {
    moduleInstalled = false,
    common = {
      bridgeLoaded = false,
      initFailureReason =
        'The native ' .. productName .. ' bridge is not loaded. It needs dxgi.dll, ' ..
        'd3d12.dll and beamng_fsr_bridge.dll next to BeamNG.drive.exe, plus this ' ..
        "product's module manifest, and BeamNG must be running the Direct3D 12 " ..
        'renderer. See docs/INSTALL.md.',
    },
  }
end

-- ---------------------------------------------------------------------------
-- Settings persistence
-- ---------------------------------------------------------------------------

--- Merges a saved document over defaults rather than replacing them: a file
--- written by an older version is missing newer keys, and dropping to defaults
--- for all of them is a worse outcome than filling the gaps.
function M.loadSettings(path, defaults)
  local loaded = jsonReadFile(path)
  if type(loaded) ~= 'table' then return defaults end

  local function merge(target, source)
    for key, value in pairs(target) do
      if source[key] == nil then
        source[key] = value
      elseif type(value) == 'table' and type(source[key]) == 'table' then
        merge(value, source[key])
      end
    end
  end
  merge(defaults, loaded)
  return loaded
end

function M.saveSettings(path, settings)
  jsonWriteFile(path, settings, true)
end

--- Calibration and diagnostics blocks are shared between the products by value.
--- Both UIs expose the same controls and write the same numbers, so when both
--- are installed they agree; when only one is, it is self-sufficient.
function M.defaultCalibration()
  return {
    depthConvention = 'auto',
    depthInfinite = true,
    motionVectorScaleX = 1.0,
    motionVectorScaleY = 1.0,
    motionVectorFlipY = true,
    motionVectorsAreUvSpace = true,
  }
end

function M.defaultDiagnostics()
  return {
    overlay = false,
    debugView = false,
    benchmarkMode = false,
    logLevel = 'info',
    forceCompatibilityMode = false,
  }
end

return M
