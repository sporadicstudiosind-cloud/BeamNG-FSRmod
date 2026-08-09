// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <d3d12.h>

#include "beamng_fsr/ffx_loader.hpp"
#include "beamng_fsr/plan.hpp"
#include "beamng_fsr/settings.hpp"

namespace bfsr {

/// Per-frame inputs for one upscale dispatch.
///
/// Everything here is either produced by the mod half (the textures and camera
/// parameters) or measured by the bridge (timing). Nothing is invented: when a
/// field cannot be obtained the caller declines to dispatch rather than
/// substituting a plausible-looking value.
struct UpscaleFrameInputs {
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* exposure = nullptr;  ///< Optional 1x1; null selects auto-exposure.
    ID3D12Resource* reactive = nullptr;  ///< Optional.

    Extent2D renderSize;
    Extent2D upscaleSize;

    /// Zero on BeamNG: no projection-jitter hook is exposed to mods. See
    /// docs/FEASIBILITY.md §6.3 — this is the project's headline limitation and
    /// it is surfaced to the user, not hidden here.
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    float motionVectorScaleX = 1.0f;
    float motionVectorScaleY = 1.0f;

    float frameTimeDeltaMs = 16.6f;
    float preExposure = 1.0f;
    bool reset = false;

    float cameraNear = 0.1f;
    float cameraFar = 10000.0f;
    float cameraFovVerticalRadians = 1.2f;
    float viewSpaceToMetersFactor = 1.0f; ///< BeamNG world units are metres.
};

/// Wraps one FSR upscaling context.
///
/// The context is recreated whenever a creation-time property changes
/// (resolution, HDR, depth convention, pinned version), because those are baked
/// into the context by AMD's API. Recreation is explicit and logged so a user
/// seeing a hitch on a settings change knows why.
class FsrUpscaler {
public:
    FsrUpscaler() = default;
    ~FsrUpscaler();

    FsrUpscaler(const FsrUpscaler&) = delete;
    FsrUpscaler& operator=(const FsrUpscaler&) = delete;

    Result create(FfxLoader& loader,
                  ID3D12Device* device,
                  const UpscalePlan& plan,
                  const UpscaleSettings& settings,
                  bool colourIsHdr);
    void destroy();

    [[nodiscard]] bool isValid() const noexcept { return context_ != nullptr; }

    /// True when `plan`/`settings` require tearing the context down and
    /// rebuilding it rather than just dispatching with new parameters.
    [[nodiscard]] bool needsRecreate(const UpscalePlan& plan,
                                     const UpscaleSettings& settings,
                                     bool colourIsHdr) const noexcept;

    Result dispatch(const UpscaleFrameInputs& inputs, const UpscaleSettings& settings);

    /// AMD's own memory-usage query. Used by the planner's VRAM guard so the
    /// figure is measured rather than guessed.
    static Result queryMemoryUsage(FfxLoader& loader,
                                   ID3D12Device* device,
                                   Extent2D maxRenderSize,
                                   Extent2D maxUpscaleSize,
                                   uint32_t createFlags,
                                   uint64_t& outBytes);

    /// AMD's recommended jitter sequence. Unused on BeamNG today (§6.3) but
    /// implemented so the moment a jitter hook exists the correct phase
    /// sequence is already wired up rather than approximated.
    static Result queryJitterOffset(FfxLoader& loader,
                                    int32_t frameIndex,
                                    Extent2D renderSize,
                                    Extent2D displaySize,
                                    float& outX,
                                    float& outY);

    [[nodiscard]] uint32_t createFlags() const noexcept { return createFlags_; }
    [[nodiscard]] const std::string& activeVersionName() const noexcept { return versionName_; }

private:
    FfxLoader* loader_ = nullptr;
    ffxContext context_ = nullptr;
    uint32_t createFlags_ = 0;
    Extent2D maxRenderSize_;
    Extent2D maxUpscaleSize_;
    uint64_t versionId_ = 0;
    std::string versionName_;
    ID3D12Device* device_ = nullptr;
};

} // namespace bfsr
