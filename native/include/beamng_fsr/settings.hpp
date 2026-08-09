// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// Settings are split along the same line as the products themselves: frame
// generation and upscaling each own an independent settings document, so one
// can be installed, configured and uninstalled without the other existing.
//
// The two structs share `RenderCalibration` and `DiagnosticsSettings` by value
// rather than by reference to a third file. Both products' UIs expose the same
// calibration controls and write the same values, so when both are installed
// they agree; when only one is installed it is self-sufficient. A shared file
// would create a dependency between two things that are supposed to be
// independent.
#pragma once

#include <cstdint>
#include <string>

#include "beamng_fsr/capabilities.hpp"
#include "beamng_fsr/json.hpp"

namespace bfsr {

// ---------------------------------------------------------------------------
// Shared sub-structures
// ---------------------------------------------------------------------------

/// Depth conventions. BeamNG's is undocumented (FEASIBILITY §5.1), so this is a
/// user-overridable calibration knob rather than a hardcoded assumption.
enum class DepthConvention { Auto, ReversedZ, StandardZ };
const char* toString(DepthConvention c) noexcept;
DepthConvention depthConventionFromString(const std::string& s, DepthConvention def = DepthConvention::Auto);

/// How BeamNG's depth and velocity buffers are interpreted.
///
/// Every field here is a calibration parameter, not a preference: BeamNG
/// documents neither encoding, so these are assumptions the user can correct
/// with the debug view rather than facts this project asserts.
struct RenderCalibration {
    DepthConvention depthConvention = DepthConvention::Auto;
    bool depthInfinite = true;
    float motionVectorScaleX = 1.0f;
    float motionVectorScaleY = 1.0f;
    bool motionVectorFlipY = true;
    bool motionVectorsAreUvSpace = true;

    /// The Direct3D 12 resource state the beacon targets are in when BeamNG
    /// presents.
    ///
    /// D3D12 provides no way to query a resource's current state, and BeamNG
    /// does not document what state its PostFX system leaves a render target
    /// in. FSR's backend transitions the resources it is given from the state
    /// the caller declares, so declaring the wrong one produces incorrect
    /// barriers. This is therefore an assumption with an override, exactly like
    /// the depth convention and motion-vector encoding above, and it is
    /// documented as unverified in docs/LIMITATIONS.md.
    ///
    /// One of: "pixelComputeRead" (default), "pixelRead", "computeRead",
    /// "renderTarget", "genericRead", "common".
    std::string beaconResourceState = "pixelComputeRead";

    [[nodiscard]] Json toJson() const;
    static RenderCalibration fromJson(const Json& j);
    void sanitise();
    [[nodiscard]] bool operator==(const RenderCalibration& o) const;
};

struct DiagnosticsSettings {
    bool overlay = false;
    bool debugView = false;       ///< AMD's own debug visualisation.
    bool benchmarkMode = false;
    std::string logLevel = "info";
    bool forceCompatibilityMode = false; ///< Disables async compute and version pinning.

    [[nodiscard]] Json toJson() const;
    static DiagnosticsSettings fromJson(const Json& j);
    void sanitise();
    [[nodiscard]] bool operator==(const DiagnosticsSettings& o) const;
};

// ---------------------------------------------------------------------------
// Frame generation
// ---------------------------------------------------------------------------

enum class FrameGenSelection { Disabled, Auto, Analytical, Ml };
const char* toString(FrameGenSelection s) noexcept;
FrameGenSelection frameGenSelectionFromString(const std::string& s,
                                              FrameGenSelection def = FrameGenSelection::Disabled);

struct FrameGenSettings {
    bool enabled = false;
    bool safeMode = false;

    FrameGenSelection implementation = FrameGenSelection::Auto;
    std::string versionId = "auto"; ///< Opaque AMD version id, or "auto".

    bool asyncCompute = true;
    bool uiComposition = true;   ///< Composite the UI after interpolation.

    // --- latency ---
    bool antiLag2 = false;
    /// Frame cap handed to Anti-Lag 2. Zero means uncapped.
    uint32_t antiLagMaxFps = 0;

    RenderCalibration calibration;
    DiagnosticsSettings diagnostics;

    [[nodiscard]] Json toJson() const;
    static FrameGenSettings fromJson(const Json& j);
    void sanitise();
    [[nodiscard]] bool operator==(const FrameGenSettings& o) const;
    [[nodiscard]] bool operator!=(const FrameGenSettings& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// Upscaling
// ---------------------------------------------------------------------------

/// Mirrors FfxApiUpscaleQualityMode from AMD's ffx_upscale.h. The ratios are
/// AMD's, quoted in that header: NativeAA 1.0x, Quality 1.5x, Balanced 1.7x,
/// Performance 2.0x, UltraPerformance 3.0x, per dimension.
///
/// "Ultra Quality" is deliberately absent — it is not a mode in the current FSR
/// API, and offering it would mean inventing a ratio AMD does not define.
/// `Custom` covers that need honestly with an explicit ratio.
enum class QualityMode {
    NativeAA = 0,
    Quality = 1,
    Balanced = 2,
    Performance = 3,
    UltraPerformance = 4,
    Custom = 100,
};

const char* toString(QualityMode m) noexcept;
QualityMode qualityModeFromString(const std::string& s, QualityMode def = QualityMode::Quality);
/// AMD's documented per-dimension ratio. Returns 0 for Custom.
float qualityModeRatio(QualityMode m) noexcept;

enum class UpscalerSelection { Disabled, Auto, Analytical, Ml };
const char* toString(UpscalerSelection s) noexcept;
UpscalerSelection upscalerSelectionFromString(const std::string& s,
                                              UpscalerSelection def = UpscalerSelection::Disabled);

struct UpscaleSettings {
    bool enabled = false;
    bool safeMode = false;

    UpscalerSelection implementation = UpscalerSelection::Auto;
    std::string versionId = "auto";

    QualityMode quality = QualityMode::Quality;
    /// Per-dimension divisor used when quality == Custom. Same convention as
    /// AMD's ratios: render = output / ratio, so 1.0 is native and 3.0 matches
    /// ultra performance.
    float customUpscaleRatio = 1.5f;

    bool sharpeningEnabled = true;
    float sharpness = 0.5f; ///< AMD's range is [0,1].

    RenderCalibration calibration;
    DiagnosticsSettings diagnostics;

    [[nodiscard]] Json toJson() const;
    static UpscaleSettings fromJson(const Json& j);
    void sanitise();
    [[nodiscard]] bool operator==(const UpscaleSettings& o) const;
    [[nodiscard]] bool operator!=(const UpscaleSettings& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------

/// Loads and stores one settings document. Writes go through a temp file and a
/// rename so a crash mid-write cannot leave a truncated config behind.
template <typename T>
class SettingsStore {
public:
    explicit SettingsStore(std::string path) : path_(std::move(path)) {}

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    /// Returns defaults (and reports why) when the file is missing or corrupt.
    T load(std::string* diagnostic = nullptr) const;
    bool save(const T& settings, std::string* diagnostic = nullptr) const;

private:
    std::string path_;
};

extern template class SettingsStore<FrameGenSettings>;
extern template class SettingsStore<UpscaleSettings>;

} // namespace bfsr
