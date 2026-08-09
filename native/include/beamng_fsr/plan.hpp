// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
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
    std::string name;        ///< AMD's display string, e.g. "FSR 3.1.5".
    bool isMachineLearning = false; ///< Derived from the name; see classifyFfxVersion.
};

/// Everything the planner needs that is not a user preference.
struct RuntimeContext {
    Extent2D displaySize;
    /// The resolution BeamNG is actually rendering its 3D scene at. Equal to
    /// displaySize unless the user lowered the game's own resolution — the
    /// bridge cannot change this itself (FEASIBILITY §6.4).
    Extent2D sceneRenderSize;
    uint64_t availableVramBytes = 0;
    /// Bytes the FSR runtime reported it needs, via
    /// ffxQueryDescUpscaleGetGPUMemoryUsageV2 / the frame-generation equivalent.
    /// Zero means "not yet queried", which the planner treats as unknown rather
    /// than as zero cost.
    uint64_t estimatedUpscalerVramBytes = 0;
    uint64_t estimatedFrameGenVramBytes = 0;
    /// True once the mod half's beacon (or the heuristic fallback) has produced
    /// usable depth and motion vectors. Without both, nothing temporal can run.
    bool frameInputsAvailable = false;
    bool motionVectorsAvailable = false;
    bool depthAvailable = false;
    /// Always false today: BeamNG exposes no projection-jitter hook.
    /// Kept as a field so the planner's behaviour is testable for the day it changes.
    bool cameraJitterAvailable = false;
    /// Smoothed rendered (not displayed) frame rate; 0 when not yet measured.
    float measuredBaseFps = 0.0f;
    std::vector<FfxVersionEntry> upscalerVersions;
    std::vector<FfxVersionEntry> frameGenVersions;
};

enum class UpscalerBackend { None, Analytical, Ml };
enum class FrameGenBackend { None, Analytical, Ml };

const char* toString(UpscalerBackend b) noexcept;
const char* toString(FrameGenBackend b) noexcept;

/// The concrete configuration the runtime will actually execute, together with
/// a human-readable justification for every decision. The UI shows these; the
/// brief's requirement that unavailable options be *explained* rather than
/// silently dropped is why the reasons are part of the plan, not log spam.
struct ResolvedPlan {
    UpscalerBackend upscaler = UpscalerBackend::None;
    FrameGenBackend frameGen = FrameGenBackend::None;
    bool antiLag2 = false;

    uint64_t upscalerVersionId = 0; ///< 0 == let AMD choose the default provider.
    uint64_t frameGenVersionId = 0;
    std::string upscalerVersionName;
    std::string frameGenVersionName;

    QualityMode quality = QualityMode::Quality;
    Extent2D renderSize;   ///< What FSR is told the inputs are.
    Extent2D upscaleSize;  ///< What FSR outputs.
    float sharpness = 0.5f;
    bool sharpeningEnabled = true;

    /// Non-fatal notes the user should see (e.g. the jitter limitation).
    std::vector<std::string> warnings;
    /// Why each requested-but-not-granted feature was refused.
    std::vector<std::string> downgrades;
    /// Why Auto picked what it picked.
    std::vector<std::string> decisions;

    [[nodiscard]] bool anythingActive() const noexcept {
        return upscaler != UpscalerBackend::None || frameGen != FrameGenBackend::None;
    }
    [[nodiscard]] Json toJson() const;
};

/// Classifies an AMD version string as ML or analytical.
///
/// AMD does not expose a machine-readable "is ML" flag on the version query, so
/// this parses the display name: FSR *Upscaling* 4.x and FSR *Frame Generation*
/// 4.x are the ML implementations, 3.1.x/2.x are analytical (FEASIBILITY §4.1).
/// Unrecognised names default to analytical, i.e. the conservative answer.
bool classifyFfxVersionAsMl(const std::string& versionName) noexcept;

/// Computes render resolution from an output size and a quality mode.
/// Mirrors AMD's per-dimension ratios and rounds the way the SDK does.
Extent2D renderSizeForQuality(Extent2D output, QualityMode mode, float customScale) noexcept;

/// Picks a quality preset for Auto based on output resolution.
/// This is *this project's* heuristic, not an AMD recommendation, and is
/// labelled as such wherever it surfaces.
QualityMode autoQualityForResolution(Extent2D output) noexcept;

/// The planner. Pure and deterministic: same inputs, same plan. All the
/// interesting failure behaviour of the mod is decided here, which is why it is
/// isolated from every platform API and covered by unit tests.
ResolvedPlan resolvePlan(const Settings& settings,
                         const CapabilitySet& caps,
                         const RuntimeContext& ctx);

} // namespace bfsr
