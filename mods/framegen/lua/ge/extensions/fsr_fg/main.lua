-- SPDX-License-Identifier: MIT
-- Part of BeamNG FSR Bridge. See LICENSE at the repository root.
--
-- The BeamNG half of the AMD FSR Frame Generation product.
--
-- Self-contained: it does not know or care whether the upscaling product is
-- also installed. It owns one settings document and one IPC channel, and shares
-- only the beacon (reference-counted in fsr_core/common) and the shader that
-- feeds it.

local core = require('fsr_core/common')

local M = {}

local MODULE_ID = 'framegen'
local SETTINGS_PATH = 'settings/beamng_fsr_framegen.json'

local state = {
  sequence = 0,
  pushTimer = 0,
  statusTimer = 0,
  lastStatusSequence = -1,
  status = nil,
  settings = nil,
  beacon = nil,
  protocolWarned = false,
}

--- Mirrors bfsr::FrameGenSettings. Kept in step with the native struct; a
--- mismatch silently resets a user's configuration.
local function defaultSettings()
  return {
    enabled = false,
    safeMode = false,
    implementation = 'auto',
    versionId = 'auto',
    asyncCompute = true,
    uiComposition = true,
    antiLag2 = false,
    antiLagMaxFps = 0,
    calibration = core.defaultCalibration(),
    diagnostics = core.defaultDiagnostics(),
  }
end

local function push(clearSafeMode)
  state.sequence = state.sequence + 1
  core.writeRequest(MODULE_ID, state.sequence, state.settings, state.beacon, clearSafeMode)
end

local function poll()
  local status, warning, sequence = core.readStatus(MODULE_ID, state.lastStatusSequence)
  if warning and not state.protocolWarned then
    state.protocolWarned = true
    log('E', 'fsr.fg', warning)
    return
  end
  if not status then return end

  state.lastStatusSequence = sequence
  state.status = status
  guihooks.trigger('FsrFrameGenStatus', status)
end

-- --- API used by the UI app -----------------------------------------------

function M.getSettings() return state.settings end

function M.getStatus()
  return state.status or core.missingBridgeStatus('frame generation')
end

function M.setSetting(path, value)
  -- `path` may be nested, e.g. "calibration.motionVectorScaleX".
  local target = state.settings
  local keys = {}
  for key in string.gmatch(path, '[^%.]+') do keys[#keys + 1] = key end
  for i = 1, #keys - 1 do
    target = target[keys[i]]
    if type(target) ~= 'table' then
      log('W', 'fsr.fg', 'ignoring unknown setting path: ' .. tostring(path))
      return
    end
  end
  local leaf = keys[#keys]
  if target[leaf] == nil then
    log('W', 'fsr.fg', 'ignoring unknown setting: ' .. tostring(path))
    return
  end
  target[leaf] = value
  push(false)
  core.saveSettings(SETTINGS_PATH, state.settings)
end

function M.resetSettings()
  state.settings = defaultSettings()
  push(false)
  core.saveSettings(SETTINGS_PATH, state.settings)
  log('I', 'fsr.fg', 'frame generation settings reset to defaults')
end

function M.clearSafeMode()
  state.settings.safeMode = false
  core.saveSettings(SETTINGS_PATH, state.settings)
  push(true)
  log('I', 'fsr.fg', 'safe mode cleared')
end

function M.exportDiagnostics()
  local path = 'beamng_fsr_framegen_diagnostics.json'
  jsonWriteFile(path, {
    generatedAt = os.date('!%Y-%m-%dT%H:%M:%SZ'),
    product = 'AMD FSR Frame Generation',
    modVersion = core.VERSION,
    protocolVersion = core.PROTOCOL_VERSION,
    beamngVersion = core.beamngVersion(),
    settings = state.settings,
    shared = core.sharedFrameState(state.beacon),
    bridge = state.status or { moduleInstalled = false },
  }, true)
  log('I', 'fsr.fg', 'diagnostic report written to ' .. path)
  return path
end

-- --- lifecycle -------------------------------------------------------------

function M.onExtensionLoaded()
  state.settings = core.loadSettings(SETTINGS_PATH, defaultSettings())
  state.beacon = core.acquireBeacon()
  log('I', 'fsr.fg', 'AMD FSR Frame Generation ' .. core.VERSION .. ' loaded')
  push(false)
end

function M.onExtensionUnloaded()
  core.saveSettings(SETTINGS_PATH, state.settings)
  core.releaseBeacon()
  state.beacon = nil
  log('I', 'fsr.fg', 'AMD FSR Frame Generation unloaded')
end

function M.onUpdate(dtReal)
  if not state.settings then return end
  core.tickCameraCut()

  state.pushTimer = state.pushTimer + dtReal
  if state.pushTimer >= core.PUSH_INTERVAL then
    state.pushTimer = 0
    push(false)
  end

  state.statusTimer = state.statusTimer + dtReal
  if state.statusTimer >= core.STATUS_POLL_INTERVAL then
    state.statusTimer = 0
    poll()
  end
end

function M.onClientStartMission() core.markCameraCut('mission start') end
function M.onClientEndMission() core.markCameraCut('mission end') end
function M.onVehicleSwitched() core.markCameraCut('vehicle switched') end
function M.onCameraModeChanged() core.markCameraCut('camera mode changed') end
function M.onSettingsChanged() core.refreshBeacon() end

function M.onSerialize() return { settings = state.settings, sequence = state.sequence } end

function M.onDeserialized(data)
  if type(data) == 'table' and type(data.settings) == 'table' then
    state.settings = data.settings
    state.sequence = data.sequence or 0
  end
end

return M
