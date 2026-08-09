// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// Two independent planners, one per product.
//
// `resolveFrameGenPlan` and `resolveUpscalePlan` share no state and neither
// consults the other's settings or result. That is what makes the two mods
// separately installable: whichever is present plans for itself, and when both
// are present they compose without either needing to know.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "beamng_fsr/capabilities.hpp"
#include "beamng_fsr/settings.hpp"

namespace bfsr {

struct Extent2D {
    uint32_t width = 0;
    uint32_t height = 0;
    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
    [[nodiscard]] uint64_t pixels() const noexcept {
        return static_cast<uint64_t>(width) * height;
    }
    [[nodiscard]] bool operator==(const Extent2D& o) const noexcept {
        return width == o.width && height == o.height;
    }
    [[nodiscard]] bool operator!=(const Extent2D& o) const noexcept { return !(*this == o); }
};

/// One entry from AMD's `ffxQueryDescGetVersions`. Populated at runtime from the
/// installed DLLs — never hardcoded, so a newer AMD SDK adds options for free.
struct FfxVersionEntry {
    uint64_t id = 0;
    std::string name;
    bool isMachineLearning = false;
};

/// Everything the planners need that is not a user preference. Shared by both
/// because it describes the machine, not either product.
struct RuntimeContext {
    Extent2D displaySize;
    /// The resolution BeamNG is actually rendering its 3D scene at. Equal to
    /// displaySize unless the user lowered the game's own resolution — the
    /// bridge cannot change this itself (FEASIBILITY §6.4).
    Extent2D sceneRenderSize;
    uint64_t availableVramBytes = 0;
    /// Reported by AMD's own GetGPUMemoryUsageV2 queries. Zero means "not
    /// measured", which the planners treat as unknown rather than as zero cost.
    uint64_t estimatedUpscalerVramBytes = 0;
    uint64_t estimatedFrameGenVramBytes = 0;

    bool frameInputsAvailable = false;
    bool motionVectorsAvailable = false;
    bool depthAvailable = false;
    /// Always false today: BeamNG exposes no projection-jitter hook. Kept as a
    /// field so the planner's behaviour is testable for the day it changes.
    bool cameraJitterAvailable = false;
    /// Smoothed rendered (not displayed) frame rate; 0 when not yet measured.
    float measuredBaseFps = 0.0f;

    std::vector<FfxVersionEntry> upscalerVersions;
    std::vector<FfxVersionEntry> frameGenVersions;
};

/// Notes attached to a plan. Split by kind because the UI renders them
/// differently: a downgrade is something the user asked for and did not get, a
/// decision is Auto explaining itself, a warning applies to what is running.
struct PlanNotes {
    std::vector<std::string> warnings;
    std::vector<std::string> downgrades;
    std::vector<std::string> decisions;

    [[nodiscard]] bool empty() const noexcept {
        return warnings.empty() && downgrades.empty() && decisions.empty();
    }
};

// ---------------------------------------------------------------------------
// Frame generation
// ---------------------------------------------------------------------------

enum class FrameGenBackend { None, Analytical, Ml };
const char* toString(FrameGenBackend b) noexcept;

/// AMD's documented minimum rendered frame rate below which generated frames
/// "may result in visual artifacts severe enough to negatively impact the user
/// experience" (FSR SDK 2.3.0, frame-interpolation-swap-chain.md). These are
/// AMD's numbers, per implementation — not a heuristic invented here.
constexpr float kAmdMinBaseFpsAnalytical = 60.0f; ///< FSR-SRFG 3.1.6
constexpr float kAmdMinBaseFpsMl = 30.0f;         ///< FSR-FG 4.0.1

float amdRecommendedMinimumBaseFps(FrameGenBackend backend) noexcept;

struct FrameGenPlan {
    FrameGenBackend backend = FrameGenBackend::None;
    bool antiLag2 = false;
    uint32_t antiLagMaxFps = 0;
    bool asyncCompute = true;
    bool uiComposition = true;

    uint64_t versionId = 0; ///< 0 == let AMD choose the default provider.
    std::string versionName;

    Extent2D displaySize;
    PlanNotes notes;

    [[nodiscard]] bool active() const noexcept { return backend != FrameGenBackend::None; }
    [[nodiscard]] Json toJson() const;
};

FrameGenPlan resolveFrameGenPlan(const FrameGenSettings& settings,
                                 const CapabilitySet& caps,
                                 const RuntimeContext& ctx);

// ---------------------------------------------------------------------------
// Upscaling
// ---------------------------------------------------------------------------

enum class UpscalerBackend { None, Analytical, Ml };
const char* toString(UpscalerBackend b) noexcept;

struct UpscalePlan {
    UpscalerBackend backend = UpscalerBackend::None;

    uint64_t versionId = 0;
    std::string versionName;

    QualityMode quality = QualityMode::Quality;
    Extent2D renderSize;  ///< What FSR is told the inputs are.
    Extent2D upscaleSize; ///< What FSR outputs.
    float sharpness = 0.5f;
    bool sharpeningEnabled = true;

    PlanNotes notes;

    [[nodiscard]] bool active() const noexcept { return backend != UpscalerBackend::None; }
    [[nodiscard]] Json toJson() const;
};

UpscalePlan resolveUpscalePlan(const UpscaleSettings& settings,
                               const CapabilitySet& caps,
                               const RuntimeContext& ctx);

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

/// Classifies an AMD version string as ML or analytical.
///
/// AMD exposes no machine-readable "is ML" flag on the version query, so this
/// parses the display name: the 4.x lines of both the upscaler and frame
/// generation are the ML implementations. Unrecognised names default to
/// analytical, the conservative answer.
bool classifyFfxVersionAsMl(const std::string& versionName) noexcept;

/// Computes render resolution from an output size and a quality mode, mirroring
/// AMD's per-dimension ratios.
Extent2D renderSizeForQuality(Extent2D output, QualityMode mode, float customRatio) noexcept;

/// Picks a quality preset for Auto based on output resolution. This is *this
/// project's* heuristic, not an AMD recommendation, and is labelled as such
/// wherever it surfaces.
QualityMode autoQualityForResolution(Extent2D output) noexcept;

} // namespace bfsr
