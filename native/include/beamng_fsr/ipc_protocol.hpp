// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "beamng_fsr/beacon_signature.hpp"
#include "beamng_fsr/capabilities.hpp"
#include "beamng_fsr/json.hpp"
#include "beamng_fsr/plan.hpp"
#include "beamng_fsr/settings.hpp"

namespace bfsr {

/// Wire format version. The mod half refuses to talk to a bridge reporting a
/// different major version rather than misinterpreting fields — a mismatched
/// pair is exactly what happens when a user updates one half and not the other.
constexpr int kIpcProtocolVersion = 1;

/// Mod half -> bridge.
///
/// Deliberately low-rate (~10 Hz is ample). Nothing latency-critical travels
/// through here: the actual per-frame render data is GPU-side, and every field
/// below changes on the order of seconds, not frames. Using files rather than a
/// pipe keeps the mod half inside BeamNG's Lua sandbox, which permits file I/O
/// but not sockets or native code.
struct IpcRequest {
    int protocolVersion = kIpcProtocolVersion;
    uint64_t sequence = 0;
    std::string modVersion;
    std::string beamngVersion;

    Settings settings;

    // --- live game state feeding the frame-generation gate ---
    bool paused = false;
    bool loading = false;
    bool uiFocused = false;
    bool photoMode = false;
    bool replay = false;
    bool cameraCut = false; ///< Set for one tick after a camera/level change.

    // --- camera parameters FSR needs and Lua can read ---
    float cameraNear = 0.1f;
    float cameraFar = 10000.0f;
    float cameraFovVerticalRadians = 1.2f;
    bool cameraParamsValid = false;

    /// Description of the render target the mod's PostFX pass writes into.
    BeaconSignature depthMvBeacon;

    /// Set when the user pressed "clear safe mode" in the UI.
    bool clearSafeMode = false;

    [[nodiscard]] Json toJson() const;
    static IpcRequest fromJson(const Json& j);
};

/// Bridge -> mod half. Drives the settings UI and the diagnostics panel.
struct IpcStatus {
    int protocolVersion = kIpcProtocolVersion;
    uint64_t sequence = 0;
    std::string bridgeVersion;
    bool bridgeLoaded = false;
    bool initialised = false;
    /// Present when the bridge refused to start; shown verbatim in the UI.
    std::string initFailureReason;

    GpuInfo gpu;
    SystemInfo system;
    CapabilitySet capabilities;
    ResolvedPlan plan;

    std::vector<FfxVersionEntry> upscalerVersions;
    std::vector<FfxVersionEntry> frameGenVersions;

    // --- measured diagnostics (never estimated; zero means "not measured") ---
    float renderedFps = 0.0f;
    float displayedFps = 0.0f;
    float onePercentLowFps = 0.0f;
    float pointOnePercentLowFps = 0.0f;
    float frameTimeMs = 0.0f;
    float upscalerGpuMs = 0.0f;
    float frameGenGpuMs = 0.0f;
    uint64_t upscalerVramBytes = 0;
    uint64_t frameGenVramBytes = 0;
    uint64_t framesRendered = 0;
    uint64_t framesGenerated = 0;

    std::string frameGenSuppressReason;
    std::string depthSource;         ///< e.g. "beacon (exact)"
    std::string motionVectorSource;
    bool safeModeActive = false;

    std::vector<std::string> recentLog;

    [[nodiscard]] Json toJson() const;
    static IpcStatus fromJson(const Json& j);
};

/// True when the two halves can safely interoperate.
bool isProtocolCompatible(int otherVersion) noexcept;

} // namespace bfsr
