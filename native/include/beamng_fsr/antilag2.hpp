// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <d3d12.h>

#include "beamng_fsr/result.hpp"

namespace bfsr {

/// AMD Radeon Anti-Lag 2.
///
/// Implemented against AMD's header-only ffx_antilag2_dx12.h, which resolves
/// AmdExtD3DCreateInterface from the installed AMD driver. It is therefore an
/// AMD-GPU-only facility by construction: on other vendors Initialize simply
/// fails and the feature reports itself unavailable rather than pretending.
class AntiLag2 {
public:
    AntiLag2() = default;
    ~AntiLag2();

    AntiLag2(const AntiLag2&) = delete;
    AntiLag2& operator=(const AntiLag2&) = delete;

    Result initialise(ID3D12Device* device);
    void shutdown();
    [[nodiscard]] bool isActive() const noexcept { return active_; }

    /// Call immediately before the game polls input. `maxFps` of 0 means no cap.
    Result update(bool enabled, unsigned maxFps);
    /// Call when the frame's rendering commands have been submitted.
    Result markEndOfFrameRendering();
    /// Tells the driver whether the frame about to be presented is interpolated,
    /// which is what lets Anti-Lag pace correctly alongside frame generation.
    Result setFrameGenFrameType(bool isInterpolated);

private:
    void* context_ = nullptr; ///< AMD::AntiLag2DX12::Context, kept opaque here.
    bool active_ = false;
};

} // namespace bfsr
