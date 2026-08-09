-- SPDX-License-Identifier: MIT
-- Part of BeamNG FSR Bridge. See LICENSE at the repository root.
--
-- Auto-loads the upscaling extension. Separate from main.lua so the
-- extension itself stays unloadable and reloadable at runtime without this
-- bootstrap going with it.
local M = {}

function M.onExtensionLoaded()
  extensions.load('fsr_up_main')
  log('I', 'fsr.up', 'AMD FSR Upscaling registered')
end

function M.onExtensionUnloaded()
  extensions.unload('fsr_up_main')
end

return M
