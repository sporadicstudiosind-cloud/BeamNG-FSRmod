// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <d3d12.h>
#include <dxgi1_5.h> // IDXGISwapChain4, which AMD's wrap descriptor requires

#include "beamng_fsr/ffx_loader.hpp"
#include "beamng_fsr/plan.hpp"
#include "beamng_fsr/settings.hpp"

// Needed in the header for ffxDispatchDescFrameGeneration, which appears in the
// signature of the callback AMD's swapchain invokes.
#include <ffx_framegeneration.h>

namespace bfsr {

struct FrameGenPrepareInputs {
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;

    Extent2D renderSize;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float motionVectorScaleX = 1.0f;
    float motionVectorScaleY = 1.0f;

    float frameTimeDeltaMs = 16.6f;
    bool reset = false;

    float cameraNear = 0.1f;
    float cameraFar = 10000.0f;
    float cameraFovVerticalRadians = 1.2f;
    float viewSpaceToMetersFactor = 1.0f;

    /// Camera basis in world space. FSR's ML frame generation uses these to
    /// reason about camera motion; they are supplied by the mod half over IPC.
    /// `cameraBasisValid` stays false until real values arrive — the prepare
    /// dispatch then omits them rather than sending an identity basis that
    /// would describe motion the camera is not making.
    bool cameraBasisValid = false;
    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float cameraUp[3] = {0.0f, 1.0f, 0.0f};
    float cameraRight[3] = {1.0f, 0.0f, 0.0f};
    float cameraForward[3] = {0.0f, 0.0f, 1.0f};
};

/// Owns the FSR frame-generation context *and* the frame-interpolation
/// swapchain that presents its output.
///
/// The swapchain is the part that makes frame generation real rather than
/// theoretical: AMD's `FrameInterpolationSwapChain` replaces the game's
/// IDXGISwapChain, schedules the interpolated frame between two rendered ones,
/// and paces both. This class wraps the game's existing swapchain in it — which
/// is the one integration point a DXGI-level layer can legitimately reach.
class FsrFrameGenerator {
public:
    FsrFrameGenerator() = default;
    ~FsrFrameGenerator();

    FsrFrameGenerator(const FsrFrameGenerator&) = delete;
    FsrFrameGenerator& operator=(const FsrFrameGenerator&) = delete;

    /// Wraps `inOutSwapChain` in AMD's frame-interpolation swapchain.
    ///
    /// On success the caller must hand the *replaced* pointer back to the game:
    /// every subsequent Present must go through it or no frames are generated.
    /// On failure the original swapchain is left untouched and usable, so a
    /// frame-generation failure degrades to ordinary presentation.
    Result wrapSwapChain(FfxLoader& loader,
                         IDXGISwapChain4** inOutSwapChain,
                         ID3D12CommandQueue* gameQueue);

    Result createContext(FfxLoader& loader,
                         ID3D12Device* device,
                         const ResolvedPlan& plan,
                         const Settings& settings,
                         DXGI_FORMAT backBufferFormat);

    void destroyContext();
    /// Drains queued presents before releasing the swapchain. Skipping this is
    /// a reliable way to crash on shutdown or on a resolution change.
    void unwrapSwapChain();

    [[nodiscard]] bool hasContext() const noexcept { return context_ != nullptr; }
    [[nodiscard]] bool hasWrappedSwapChain() const noexcept { return swapChainContext_ != nullptr; }

    [[nodiscard]] bool needsRecreate(const ResolvedPlan& plan,
                                     const Settings& settings,
                                     DXGI_FORMAT backBufferFormat) const noexcept;

    /// Records the motion/depth preparation for frame `frameId`.
    Result prepare(const FrameGenPrepareInputs& inputs, const Settings& settings, uint64_t frameId);

    /// Enables or disables interpolation for the coming present. Called every
    /// frame — the frame-generation gate decides `enabled`.
    Result configure(bool enabled, const Settings& settings, uint64_t frameId);

    /// Hands the swapchain a texture containing the UI so it can composite it
    /// *after* interpolation. Without this, UI drawn into the backbuffer gets
    /// interpolated too and smears. Pass nullptr to clear.
    Result registerUiResource(ID3D12Resource* uiTexture, bool premultipliedAlpha);

    static Result queryMemoryUsage(FfxLoader& loader,
                                   ID3D12Device* device,
                                   Extent2D maxRenderSize,
                                   Extent2D displaySize,
                                   uint32_t createFlags,
                                   uint32_t dispatchFlags,
                                   DXGI_FORMAT backBufferFormat,
                                   uint64_t& outBytes);

    [[nodiscard]] const std::string& activeVersionName() const noexcept { return versionName_; }
    [[nodiscard]] uint64_t generatedFrameCount() const noexcept { return generatedFrames_; }

private:
    /// Invoked by AMD's swapchain, on its own thread, when it needs the
    /// interpolated frame produced.
    static ffxReturnCode_t dispatchCallback(ffxDispatchDescFrameGeneration* params, void* userCtx);

    FfxLoader* loader_ = nullptr;
    ffxContext context_ = nullptr;
    ffxContext swapChainContext_ = nullptr;
    IDXGISwapChain4* wrappedSwapChain_ = nullptr;

    uint32_t createFlags_ = 0;
    Extent2D displaySize_;
    Extent2D maxRenderSize_;
    DXGI_FORMAT backBufferFormat_ = DXGI_FORMAT_UNKNOWN;
    uint64_t versionId_ = 0;
    std::string versionName_;
    uint64_t generatedFrames_ = 0;
    bool lastEnabled_ = false;
};

} // namespace bfsr
