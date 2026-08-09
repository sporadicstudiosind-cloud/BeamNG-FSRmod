// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// One IPC channel per product. Each mod half writes its own request document
// and reads its own status document, so neither can be blocked, confused or
// left stale by the other's absence.
//
// Both requests carry the same `SharedFrameState` (beacon description, camera
// parameters, game state). That is duplication by design: it keeps each product
// self-sufficient. A shared third file would make the products depend on each
// other, which is exactly what this split exists to avoid.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "beamng_fsr/beacon_signature.hpp"
#include "beamng_fsr/capabilities.hpp"
#include "beamng_fsr/json.hpp"
#include "beamng_fsr/module_registry.hpp"
#include "beamng_fsr/plan.hpp"
#include "beamng_fsr/settings.hpp"

namespace bfsr {

/// Wire format version. Each half refuses to act on a document from a different
/// version rather than misinterpreting fields — a mismatched pair is what
/// happens when a user updates one product and not the other.
constexpr int kIpcProtocolVersion = 2;

bool isProtocolCompatible(int otherVersion) noexcept;

/// State both products need, produced by the shared Lua core.
struct SharedFrameState {
    // Live game state, feeding the frame-generation gate and upscaler resets.
    bool paused = false;
    bool loading = false;
    bool uiFocused = false;
    bool photoMode = false;
    bool replay = false;
    bool cameraCut = false;

    // Camera parameters FSR needs and Lua can read.
    bool cameraValid = false;
    float cameraNear = 0.1f;
    float cameraFar = 10000.0f;
    float cameraFovVerticalRadians = 1.2f;

    /// Description of the render target the shared PostFX beacon writes into.
    BeaconSignature beacon;

    [[nodiscard]] Json toJson() const;
    static SharedFrameState fromJson(const Json& j);
};

/// Frame generation: mod half -> bridge.
struct FrameGenRequest {
    int protocolVersion = kIpcProtocolVersion;
    uint64_t sequence = 0;
    std::string modVersion;
    std::string beamngVersion;
    FrameGenSettings settings;
    SharedFrameState shared;
    bool clearSafeMode = false;

    [[nodiscard]] Json toJson() const;
    static FrameGenRequest fromJson(const Json& j);
};

/// Upscaling: mod half -> bridge.
struct UpscaleRequest {
    int protocolVersion = kIpcProtocolVersion;
    uint64_t sequence = 0;
    std::string modVersion;
    std::string beamngVersion;
    UpscaleSettings settings;
    SharedFrameState shared;
    bool clearSafeMode = false;

    [[nodiscard]] Json toJson() const;
    static UpscaleRequest fromJson(const Json& j);
};

/// Environment and metrics common to both status documents.
struct StatusCommon {
    std::string bridgeVersion;
    bool bridgeLoaded = false;
    bool initialised = false;
    std::string initFailureReason;

    GpuInfo gpu;
    SystemInfo system;
    CapabilitySet capabilities;
    Json installedModules = Json::array();

    // Measured only. Zero means "not measured", never "zero cost".
    float renderedFps = 0.0f;
    float displayedFps = 0.0f;
    float onePercentLowFps = 0.0f;
    float pointOnePercentLowFps = 0.0f;
    float frameTimeMs = 0.0f;
    uint64_t framesRendered = 0;

    std::string depthSource;
    std::string motionVectorSource;
    std::vector<std::string> recentLog;

    [[nodiscard]] Json toJson() const;
    static StatusCommon fromJson(const Json& j);
};

/// Frame generation: bridge -> mod half.
struct FrameGenStatus {
    int protocolVersion = kIpcProtocolVersion;
    uint64_t sequence = 0;
    bool moduleInstalled = false;
    StatusCommon common;
    FrameGenPlan plan;
    std::vector<FfxVersionEntry> versions;

    float gpuMs = 0.0f;
    uint64_t vramBytes = 0;
    uint64_t framesGenerated = 0;
    /// Frames AMD's swapchain reported generating per rendered frame, as
    /// observed. SDK 2.3.0 gives the application no way to *request* a
    /// multiplier — `numGeneratedFrames` arrives filled in on the dispatch
    /// callback — so this is measured, never configured.
    uint32_t observedGeneratedPerFrame = 0;
    std::string suppressReason;
    bool antiLagActive = false;
    bool safeModeActive = false;

    [[nodiscard]] Json toJson() const;
    static FrameGenStatus fromJson(const Json& j);
};

/// Upscaling: bridge -> mod half.
struct UpscaleStatus {
    int protocolVersion = kIpcProtocolVersion;
    uint64_t sequence = 0;
    bool moduleInstalled = false;
    StatusCommon common;
    UpscalePlan plan;
    std::vector<FfxVersionEntry> versions;

    float gpuMs = 0.0f;
    uint64_t vramBytes = 0;
    bool jitterAvailable = false;
    bool safeModeActive = false;

    [[nodiscard]] Json toJson() const;
    static UpscaleStatus fromJson(const Json& j);
};

} // namespace bfsr
