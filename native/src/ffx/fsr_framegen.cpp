// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/fsr_framegen.hpp"

#include "beamng_fsr/logging.hpp"

#include <ffx_api_dx12.h>
#include <ffx_api_framegeneration_dx12.h>
#include <ffx_framegeneration.h>

namespace bfsr {
namespace {

uint32_t buildFrameGenCreateFlags(const FrameGenSettings& settings) {
    uint32_t flags = 0;
    if (settings.asyncCompute) {
        flags |= FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    }
    const bool reversedZ = settings.calibration.depthConvention != DepthConvention::StandardZ;
    if (reversedZ) flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (settings.calibration.depthInfinite) flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    if (settings.diagnostics.logLevel == "trace" || settings.diagnostics.logLevel == "debug") {
        flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    }
    return flags;
}

uint32_t buildFrameGenDispatchFlags(const FrameGenSettings& settings) {
    uint32_t flags = 0;
    if (settings.diagnostics.debugView) flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;
    return flags;
}


} // namespace

FsrFrameGenerator::~FsrFrameGenerator() {
    destroyContext();
    unwrapSwapChain();
}

ffxReturnCode_t FsrFrameGenerator::dispatchCallback(ffxDispatchDescFrameGeneration* params,
                                                    void* userCtx) {
    auto* self = static_cast<FsrFrameGenerator*>(userCtx);
    if (!self || !self->loader_ || !self->context_ || !params) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    // Runs on AMD's presentation thread. Keep it to exactly one forwarding
    // call: anything that can block or allocate here shows up directly as
    // frame-pacing jitter.
    const ffxReturnCode_t rc = self->loader_->fn().Dispatch(&self->context_, &params->header);
    if (rc == FFX_API_RETURN_OK) {
        self->generatedFrames_ += params->numGeneratedFrames;
        // Observed, not requested: the swapchain decides this and hands it to
        // us. Recording it is how the UI can report the real multiplier
        // instead of claiming one.
        self->observedGeneratedPerFrame_ = params->numGeneratedFrames;
    }
    return rc;
}

Result FsrFrameGenerator::wrapSwapChain(FfxLoader& loader,
                                        IDXGISwapChain4** inOutSwapChain,
                                        ID3D12CommandQueue* gameQueue) {
    if (!loader.isLoaded()) {
        return Result::fail(Status::NotInitialised, "the AMD FSR runtime is not loaded");
    }
    if (!inOutSwapChain || !*inOutSwapChain || !gameQueue) {
        return Result::fail(Status::InvalidArgument,
                            "a swapchain and command queue are required to enable frame "
                            "generation");
    }
    if (swapChainContext_) return Result::ok();

    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrapDesc{};
    wrapDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    wrapDesc.header.pNext = nullptr;
    wrapDesc.swapchain = inOutSwapChain;
    wrapDesc.gameQueue = gameQueue;

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc{};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    versionDesc.header.pNext = nullptr;
    versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    wrapDesc.header.pNext = &versionDesc.header;

    IDXGISwapChain4* original = *inOutSwapChain;
    const ffxReturnCode_t rc =
        loader.fn().CreateContext(&swapChainContext_, &wrapDesc.header, nullptr);
    if (rc != FFX_API_RETURN_OK) {
        swapChainContext_ = nullptr;
        // AMD leaves the original untouched on failure; restore defensively so
        // the caller can keep presenting normally either way.
        *inOutSwapChain = original;
        return FfxLoader::fromFfx(rc, "wrapping the swapchain for frame generation");
    }

    loader_ = &loader;
    wrappedSwapChain_ = *inOutSwapChain;
    BFSR_LOG_INFO("frame-generation swapchain active (replaced %p with %p)",
                  static_cast<void*>(original), static_cast<void*>(wrappedSwapChain_));
    return Result::ok();
}

void FsrFrameGenerator::unwrapSwapChain() {
    if (!swapChainContext_ || !loader_ || !loader_->isLoaded()) {
        swapChainContext_ = nullptr;
        wrappedSwapChain_ = nullptr;
        return;
    }

    // Presents are queued asynchronously; releasing the context while frames
    // are still in flight destroys resources the presentation thread is using.
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 waitDesc{};
    waitDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    waitDesc.header.pNext = nullptr;
    const ffxReturnCode_t rc = loader_->fn().Dispatch(&swapChainContext_, &waitDesc.header);
    if (rc != FFX_API_RETURN_OK) {
        BFSR_LOG_WARN("waiting for queued presents returned ffx code %u; releasing anyway", rc);
    }

    loader_->fn().DestroyContext(&swapChainContext_, nullptr);
    swapChainContext_ = nullptr;
    wrappedSwapChain_ = nullptr;
    BFSR_LOG_INFO("frame-generation swapchain released");
}

bool FsrFrameGenerator::needsRecreate(const FrameGenPlan& plan,
                                      const FrameGenSettings& settings,
                                      DXGI_FORMAT backBufferFormat) const noexcept {
    if (!context_) return true;
    if (buildFrameGenCreateFlags(settings) != createFlags_) return true;
    if (plan.displaySize != displaySize_) return true;
    // Frame generation consumes depth and motion vectors at whatever
    // resolution the scene was rendered. Sizing for the display resolution
    // covers every case, so only a display change forces a rebuild.
    if (backBufferFormat != backBufferFormat_) return true;
    if (plan.versionId != versionId_) return true;
    return false;
}

Result FsrFrameGenerator::createContext(FfxLoader& loader,
                                        ID3D12Device* device,
                                        const FrameGenPlan& plan,
                                        const FrameGenSettings& settings,
                                        DXGI_FORMAT backBufferFormat) {
    destroyContext();

    if (!loader.isLoaded()) {
        return Result::fail(Status::NotInitialised, "the AMD FSR runtime is not loaded");
    }
    if (!device) return Result::fail(Status::InvalidArgument, "no Direct3D 12 device");
    if (!plan.displaySize.valid()) {
        return Result::fail(Status::InvalidArgument, "output size is zero");
    }

    createFlags_ = buildFrameGenCreateFlags(settings);
    displaySize_ = plan.displaySize;
    // Sized for the display resolution: that is the upper bound on the
    // resolution the scene can have been rendered at, so an upscaling quality
    // change never forces a frame-generation rebuild. This is what keeps the
    // two products independent at runtime as well as at install time.
    maxRenderSize_ = plan.displaySize;
    backBufferFormat_ = backBufferFormat;

    ffxCreateContextDescFrameGeneration createDesc{};
    createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    createDesc.header.pNext = nullptr;
    createDesc.flags = createFlags_;
    createDesc.displaySize = {displaySize_.width, displaySize_.height};
    createDesc.maxRenderSize = {maxRenderSize_.width, maxRenderSize_.height};
    createDesc.backBufferFormat = ffxApiGetSurfaceFormatDX12(backBufferFormat);

    ffxCreateContextDescFrameGenerationVersion versionDesc{};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    versionDesc.header.pNext = nullptr;
    versionDesc.version = FFX_FRAMEGENERATION_VERSION;

    ffxCreateBackendDX12Desc backendDesc{};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.header.pNext = nullptr;
    backendDesc.device = device;

    ffxOverrideVersion overrideDesc{};
    overrideDesc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    overrideDesc.header.pNext = nullptr;
    overrideDesc.versionId = plan.versionId;

    createDesc.header.pNext = &versionDesc.header;
    versionDesc.header.pNext = &backendDesc.header;
    if (plan.versionId != 0) {
        backendDesc.header.pNext = &overrideDesc.header;
    }

    const ffxReturnCode_t rc = loader.fn().CreateContext(&context_, &createDesc.header, nullptr);
    if (rc != FFX_API_RETURN_OK) {
        context_ = nullptr;
        return FfxLoader::fromFfx(rc, "creating the FSR frame-generation context");
    }

    loader_ = &loader;
    versionId_ = plan.versionId;
    versionName_ = plan.versionName;

    ffxQueryGetProviderVersion providerQuery{};
    providerQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    providerQuery.header.pNext = nullptr;
    if (loader.fn().Query(&context_, &providerQuery.header) == FFX_API_RETURN_OK &&
        providerQuery.versionName != nullptr) {
        versionName_ = providerQuery.versionName;
        versionId_ = providerQuery.versionId;
    }

    BFSR_LOG_INFO("FSR frame generation created: provider '%s', display %ux%u, flags 0x%x",
                  versionName_.c_str(), displaySize_.width, displaySize_.height, createFlags_);
    return Result::ok();
}

void FsrFrameGenerator::destroyContext() {
    if (context_ && loader_ && loader_->isLoaded()) {
        // Stop the swapchain calling back into a context that is about to die.
        if (swapChainContext_ && lastEnabled_) {
            ffxConfigureDescFrameGeneration off{};
            off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            off.header.pNext = nullptr;
            off.swapChain = wrappedSwapChain_;
            off.frameGenerationEnabled = false;
            loader_->fn().Configure(&context_, &off.header);
            lastEnabled_ = false;
        }
        loader_->fn().DestroyContext(&context_, nullptr);
    }
    context_ = nullptr;
    createFlags_ = 0;
    versionId_ = 0;
    versionName_.clear();
    displaySize_ = {};
    maxRenderSize_ = {};
    backBufferFormat_ = DXGI_FORMAT_UNKNOWN;
    lastEnabled_ = false;
}

Result FsrFrameGenerator::prepare(const FrameGenPrepareInputs& in,
                                  const FrameGenSettings& settings,
                                  uint64_t frameId) {
    if (!context_ || !loader_) {
        return Result::fail(Status::NotInitialised, "no frame-generation context");
    }
    if (!in.commandList || !in.depth || !in.motionVectors) {
        return Result::fail(Status::InvalidArgument,
                            "frame generation needs depth and motion vectors and at least "
                            "one was missing");
    }

    ffxDispatchDescFrameGenerationPrepareV2 desc{};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    desc.header.pNext = nullptr;
    desc.frameID = frameId;
    desc.flags = buildFrameGenDispatchFlags(settings);
    desc.commandList = in.commandList;
    desc.renderSize = {in.renderSize.width, in.renderSize.height};
    desc.jitterOffset = {in.jitterX, in.jitterY};

    float scaleX = in.motionVectorScaleX;
    float scaleY = in.motionVectorScaleY;
    if (settings.calibration.motionVectorsAreUvSpace) {
        scaleX *= static_cast<float>(in.renderSize.width);
        scaleY *= static_cast<float>(in.renderSize.height);
    }
    if (settings.calibration.motionVectorFlipY) scaleY = -scaleY;
    desc.motionVectorScale = {scaleX, scaleY};

    desc.frameTimeDelta = in.frameTimeDeltaMs;
    desc.reset = in.reset;
    desc.cameraNear = in.cameraNear;
    desc.cameraFar = in.cameraFar;
    desc.cameraFovAngleVertical = in.cameraFovVerticalRadians;
    desc.viewSpaceToMetersFactor = in.viewSpaceToMetersFactor;
    const uint32_t inputState = beaconResourceState(settings.calibration);
    desc.depth = ffxApiGetResourceDX12(in.depth, inputState);
    desc.motionVectors = ffxApiGetResourceDX12(in.motionVectors, inputState);

    if (in.cameraBasisValid) {
        for (int i = 0; i < 3; ++i) {
            desc.cameraPosition[i] = in.cameraPosition[i];
            desc.cameraUp[i] = in.cameraUp[i];
            desc.cameraRight[i] = in.cameraRight[i];
            desc.cameraForward[i] = in.cameraForward[i];
        }
    }

    const ffxReturnCode_t rc = loader_->fn().Dispatch(&context_, &desc.header);
    return FfxLoader::fromFfx(rc, "preparing frame-generation inputs");
}

Result FsrFrameGenerator::configure(bool enabled, const FrameGenSettings& settings, uint64_t frameId) {
    if (!context_ || !loader_) {
        return Result::fail(Status::NotInitialised, "no frame-generation context");
    }

    ffxConfigureDescFrameGeneration desc{};
    desc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    desc.header.pNext = nullptr;
    desc.swapChain = wrappedSwapChain_;
    desc.presentCallback = nullptr;             // The swapchain composites the UI itself.
    desc.presentCallbackUserContext = nullptr;
    desc.frameGenerationCallback = &FsrFrameGenerator::dispatchCallback;
    desc.frameGenerationCallbackUserContext = this;
    desc.frameGenerationEnabled = enabled;
    desc.allowAsyncWorkloads = enabled && settings.asyncCompute;
    desc.HUDLessColor = FfxApiResource{};
    desc.flags = buildFrameGenDispatchFlags(settings);
    desc.onlyPresentGenerated = false;
    // Whole-backbuffer interpolation: BeamNG has no letterboxing to exclude.
    desc.generationRect = {0, 0, static_cast<int32_t>(displaySize_.width),
                           static_cast<int32_t>(displaySize_.height)};
    desc.frameID = frameId;

    const ffxReturnCode_t rc = loader_->fn().Configure(&context_, &desc.header);
    if (rc == FFX_API_RETURN_OK) {
        if (enabled != lastEnabled_) {
            BFSR_LOG_INFO("frame generation %s", enabled ? "enabled" : "disabled");
        }
        lastEnabled_ = enabled;
    }
    return FfxLoader::fromFfx(rc, "configuring frame generation");
}

Result FsrFrameGenerator::registerUiResource(ID3D12Resource* uiTexture, bool premultipliedAlpha) {
    if (!swapChainContext_ || !loader_) {
        return Result::fail(Status::NotInitialised, "no frame-generation swapchain");
    }

    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 desc{};
    desc.header.type =
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    desc.header.pNext = nullptr;
    desc.uiResource = uiTexture
                          ? ffxApiGetResourceDX12(uiTexture, FFX_API_RESOURCE_STATE_COMPUTE_READ)
                          : FfxApiResource{};
    desc.flags = 0;
    if (premultipliedAlpha) {
        desc.flags |= FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA;
    }
    // The UI texture is written by the game while a previous frame may still be
    // compositing; let the swapchain double-buffer it rather than tearing.
    desc.flags |= FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;

    const ffxReturnCode_t rc = loader_->fn().Configure(&swapChainContext_, &desc.header);
    return FfxLoader::fromFfx(rc, "registering the UI resource for frame generation");
}

Result FsrFrameGenerator::queryMemoryUsage(FfxLoader& loader,
                                           ID3D12Device* device,
                                           Extent2D maxRenderSize,
                                           Extent2D displaySize,
                                           uint32_t createFlags,
                                           uint32_t dispatchFlags,
                                           DXGI_FORMAT backBufferFormat,
                                           uint64_t& outBytes) {
    outBytes = 0;
    if (!loader.isLoaded() || !device) {
        return Result::fail(Status::NotInitialised, "cannot query frame-generation memory yet");
    }

    FfxApiEffectMemoryUsage usage{};
    ffxQueryDescFrameGenerationGetGPUMemoryUsageV2 desc{};
    desc.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2;
    desc.header.pNext = nullptr;
    desc.device = device;
    desc.maxRenderSize = {maxRenderSize.width, maxRenderSize.height};
    desc.displaySize = {displaySize.width, displaySize.height};
    desc.createFlags = createFlags;
    desc.dispatchFlags = dispatchFlags;
    desc.backBufferFormat = ffxApiGetSurfaceFormatDX12(backBufferFormat);
    desc.hudlessBackBufferFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
    desc.gpuMemoryUsageFrameGeneration = &usage;

    const ffxReturnCode_t rc = loader.fn().Query(nullptr, &desc.header);
    if (rc != FFX_API_RETURN_OK) {
        return FfxLoader::fromFfx(rc, "querying frame-generation memory usage");
    }
    outBytes = usage.totalUsageInBytes;
    return Result::ok();
}

} // namespace bfsr
