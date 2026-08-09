-- SPDX-License-Identifier: MIT
-- Part of BeamNG FSR Bridge. See LICENSE at the repository root.
--
-- Auto-loads the FSR extension when the mod is enabled. Kept separate from
-- bridge.lua so the extension itself stays unloadable and reloadable at runtime
-- without this bootstrap being torn down with it.
local M = {}

function M.onExtensionLoaded()
  extensions.load('fsr_bridge')
  log('I', 'fsr', 'FSR extension registered')
end

function M.onExtensionUnloaded()
  extensions.unload('fsr_bridge')
end

return M
