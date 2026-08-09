// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <string>

#include "beamng_fsr/capabilities.hpp"
#include "beamng_fsr/json.hpp"

namespace bfsr {

/// Mirrors FfxApiUpscaleQualityMode from AMD's ffx_upscale.h.
/// The ratios are AMD's, quoted in the header comments there:
///   NativeAA 1.0x, Quality 1.5x, Balanced 1.7x, Performance 2.0x,
///   UltraPerformance 3.0x  (per dimension).
///
/// Note: "Ultra Quality" appears in the brief's mock-up but is NOT a mode in
/// the current FSR API — it existed in FSR 1.0 and was dropped. Offering it
/// would mean inventing a ratio AMD does not define, so it is deliberately
/// absent and `Custom` covers that need honestly via an explicit render scale.
enum class QualityMode {
    NativeAA = 0,
    Quality = 1,
    Balanced = 2,
    Performance = 3,
    UltraPerformance = 4,
    Custom = 100, ///< Uses Settings::customUpscaleRatio instead of an AMD ratio.
};

const char* toString(QualityMode m) noexcept;
QualityMode qualityModeFromString(const std::string& s, QualityMode def = QualityMode::Quality);
/// AMD's documented per-dimension ratio. Returns 0 for Custom.
float qualityModeRatio(QualityMode m) noexcept;

/// What the user asked for. Distinct from what is actually running — see
/// ResolvedPlan in auto_mode.hpp.
enum class UpscalerSelection { Disabled, Auto, Analytical, Ml };
enum class FrameGenSelection { Disabled, Auto, Analytical, Ml };

const char* toString(UpscalerSelection s) noexcept;
const char* toString(FrameGenSelection s) noexcept;
UpscalerSelection upscalerSelectionFromString(const std::string& s, UpscalerSelection def = UpscalerSelection::Disabled);
FrameGenSelection frameGenSelectionFromString(const std::string& s, FrameGenSelection def = FrameGenSelection::Disabled);

/// Depth conventions. BeamNG's is undocumented (FEASIBILITY §5.1), so this is a
/// user-overridable calibration knob rather than a hardcoded assumption.
enum class DepthConvention { Auto, ReversedZ, StandardZ };
const char* toString(DepthConvention c) noexcept;
DepthConvention depthConventionFromString(const std::string& s, DepthConvention def = DepthConvention::Auto);

struct Settings {
    // --- master ---
    bool enabled = false;              ///< Off until the user opts in.
    bool safeMode = false;             ///< Set by the crash guard; forces pass-through.

    // --- upscaling ---
    UpscalerSelection upscaler = UpscalerSelection::Disabled;
    std::string upscalerVersionId = "auto"; ///< Opaque AMD version id, or "auto".
    QualityMode quality = QualityMode::Quality;
    float customUpscaleRatio = 1.5f;   ///< Per-dimension divisor used when quality == Custom.
                                       ///< Same convention as AMD's ratios: render = output / ratio,
                                       ///< so 1.0 is native and 3.0 matches ultra performance.
    bool sharpeningEnabled = true;
    float sharpness = 0.5f;            ///< AMD range [0,1].

    // --- frame generation ---
    FrameGenSelection frameGeneration = FrameGenSelection::Disabled;
    std::string frameGenVersionId = "auto";
    bool frameGenAsyncCompute = true;
    bool frameGenUiComposition = true; ///< Composite the UI after interpolation.

    // --- latency ---
    bool antiLag2 = false;

    // --- calibration (see FEASIBILITY §5.1/§5.2) ---
    DepthConvention depthConvention = DepthConvention::Auto;
    bool depthInfinite = true;
    float motionVectorScaleX = 1.0f;
    float motionVectorScaleY = 1.0f;
    bool motionVectorFlipY = true;
    bool motionVectorsAreUvSpace = true;

    // --- diagnostics ---
    bool debugOverlay = false;
    bool debugView = false;            ///< FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW
    bool benchmarkMode = false;
    std::string logLevel = "info";
    bool forceCompatibilityMode = false; ///< Disables async compute and version overrides.

    [[nodiscard]] Json toJson() const;
    static Settings fromJson(const Json& j);

    /// Clamps every numeric field into a sane range and repairs contradictions
    /// (e.g. frame generation requested while the master switch is off).
    /// Applied on every load so a hand-edited file cannot destabilise the bridge.
    void sanitise();

    [[nodiscard]] bool operator==(const Settings& other) const;
    [[nodiscard]] bool operator!=(const Settings& other) const { return !(*this == other); }
};

/// Loads/stores settings as JSON. Writes go through a temp file + rename so a
/// crash mid-write cannot leave a truncated config behind.
class SettingsStore {
public:
    explicit SettingsStore(std::string path);

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    /// Returns defaults (and reports why) when the file is missing or corrupt.
    Settings load(std::string* diagnostic = nullptr) const;
    bool save(const Settings& settings, std::string* diagnostic = nullptr) const;

private:
    std::string path_;
};

} // namespace bfsr
