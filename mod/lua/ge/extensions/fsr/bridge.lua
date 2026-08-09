-- SPDX-License-Identifier: MIT
-- Part of BeamNG FSR Bridge. See LICENSE at the repository root.
--
-- The BeamNG half of the FSR integration.
--
-- What this extension can and cannot do is not a design choice, it is what
-- BeamNG's Lua sandbox permits. `ffi.load` and `package.loadlib` are denied, so
-- this file CANNOT call the FSR DLLs directly and does not pretend to. What it
-- can do, and does:
--
--   * read and write JSON files, which is how it talks to the native bridge
--   * read camera parameters FSR needs and Lua can see
--   * observe game state (paused, loading, replay) for the frame-generation gate
--   * drive the PostFX beacon pass that publishes depth and motion vectors
--   * back the settings UI
--
-- Everything graphics-related happens in the native bridge. See
-- docs/FEASIBILITY.md §1.3 for the sandbox restriction this is built around.

local M = {}

M.dependencies = {}

local PROTOCOL_VERSION = 1
local MOD_VERSION = '0.1.0'

-- The bridge writes its files next to its own DLL, which lives in the BeamNG
-- install directory. Lua cannot read outside the user folder and the game
-- directory, so the install step also mirrors the paths here.
local IPC_DIR = 'beamng_fsr/'
local REQUEST_PATH = IPC_DIR .. 'request.json'
local STATUS_PATH = IPC_DIR .. 'status.json'
local SETTINGS_PATH = 'settings/beamng_fsr_settings.json'

-- Push at ~10 Hz. Nothing here is per-frame data: the render buffers travel
-- GPU-side and every field below changes on the order of seconds. Polling
-- faster would only add file I/O to the frame loop for no benefit.
local PUSH_INTERVAL = 0.1
local STATUS_POLL_INTERVAL = 0.25

local state = {
  sequence = 0,
  pushTimer = 0,
  statusTimer = 0,
  bridgeStatus = nil,
  bridgeSeen = false,
  lastStatusSequence = -1,
  cameraCutFrames = 0,
  settings = nil,
  settingsDirty = false,
  beacon = nil,
}

-- ---------------------------------------------------------------------------
-- Settings
-- ---------------------------------------------------------------------------

-- Mirrors bfsr::Settings in the native half. Kept in sync by the round-trip
-- test in tests/; a mismatch here silently resets a user's configuration.
local function defaultSettings()
  return {
    enabled = false,
    safeMode = false,
    upscaler = 'disabled',
    upscalerVersionId = 'auto',
    quality = 'quality',
    customUpscaleRatio = 1.5,
    sharpeningEnabled = true,
    sharpness = 0.5,
    frameGeneration = 'disabled',
    frameGenVersionId = 'auto',
    frameGenAsyncCompute = true,
    frameGenUiComposition = true,
    antiLag2 = false,
    depthConvention = 'auto',
    depthInfinite = true,
    motionVectorScaleX = 1.0,
    motionVectorScaleY = 1.0,
    motionVectorFlipY = true,
    motionVectorsAreUvSpace = true,
    debugOverlay = false,
    debugView = false,
    benchmarkMode = false,
    logLevel = 'info',
    forceCompatibilityMode = false,
  }
end

local function loadSettings()
  local defaults = defaultSettings()
  local loaded = jsonReadFile(SETTINGS_PATH)
  if type(loaded) ~= 'table' then
    log('I', 'fsr', 'no saved FSR settings found; using defaults')
    return defaults
  end
  -- Merge rather than replace: a settings file written by an older mod version
  -- is missing newer keys, and dropping to defaults for all of them would be a
  -- worse outcome than filling the gaps.
  for key, value in pairs(defaults) do
    if loaded[key] == nil then loaded[key] = value end
  end
  return loaded
end

local function saveSettings()
  if not state.settings then return end
  jsonWriteFile(SETTINGS_PATH, state.settings, true)
end

-- ---------------------------------------------------------------------------
-- Game state for the frame-generation gate
-- ---------------------------------------------------------------------------

-- Each of these is wrapped because the exact API differs across BeamNG
-- versions and an unexpected nil here must not take the extension down with it.
local function safeCall(fn, fallback)
  local ok, result = pcall(fn)
  if ok and result ~= nil then return result end
  return fallback
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
    cameraCut = state.cameraCutFrames > 0,
  }
end

-- FSR needs the vertical FOV in radians plus the near/far planes. BeamNG's
-- camera API reports FOV in degrees.
local function readCameraParams()
  local params = { valid = false, near = 0.1, far = 10000.0, fovVerticalRadians = 1.2 }

  local fovDegrees = safeCall(function() return core_camera.getFovDeg() end, nil)
  local nearPlane = safeCall(function() return core_camera.getNearClip() end, nil)
  local farPlane = safeCall(function() return core_camera.getFarClip() end, nil)

  if fovDegrees and nearPlane and farPlane and fovDegrees > 0 then
    -- BeamNG reports a horizontal FOV; FSR wants vertical. Convert through the
    -- viewport aspect rather than assuming 16:9.
    local width, height = safeCall(function()
      local res = getScreenResolution()
      return res.x, res.y
    end, nil)
    local aspect = 16 / 9
    if width and height and height > 0 then aspect = width / height end

    local horizontalRad = math.rad(fovDegrees)
    params.fovVerticalRadians = 2 * math.atan(math.tan(horizontalRad / 2) / aspect)
    params.near = nearPlane
    params.far = farPlane
    params.valid = true
  end

  return params
end

-- ---------------------------------------------------------------------------
-- IPC
-- ---------------------------------------------------------------------------

local function pushRequest()
  state.sequence = state.sequence + 1

  local request = {
    protocolVersion = PROTOCOL_VERSION,
    -- 64-bit counters travel as strings: JSON numbers are doubles and would
    -- lose precision on a long session.
    sequence = tostring(state.sequence),
    modVersion = MOD_VERSION,
    beamngVersion = safeCall(function() return beamng_versionb or beamng_version end, 'unknown'),
    settings = state.settings,
    gameState = readGameState(),
    camera = readCameraParams(),
    depthMvBeacon = state.beacon and state.beacon:describe() or { valid = false },
    clearSafeMode = false,
  }

  jsonWriteFile(REQUEST_PATH, request, false)
end

local function pollStatus()
  local status = jsonReadFile(STATUS_PATH)
  if type(status) ~= 'table' then return end

  if status.protocolVersion ~= PROTOCOL_VERSION then
    if not state.protocolWarned then
      state.protocolWarned = true
      log('E', 'fsr',
        string.format(
          'the native FSR bridge speaks IPC protocol %s but this mod speaks %d. ' ..
          'Install matching versions of both halves.',
          tostring(status.protocolVersion), PROTOCOL_VERSION))
    end
    return
  end

  local sequence = tonumber(status.sequence or '0') or 0
  if sequence == state.lastStatusSequence then return end
  state.lastStatusSequence = sequence
  state.bridgeStatus = status

  if not state.bridgeSeen then
    state.bridgeSeen = true
    log('I', 'fsr', 'native FSR bridge detected, version ' .. tostring(status.bridgeVersion))
    if status.initFailureReason and status.initFailureReason ~= '' then
      log('W', 'fsr', 'bridge reported: ' .. status.initFailureReason)
    end
  end

  -- Mirror to the UI without requiring it to poll a file itself.
  guihooks.trigger('FsrStatusUpdated', status)
end

-- ---------------------------------------------------------------------------
-- Public API, called by the UI app
-- ---------------------------------------------------------------------------

function M.getSettings()
  return state.settings
end

function M.getStatus()
  if not state.bridgeStatus then
    return {
      bridgeLoaded = false,
      -- The UI shows this verbatim, so it has to say what to do about it.
      initFailureReason =
        'The native FSR bridge is not loaded. FSR needs dxgi.dll, d3d12.dll and ' ..
        'beamng_fsr_bridge.dll next to BeamNG.drive.exe, and BeamNG must be running ' ..
        'the Direct3D 12 renderer. See docs/INSTALL.md.',
    }
  end
  return state.bridgeStatus
end

function M.setSetting(key, value)
  if not state.settings then return end
  if state.settings[key] == nil then
    log('W', 'fsr', 'ignoring unknown FSR setting: ' .. tostring(key))
    return
  end
  state.settings[key] = value
  state.settingsDirty = true
  -- Push immediately so the UI reflects the change on the next status tick
  -- rather than after the regular interval.
  pushRequest()
  saveSettings()
end

function M.applySettings(newSettings)
  if type(newSettings) ~= 'table' then return end
  for key, value in pairs(newSettings) do
    if state.settings[key] ~= nil then state.settings[key] = value end
  end
  state.settingsDirty = true
  pushRequest()
  saveSettings()
end

function M.resetSettings()
  state.settings = defaultSettings()
  state.settingsDirty = true
  pushRequest()
  saveSettings()
  log('I', 'fsr', 'FSR settings reset to defaults')
end

function M.clearSafeMode()
  state.settings.safeMode = false
  saveSettings()
  state.sequence = state.sequence + 1
  local request = {
    protocolVersion = PROTOCOL_VERSION,
    sequence = tostring(state.sequence),
    modVersion = MOD_VERSION,
    settings = state.settings,
    gameState = readGameState(),
    camera = readCameraParams(),
    depthMvBeacon = state.beacon and state.beacon:describe() or { valid = false },
    clearSafeMode = true,
  }
  jsonWriteFile(REQUEST_PATH, request, false)
  log('I', 'fsr', 'safe mode cleared')
end

--- Writes a self-contained report a user can attach to a bug report.
function M.exportDiagnostics()
  local report = {
    generatedAt = os.date('!%Y-%m-%dT%H:%M:%SZ'),
    modVersion = MOD_VERSION,
    protocolVersion = PROTOCOL_VERSION,
    beamngVersion = safeCall(function() return beamng_versionb or beamng_version end, 'unknown'),
    settings = state.settings,
    gameState = readGameState(),
    camera = readCameraParams(),
    beacon = state.beacon and state.beacon:describe() or { valid = false },
    bridge = state.bridgeStatus or { bridgeLoaded = false },
  }
  local path = 'beamng_fsr_diagnostics.json'
  jsonWriteFile(path, report, true)
  log('I', 'fsr', 'diagnostic report written to ' .. path)
  return path
end

-- ---------------------------------------------------------------------------
-- Lifecycle
-- ---------------------------------------------------------------------------

function M.onExtensionLoaded()
  state.settings = loadSettings()
  state.beacon = require('fsr/beacon')
  state.beacon:setup()
  log('I', 'fsr', 'BeamNG FSR mod ' .. MOD_VERSION .. ' loaded')
  pushRequest()
end

function M.onExtensionUnloaded()
  if state.beacon then state.beacon:teardown() end
  saveSettings()
  log('I', 'fsr', 'BeamNG FSR mod unloaded')
end

function M.onUpdate(dtReal)
  if not state.settings then return end

  if state.cameraCutFrames > 0 then
    state.cameraCutFrames = state.cameraCutFrames - 1
  end

  state.pushTimer = state.pushTimer + dtReal
  if state.pushTimer >= PUSH_INTERVAL then
    state.pushTimer = 0
    pushRequest()
  end

  state.statusTimer = state.statusTimer + dtReal
  if state.statusTimer >= STATUS_POLL_INTERVAL then
    state.statusTimer = 0
    pollStatus()
  end
end

-- Anything that breaks temporal continuity must reach the frame-generation
-- gate, or the first frames after it are interpolated against a stale
-- predecessor and visibly smear.
local function markCameraCut(reason)
  state.cameraCutFrames = 4
  log('D', 'fsr', 'camera cut: ' .. tostring(reason))
end

function M.onClientStartMission() markCameraCut('mission start') end
function M.onClientEndMission() markCameraCut('mission end') end
function M.onVehicleSwitched() markCameraCut('vehicle switched') end
function M.onCameraModeChanged() markCameraCut('camera mode changed') end
function M.onSettingsChanged() if state.beacon then state.beacon:refresh() end end

function M.onSerialize()
  return { settings = state.settings, sequence = state.sequence }
end

function M.onDeserialized(data)
  if type(data) == 'table' and type(data.settings) == 'table' then
    state.settings = data.settings
    state.sequence = data.sequence or 0
  end
end

return M
