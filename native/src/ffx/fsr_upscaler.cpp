// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/fsr_upscaler.hpp"

#include "beamng_fsr/logging.hpp"

#include <ffx_api_dx12.h>
#include <ffx_upscale.h>

namespace bfsr {
namespace {

/// Builds the creation-time flag set. These are baked into the context, which
/// is why changing any of them forces a recreate.
uint32_t buildCreateFlags(const Settings& settings, bool colourIsHdr) {
    uint32_t flags = 0;

    if (colourIsHdr) flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;

    // Motion vectors come from BeamNG's velocity buffer at render resolution,
    // and BeamNG does not apply a jitter pattern we control, so neither the
    // display-resolution nor the jitter-cancellation flag applies.

    // BeamNG's depth convention is undocumented (FEASIBILITY §5.1). Auto
    // resolves to reversed-Z, which is the modern default and what the beacon
    // shader's projParams inspection is expected to confirm; the user can
    // override it and the choice is logged.
    const bool reversedZ = settings.depthConvention != DepthConvention::StandardZ;
    if (reversedZ) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (settings.depthInfinite) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;

    // No exposure texture is produced by the mod half today, so let FSR derive
    // it. This is a documented, supported mode rather than a workaround.
    flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;

    if (settings.debugView) flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    if (settings.logLevel == "trace" || settings.logLevel == "debug") {
        flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    }
    return flags;
}

void ffxUpscalerMessage(uint32_t type, const wchar_t* message) {
    if (!message) return;
    const int needed = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return;
    std::string text(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text.data(), needed, nullptr, nullptr);
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        BFSR_LOG_ERROR("FSR upscaler: %s", text.c_str());
    } else {
        BFSR_LOG_WARN("FSR upscaler: %s", text.c_str());
    }
}

} // namespace

FsrUpscaler::~FsrUpscaler() { destroy(); }

bool FsrUpscaler::needsRecreate(const ResolvedPlan& plan,
                                const Settings& settings,
                                bool colourIsHdr) const noexcept {
    if (!context_) return true;
    if (buildCreateFlags(settings, colourIsHdr) != createFlags_) return true;
    // A context can dispatch at any size up to its maximum, so only growth
    // forces a rebuild. Shrinking (a lower quality preset) reuses the context,
    // which is what makes preset changes cheap.
    if (plan.renderSize.width > maxRenderSize_.width ||
        plan.renderSize.height > maxRenderSize_.height) return true;
    if (plan.upscaleSize != maxUpscaleSize_) return true;
    if (plan.upscalerVersionId != versionId_) return true;
    return false;
}

Result FsrUpscaler::create(FfxLoader& loader,
                           ID3D12Device* device,
                           const ResolvedPlan& plan,
                           const Settings& settings,
                           bool colourIsHdr) {
    destroy();

    if (!loader.isLoaded()) {
        return Result::fail(Status::NotInitialised, "the AMD FSR runtime is not loaded");
    }
    if (!device) {
        return Result::fail(Status::InvalidArgument, "no Direct3D 12 device");
    }
    if (!plan.renderSize.valid() || !plan.upscaleSize.valid()) {
        return Result::fail(Status::InvalidArgument, "render or output size is zero");
    }

    createFlags_ = buildCreateFlags(settings, colourIsHdr);

    // Size the context for the largest render resolution any quality preset can
    // ask for (i.e. native), so preset changes never force a rebuild.
    maxRenderSize_ = plan.upscaleSize;
    maxUpscaleSize_ = plan.upscaleSize;

    ffxCreateContextDescUpscale createUpscale{};
    createUpscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    createUpscale.header.pNext = nullptr;
    createUpscale.flags = createFlags_;
    createUpscale.maxRenderSize = {maxRenderSize_.width, maxRenderSize_.height};
    createUpscale.maxUpscaleSize = {maxUpscaleSize_.width, maxUpscaleSize_.height};
    createUpscale.fpMessage = &ffxUpscalerMessage;

    // Declaring the header version we compiled against lets AMD's runtime
    // keep ABI compatibility when it ships a newer struct layout.
    ffxCreateContextDescUpscaleVersion versionDesc{};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    versionDesc.header.pNext = nullptr;
    versionDesc.version = FFX_UPSCALER_VERSION;

    ffxCreateBackendDX12Desc backendDesc{};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.header.pNext = nullptr;
    backendDesc.device = device;

    ffxOverrideVersion overrideDesc{};
    overrideDesc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    overrideDesc.header.pNext = nullptr;
    overrideDesc.versionId = plan.upscalerVersionId;

    // Chain: upscale -> version -> backend [-> override]
    createUpscale.header.pNext = &versionDesc.header;
    versionDesc.header.pNext = &backendDesc.header;
    if (plan.upscalerVersionId != 0) {
        backendDesc.header.pNext = &overrideDesc.header;
    }

    const ffxReturnCode_t rc =
        loader.fn().CreateContext(&context_, &createUpscale.header, nullptr);
    if (rc != FFX_API_RETURN_OK) {
        context_ = nullptr;
        return FfxLoader::fromFfx(rc, "creating the FSR upscaling context");
    }

    loader_ = &loader;
    device_ = device;
    versionId_ = plan.upscalerVersionId;
    versionName_ = plan.upscalerVersionName;

    // Ask the runtime which provider it actually bound. When the plan did not
    // pin a version this is the only way to report the truth rather than a
    // guess, and it is what the diagnostics panel shows.
    ffxQueryGetProviderVersion providerQuery{};
    providerQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    providerQuery.header.pNext = nullptr;
    if (loader.fn().Query(&context_, &providerQuery.header) == FFX_API_RETURN_OK &&
        providerQuery.versionName != nullptr) {
        versionName_ = providerQuery.versionName;
        versionId_ = providerQuery.versionId;
    }

    BFSR_LOG_INFO("FSR upscaler created: provider '%s', max render %ux%u, output %ux%u, flags 0x%x",
                  versionName_.c_str(), maxRenderSize_.width, maxRenderSize_.height,
                  maxUpscaleSize_.width, maxUpscaleSize_.height, createFlags_);
    return Result::ok();
}

void FsrUpscaler::destroy() {
    if (context_ && loader_ && loader_->isLoaded()) {
        loader_->fn().DestroyContext(&context_, nullptr);
    }
    context_ = nullptr;
    loader_ = nullptr;
    device_ = nullptr;
    createFlags_ = 0;
    versionId_ = 0;
    versionName_.clear();
    maxRenderSize_ = {};
    maxUpscaleSize_ = {};
}

Result FsrUpscaler::dispatch(const UpscaleFrameInputs& in, const Settings& settings) {
    if (!context_ || !loader_) {
        return Result::fail(Status::NotInitialised, "the FSR upscaling context does not exist");
    }
    if (!in.commandList || !in.color || !in.depth || !in.motionVectors || !in.output) {
        // Refusing here is deliberate: dispatching with a missing input would
        // either crash the runtime or produce a visibly broken frame.
        return Result::fail(Status::InvalidArgument,
                            "an FSR upscaling input was missing (colour, depth, motion "
                            "vectors, output or command list)");
    }

    ffxDispatchDescUpscale desc{};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    desc.header.pNext = nullptr;
    desc.commandList = in.commandList;

    desc.color = ffxApiGetResourceDX12(in.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.depth = ffxApiGetResourceDX12(in.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.motionVectors = ffxApiGetResourceDX12(in.motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.output = ffxApiGetResourceDX12(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.exposure = in.exposure
                        ? ffxApiGetResourceDX12(in.exposure, FFX_API_RESOURCE_STATE_COMPUTE_READ)
                        : FfxApiResource{};
    desc.reactive = in.reactive
                        ? ffxApiGetResourceDX12(in.reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ)
                        : FfxApiResource{};
    desc.transparencyAndComposition = FfxApiResource{};

    desc.jitterOffset = {in.jitterX, in.jitterY};

    // FSR wants pixel-space motion vectors at render resolution. BeamNG's
    // velocity encoding is undocumented (FEASIBILITY §5.2), so the conversion
    // is a calibration parameter with a debug view to verify it, not a silent
    // assumption. UV-space input is converted by scaling with the render size.
    float scaleX = in.motionVectorScaleX;
    float scaleY = in.motionVectorScaleY;
    if (settings.motionVectorsAreUvSpace) {
        scaleX *= static_cast<float>(in.renderSize.width);
        scaleY *= static_cast<float>(in.renderSize.height);
    }
    if (settings.motionVectorFlipY) scaleY = -scaleY;
    desc.motionVectorScale = {scaleX, scaleY};

    desc.renderSize = {in.renderSize.width, in.renderSize.height};
    desc.upscaleSize = {in.upscaleSize.width, in.upscaleSize.height};
    desc.enableSharpening = settings.sharpeningEnabled;
    desc.sharpness = settings.sharpness;
    desc.frameTimeDelta = in.frameTimeDeltaMs;
    desc.preExposure = in.preExposure > 0.0f ? in.preExposure : 1.0f;
    desc.reset = in.reset;
    desc.cameraNear = in.cameraNear;
    desc.cameraFar = in.cameraFar;
    desc.cameraFovAngleVertical = in.cameraFovVerticalRadians;
    desc.viewSpaceToMetersFactor = in.viewSpaceToMetersFactor;

    desc.flags = 0;
    if (settings.debugView) desc.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;

    const ffxReturnCode_t rc = loader_->fn().Dispatch(&context_, &desc.header);
    return FfxLoader::fromFfx(rc, "dispatching FSR upscaling");
}

Result FsrUpscaler::queryMemoryUsage(FfxLoader& loader,
                                     ID3D12Device* device,
                                     Extent2D maxRenderSize,
                                     Extent2D maxUpscaleSize,
                                     uint32_t createFlags,
                                     uint64_t& outBytes) {
    outBytes = 0;
    if (!loader.isLoaded() || !device) {
        return Result::fail(Status::NotInitialised, "cannot query FSR memory usage yet");
    }

    FfxApiEffectMemoryUsage usage{};
    ffxQueryDescUpscaleGetGPUMemoryUsageV2 desc{};
    desc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2;
    desc.header.pNext = nullptr;
    desc.device = device;
    desc.maxRenderSize = {maxRenderSize.width, maxRenderSize.height};
    desc.maxUpscaleSize = {maxUpscaleSize.width, maxUpscaleSize.height};
    desc.flags = createFlags;
    desc.gpuMemoryUsageUpscaler = &usage;

    const ffxReturnCode_t rc = loader.fn().Query(nullptr, &desc.header);
    if (rc != FFX_API_RETURN_OK) {
        return FfxLoader::fromFfx(rc, "querying FSR upscaler memory usage");
    }
    outBytes = usage.totalUsageInBytes;
    return Result::ok();
}

Result FsrUpscaler::queryJitterOffset(FfxLoader& loader,
                                      int32_t frameIndex,
                                      Extent2D renderSize,
                                      Extent2D displaySize,
                                      float& outX,
                                      float& outY) {
    outX = 0.0f;
    outY = 0.0f;
    if (!loader.isLoaded()) {
        return Result::fail(Status::NotInitialised, "the AMD FSR runtime is not loaded");
    }

    int32_t phaseCount = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount phaseDesc{};
    phaseDesc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    phaseDesc.header.pNext = nullptr;
    phaseDesc.renderWidth = renderSize.width;
    phaseDesc.displayWidth = displaySize.width;
    phaseDesc.pOutPhaseCount = &phaseCount;

    ffxReturnCode_t rc = loader.fn().Query(nullptr, &phaseDesc.header);
    if (rc != FFX_API_RETURN_OK) {
        return FfxLoader::fromFfx(rc, "querying the FSR jitter phase count");
    }
    if (phaseCount <= 0) {
        return Result::fail(Status::RuntimeError,
                            "the FSR runtime reported a non-positive jitter phase count");
    }

    ffxQueryDescUpscaleGetJitterOffset offsetDesc{};
    offsetDesc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    offsetDesc.header.pNext = nullptr;
    offsetDesc.index = frameIndex % phaseCount;
    offsetDesc.phaseCount = phaseCount;
    offsetDesc.pOutX = &outX;
    offsetDesc.pOutY = &outY;

    rc = loader.fn().Query(nullptr, &offsetDesc.header);
    return FfxLoader::fromFfx(rc, "querying the FSR jitter offset");
}

} // namespace bfsr
