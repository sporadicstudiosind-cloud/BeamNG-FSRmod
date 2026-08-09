-- SPDX-License-Identifier: MIT
-- Part of BeamNG FSR Bridge. See LICENSE at the repository root.
--
-- Auto-loads the frame generation extension. Separate from main.lua so the
-- extension itself stays unloadable and reloadable at runtime without this
-- bootstrap going with it.
local M = {}

function M.onExtensionLoaded()
  extensions.load('fsr_fg_main')
  log('I', 'fsr.fg', 'AMD FSR Frame Generation registered')
end

function M.onExtensionUnloaded()
  extensions.unload('fsr_fg_main')
end

return M
