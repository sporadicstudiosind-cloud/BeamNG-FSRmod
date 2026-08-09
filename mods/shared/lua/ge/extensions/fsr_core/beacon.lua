-- SPDX-License-Identifier: MIT
-- Part of BeamNG FSR Bridge. See LICENSE at the repository root.
--
-- The depth/motion-vector beacon.
--
-- The problem this solves: a D3D12-level layer sees hundreds of textures a
-- frame and has no way to know which one is the depth buffer and which is the
-- velocity buffer. Guessing by format and size is fragile and breaks whenever
-- BeamNG changes its render targets.
--
-- The approach: instead of the bridge inferring which textures to use, this
-- mod *publishes* them. PostEffect passes copy BeamNG's `#prepass[Depth]` and
-- `#VelocityBuffer[RT0]` into render targets that this mod created, then tell
-- the bridge those targets' exact dimensions and formats over IPC. The bridge
-- then looks for textures it has a description of, rather than inferring them
-- from an engine it knows nothing about.
--
-- That is what makes this survive BeamNG updates: the identifying signature is
-- created here, not derived from BeamNG's internals.
--
-- Why *two* targets and not one packed one: FSR reads depth from a resource's
-- channel 0 and the motion vector from channels 0 and 1 of another. Handing it
-- one packed RGBA texture as both inputs would have it read the motion vector
-- as (depth, velocity.x). The two inputs have to be two resources.
--
-- Everything below uses only documented BeamNG modding APIs:
-- https://documentation.beamng.com/modding/postfx/

local M = {}

-- DXGI_FORMAT_R32G32B32A32_FLOAT.
--
-- Depth requires the full 32-bit range to survive the copy, and this is the
-- one full-precision four-channel format the engine's format list is known to
-- carry. Both beacons use it: identical descriptions are fine because they are
-- told apart by creation order (see `stage` below), and a format that is rare
-- in a normal BeamNG frame is what keeps the bridge's match unambiguous.
--
-- The cost is real and stated plainly: two full-resolution 16-bytes-per-pixel
-- writes plus the reads, every frame. docs/LIMITATIONS.md records it as
-- overhead to measure on hardware, not as a number this project has verified.
local BEACON_DXGI_FORMAT = 2
local BEACON_FORMAT_NAME = 'GFXFormatR32G32B32A32F'

local DEPTH = {
  target = '#fsrDepthBeacon',
  effect = 'FsrDepthBeaconPostEffect',
  shader = 'FsrDepthBeaconShader',
  pixel = '/shaders/fsr/fsrDepthBeaconP.hlsl',
  source = '#prepass[Depth]',
  label = 'fsrDepthBeacon',
  priority = '0.1',
  ordinal = 0,
}

local MOTION = {
  target = '#fsrMotionBeacon',
  effect = 'FsrMotionBeaconPostEffect',
  shader = 'FsrMotionBeaconShader',
  pixel = '/shaders/fsr/fsrMotionBeaconP.hlsl',
  source = '#VelocityBuffer[RT0]',
  label = 'fsrMotionBeacon',
  priority = '0.2',
  ordinal = 1,
}

local BEACON_COUNT = 2

-- Creation order is the beacons' identity, so it is established rather than
-- assumed.
--
-- Both targets have the same description, and the bridge tells them apart by
-- which was created first. Torque allocates a PostEffect's target lazily, on
-- the effect's first render, so enabling both in one frame would leave the
-- order down to the engine's sort — which is undocumented, and getting it
-- backwards would feed FSR depth as motion and produce a broken image rather
-- than a visible failure.
--
-- So they are enabled one frame apart. Depth renders alone on the first frame
-- and allocates its target; motion is enabled for the next. Neither beacon is
-- published to the bridge until both are up, so the bridge never matches
-- against a half-built pair.
local STAGE = {
  IDLE = 0,
  DEPTH_ENABLED = 1,   -- depth is rendering; motion turns on next tick
  READY = 2,
}

local state = {
  created = false,
  stage = STAGE.IDLE,
  width = 0,
  height = 0,
  lastError = nil,
}

local function screenResolution()
  local ok, res = pcall(function() return getScreenResolution() end)
  if ok and res and res.x and res.y and res.x > 0 and res.y > 0 then
    return math.floor(res.x), math.floor(res.y)
  end
  return 0, 0
end

--- Creates one beacon's ShaderData and PostEffect. Left disabled.
local function createOne(spec)
  if scenetree.findObject(spec.effect) then return true end

  local ok, err = pcall(function()
    local shader = createObject('ShaderData')
    shader:setField('DXVertexShaderFile', 0, 'shaders/common/postFx/postFxV.hlsl')
    shader:setField('DXPixelShaderFile', 0, spec.pixel)
    shader:setField('pixVersion', 0, '5.0')
    shader:registerObject(spec.shader)

    local effect = createObject('PostEffect')
    effect:setField('isEnabled', 0, 'false')
    effect:setField('allowReflectPass', 0, 'false')
    -- Run after the prepass so depth and velocity are both populated, and
    -- before any tone mapping that would change what the bridge sees.
    effect:setField('renderTime', 0, 'PFXBeforeBin')
    effect:setField('renderBin', 0, 'AfterPostFX')
    effect:setField('renderPriority', 0, spec.priority)
    effect:setField('shader', 0, spec.shader)
    effect:setField('stateBlock', 0, 'FsrBeaconStateBlock')
    effect:setField('texture', 0, spec.source)
    effect:setField('target', 0, spec.target)
    effect:setField('targetFormat', 0, BEACON_FORMAT_NAME)
    -- Full render resolution: FSR wants depth and motion vectors at the
    -- resolution the scene was rendered at, not a scaled copy.
    effect:setField('targetScale', 0, '1 1')
    effect:registerObject(spec.effect)
  end)

  if not ok then
    state.lastError = tostring(err)
    log('E', 'fsr', 'could not create the ' .. spec.label .. ' PostEffect: ' .. state.lastError)
    return false
  end
  return true
end

local function createStateBlock()
  if scenetree.findObject('FsrBeaconStateBlock') then return true end
  local ok, err = pcall(function()
    local stateBlock = createObject('GFXStateBlockData')
    stateBlock:setField('zDefined', 0, 'true')
    stateBlock:setField('zEnable', 0, 'false')
    stateBlock:setField('zWriteEnable', 0, 'false')
    stateBlock:setField('samplersDefined', 0, 'true')
    -- Point sampling: the beacons are straight copies and any filtering would
    -- corrupt depth values and velocity magnitudes at the edges of the frame.
    stateBlock:setField('samplerStates', 0, 'SamplerClampPoint')
    stateBlock:registerObject('FsrBeaconStateBlock')
  end)
  if not ok then
    state.lastError = tostring(err)
    log('E', 'fsr', 'could not create the FSR beacon state block: ' .. state.lastError)
    return false
  end
  return true
end

local function setEnabled(spec, enabled)
  local effect = scenetree.findObject(spec.effect)
  if not effect then return false end
  local ok, err = pcall(function()
    if enabled then effect:enable() else effect:disable() end
  end)
  if not ok then
    state.lastError = tostring(err)
    log('E', 'fsr', 'could not toggle ' .. spec.label .. ': ' .. state.lastError)
    return false
  end
  return true
end

function M:setup()
  state.width, state.height = screenResolution()

  if not createStateBlock() then return false end
  if not createOne(DEPTH) then return false end
  if not createOne(MOTION) then return false end

  state.created = true
  state.lastError = nil

  -- Depth first, alone. `tick` promotes to READY on the following frame.
  if not setEnabled(DEPTH, true) then return false end
  setEnabled(MOTION, false)
  state.stage = STAGE.DEPTH_ENABLED
  log('I', 'fsr', 'FSR beacons created; staging depth before motion vectors')
  return true
end

--- Advances the staged enable. Call once per frame.
function M:tick()
  if not state.created then return end
  if state.stage == STAGE.DEPTH_ENABLED then
    if setEnabled(MOTION, true) then
      state.stage = STAGE.READY
      log('I', 'fsr', 'FSR beacons ready')
    end
  end
end

function M:teardown()
  setEnabled(DEPTH, false)
  setEnabled(MOTION, false)
  for _, spec in ipairs({ DEPTH, MOTION }) do
    local effect = scenetree.findObject(spec.effect)
    if effect then pcall(function() effect:delete() end) end
    local shader = scenetree.findObject(spec.shader)
    if shader then pcall(function() shader:delete() end) end
  end
  local block = scenetree.findObject('FsrBeaconStateBlock')
  if block then pcall(function() block:delete() end) end
  state.created = false
  state.stage = STAGE.IDLE
  log('I', 'fsr', 'FSR beacons removed')
end

--- Re-reads the resolution after a display change.
---
--- The targets follow the backbuffer automatically via targetScale, but they
--- are re-allocated when they do — which would put both allocations in the same
--- frame and lose the creation order the bridge identifies them by. So a
--- resolution change re-runs the same staging as startup.
function M:refresh()
  local width, height = screenResolution()
  if width == state.width and height == state.height then return end

  log('I', 'fsr', string.format('beacon resolution changed: %dx%d -> %dx%d',
    state.width, state.height, width, height))
  state.width, state.height = width, height

  if not state.created then return end
  setEnabled(MOTION, false)
  state.stage = STAGE.DEPTH_ENABLED
end

--- True once both targets exist and the bridge may match against them.
function M:ready()
  return state.created and state.stage == STAGE.READY
    and state.width > 0 and state.height > 0
end

local function signature(spec, valid)
  return {
    valid = valid,
    width = state.width,
    height = state.height,
    format = BEACON_DXGI_FORMAT,
    expectRenderTarget = true,
    expectUnorderedAccess = false,
    label = spec.label,
    ordinal = spec.ordinal,
    expectedCount = BEACON_COUNT,
  }
end

--- The two signatures the native bridge matches against.
function M:describe()
  local valid = M:ready()
  return {
    depth = signature(DEPTH, valid),
    motion = signature(MOTION, valid),
  }
end

function M:lastError()
  return state.lastError
end

return M
