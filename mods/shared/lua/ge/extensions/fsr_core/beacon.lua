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
-- mod *publishes* them. A PostEffect pass copies BeamNG's `#prepass[Depth]`
-- and `#VelocityBuffer[RT0]` into one render target that this mod created, then
-- tells the bridge that target's exact dimensions and format over IPC. The
-- bridge then looks for a texture it has a description of, rather than
-- inferring one from an engine it knows nothing about.
--
-- That is what makes this survive BeamNG updates: the identifying signature is
-- created here, not derived from BeamNG's internals.
--
-- Everything below uses only documented BeamNG modding APIs:
-- https://documentation.beamng.com/modding/postfx/

local M = {}

-- DXGI_FORMAT_R32G32B32A32_FLOAT.
--
-- Four channels because the pass carries three values that cannot be
-- compressed without losing what FSR needs: device depth, and both components
-- of the screen-space velocity. Depth requires the full 32-bit range to survive
-- the copy, and velocity direction is lost the moment either component is
-- dropped. A 4x32F full-resolution target is also rare in a normal BeamNG
-- frame, which makes the bridge's description match unambiguous in practice.
--
-- The cost is real and stated plainly: one full-resolution 16-bytes-per-pixel
-- write plus the reads, every frame. docs/LIMITATIONS.md records it as measured
-- overhead to confirm on hardware, not as a number this project has verified.
local BEACON_DXGI_FORMAT = 2
local BEACON_FORMAT_NAME = 'GFXFormatR32G32B32A32F'
local BEACON_TARGET_NAME = '#fsrDepthMvBeacon'
local BEACON_EFFECT_NAME = 'FsrBeaconPostEffect'
local BEACON_SHADER_NAME = 'FsrBeaconShader'
local BEACON_LABEL = 'fsrDepthMvBeacon'

local state = {
  created = false,
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

--- Creates the ShaderData, state block and PostEffect.
---
--- Returns false and records why on any failure. A missing beacon is not fatal:
--- the bridge falls back to heuristic identification and reports lower
--- confidence, and if that also fails FSR stays off rather than running on the
--- wrong textures.
local function createEffect()
  if scenetree.findObject(BEACON_EFFECT_NAME) then
    state.created = true
    return true
  end

  local ok, err = pcall(function()
    local shader = createObject('ShaderData')
    shader:setField('DXVertexShaderFile', 0, 'shaders/common/postFx/postFxV.hlsl')
    shader:setField('DXPixelShaderFile', 0, '/shaders/fsr/fsrBeaconP.hlsl')
    shader:setField('pixVersion', 0, '5.0')
    shader:registerObject(BEACON_SHADER_NAME)

    local stateBlock = createObject('GFXStateBlockData')
    stateBlock:setField('zDefined', 0, 'true')
    stateBlock:setField('zEnable', 0, 'false')
    stateBlock:setField('zWriteEnable', 0, 'false')
    stateBlock:setField('samplersDefined', 0, 'true')
    -- Point sampling: the beacon is a straight copy and any filtering would
    -- corrupt depth values at the edges of the frame.
    stateBlock:setField('samplerStates', 0, 'SamplerClampPoint')
    stateBlock:setField('samplerStates', 1, 'SamplerClampPoint')
    stateBlock:registerObject('FsrBeaconStateBlock')

    local effect = createObject('PostEffect')
    effect:setField('isEnabled', 0, 'false')
    effect:setField('allowReflectPass', 0, 'false')
    -- Run after the prepass so depth and velocity are both populated, and
    -- before any tone mapping that would change what the bridge sees.
    effect:setField('renderTime', 0, 'PFXBeforeBin')
    effect:setField('renderBin', 0, 'AfterPostFX')
    effect:setField('renderPriority', 0, '0.1')
    effect:setField('shader', 0, BEACON_SHADER_NAME)
    effect:setField('stateBlock', 0, 'FsrBeaconStateBlock')
    effect:setField('texture', 0, '#prepass[Depth]')
    effect:setField('texture', 1, '#VelocityBuffer[RT0]')
    effect:setField('target', 0, BEACON_TARGET_NAME)
    effect:setField('targetFormat', 0, BEACON_FORMAT_NAME)
    -- Full render resolution: FSR wants depth and motion vectors at the
    -- resolution the scene was rendered at, not a scaled copy.
    effect:setField('targetScale', 0, '1 1')
    effect:registerObject(BEACON_EFFECT_NAME)
  end)

  if not ok then
    state.lastError = tostring(err)
    log('E', 'fsr', 'could not create the FSR beacon PostEffect: ' .. state.lastError)
    return false
  end

  state.created = true
  state.lastError = nil
  log('I', 'fsr', 'FSR beacon PostEffect created')
  return true
end

function M:setup()
  local width, height = screenResolution()
  state.width, state.height = width, height

  if not createEffect() then return false end

  local effect = scenetree.findObject(BEACON_EFFECT_NAME)
  if effect then
    local ok, err = pcall(function() effect:enable() end)
    if not ok then
      state.lastError = tostring(err)
      log('E', 'fsr', 'could not enable the FSR beacon: ' .. state.lastError)
      return false
    end
  end
  return true
end

function M:teardown()
  local effect = scenetree.findObject(BEACON_EFFECT_NAME)
  if effect then
    pcall(function()
      effect:disable()
      effect:delete()
    end)
  end
  for _, name in ipairs({ BEACON_SHADER_NAME, 'FsrBeaconStateBlock' }) do
    local obj = scenetree.findObject(name)
    if obj then pcall(function() obj:delete() end) end
  end
  state.created = false
  log('I', 'fsr', 'FSR beacon removed')
end

--- Re-reads the resolution after a display change. The render target follows
--- the backbuffer automatically via targetScale, but the *description* the
--- bridge is matching against has to be refreshed or it keeps looking for the
--- old size.
function M:refresh()
  local width, height = screenResolution()
  if width ~= state.width or height ~= state.height then
    log('I', 'fsr', string.format('beacon resolution changed: %dx%d -> %dx%d',
      state.width, state.height, width, height))
    state.width, state.height = width, height
  end
end

--- The signature the native bridge matches against.
function M:describe()
  return {
    valid = state.created and state.width > 0 and state.height > 0,
    width = state.width,
    height = state.height,
    format = BEACON_DXGI_FORMAT,
    expectRenderTarget = true,
    expectUnorderedAccess = false,
    label = BEACON_LABEL,
  }
end

function M:lastError()
  return state.lastError
end

return M
