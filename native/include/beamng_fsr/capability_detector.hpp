// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <d3d12.h>

#include "beamng_fsr/capabilities.hpp"
#include "beamng_fsr/result.hpp"

namespace bfsr {

/// Fills `out` from the adapter actually backing `device`, matched by LUID.
Result detectGpu(ID3D12Device* device, GpuInfo& out);

/// Fills `out` with OS version, shader model and feature level.
Result detectSystem(ID3D12Device* device, SystemInfo& out);

/// Writes the standard startup block described in docs/TROUBLESHOOTING.md.
void logEnvironment(const GpuInfo& gpu, const SystemInfo& sys, const CapabilitySet& caps);

} // namespace bfsr
