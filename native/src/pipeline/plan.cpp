// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/plan.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace bfsr {
namespace {

/// Headroom applied to AMD's own reported VRAM requirement before Auto enables
/// a feature. Leaves room for the game's own allocations to grow after we
/// measured.
constexpr double kVramHeadroomFactor = 1.35;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Picks the first entry of the requested class. AMD returns its preferred
/// provider first, so first-match is the right default rather than newest-wins.
const FfxVersionEntry* pickVersion(const std::vector<FfxVersionEntry>& versions, bool wantMl) {
    for (const auto& v : versions) {
        if (v.isMachineLearning == wantMl) return &v;
    }
    return nullptr;
}

const FfxVersionEntry* findVersionById(const std::vector<FfxVersionEntry>& versions,
                                       const std::string& idText) {
    if (idText.empty() || idText == "auto") return nullptr;
    char* end = nullptr;
    const unsigned long long id = std::strtoull(idText.c_str(), &end, 10);
    if (end == idText.c_str()) return nullptr;
    for (const auto& v : versions) {
        if (v.id == id) return &v;
    }
    return nullptr;
}

bool vramAllows(uint64_t available, uint64_t required) {
    if (required == 0) return true;   // Not measured yet — do not invent a refusal.
    if (available == 0) return true;  // VRAM unknown — same reasoning.
    return static_cast<double>(available) >= static_cast<double>(required) * kVramHeadroomFactor;
}

std::string bytesToMb(uint64_t bytes) {
    return std::to_string(bytes / (1024ull * 1024ull)) + " MB";
}

/// Both planners refuse for the same reason when the render inputs are absent,
/// and both must say so in terms a user can act on.
bool checkFrameInputs(const RuntimeContext& ctx, PlanNotes& notes) {
    if (ctx.frameInputsAvailable) return true;

    std::string missing;
    if (!ctx.depthAvailable) missing += "depth";
    if (!ctx.motionVectorsAvailable) {
        if (!missing.empty()) missing += " and ";
        missing += "motion vectors";
    }
    if (missing.empty()) missing = "the required render inputs";

    notes.downgrades.push_back(
        "FSR needs " + missing +
        " from BeamNG and they are not available yet. Check that the mod half is "
        "enabled and that Lighting Quality is set to Low or higher — BeamNG only "
        "generates motion vectors above that threshold. FSR stays off rather than "
        "running on substituted data.");
    return false;
}

std::string refusalText(const CapabilitySet& caps, Feature f) {
    const auto* a = caps.find(f);
    return std::string(displayName(f)) + " is unavailable. " +
           (a ? a->unavailableReason : std::string{});
}

} // namespace

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

const char* toString(FrameGenBackend b) noexcept {
    switch (b) {
        case FrameGenBackend::None: return "none";
        case FrameGenBackend::Analytical: return "analytical";
        case FrameGenBackend::Ml: return "ml";
    }
    return "none";
}

const char* toString(UpscalerBackend b) noexcept {
    switch (b) {
        case UpscalerBackend::None: return "none";
        case UpscalerBackend::Analytical: return "analytical";
        case UpscalerBackend::Ml: return "ml";
    }
    return "none";
}

float amdRecommendedMinimumBaseFps(FrameGenBackend backend) noexcept {
    switch (backend) {
        case FrameGenBackend::Ml: return kAmdMinBaseFpsMl;
        case FrameGenBackend::Analytical: return kAmdMinBaseFpsAnalytical;
        case FrameGenBackend::None: return 0.0f;
    }
    return 0.0f;
}

bool classifyFfxVersionAsMl(const std::string& versionName) noexcept {
    const std::string n = lower(versionName);
    if (n.find("4.") != std::string::npos) return true;
    if (n.find(" 4") != std::string::npos) return true;
    if (n.find("redstone") != std::string::npos) return true;
    if (n.find("ml") != std::string::npos) return true;
    return false;
}

Extent2D renderSizeForQuality(Extent2D output, QualityMode mode, float customRatio) noexcept {
    if (!output.valid()) return Extent2D{};
    float ratio = qualityModeRatio(mode);
    if (mode == QualityMode::Custom) ratio = customRatio;
    ratio = std::max(1.0f, std::min(ratio, 3.0f));

    Extent2D r;
    r.width = static_cast<uint32_t>(std::lround(static_cast<double>(output.width) / ratio));
    r.height = static_cast<uint32_t>(std::lround(static_cast<double>(output.height) / ratio));
    // Clamp to at least 32px so a degenerate display size cannot produce a
    // zero-extent resource.
    r.width = std::max(32u, std::min(r.width, output.width));
    r.height = std::max(32u, std::min(r.height, output.height));
    return r;
}

QualityMode autoQualityForResolution(Extent2D output) noexcept {
    const uint64_t px = output.pixels();
    // 4K tolerates aggressive ratios because the render resolution stays high in
    // absolute terms; 1080p does not.
    if (px >= 3840ull * 2160ull) return QualityMode::Performance;
    if (px >= 2560ull * 1440ull) return QualityMode::Balanced;
    if (px >= 1920ull * 1080ull) return QualityMode::Quality;
    return QualityMode::NativeAA;
}

// ---------------------------------------------------------------------------
// Frame generation planner
// ---------------------------------------------------------------------------

FrameGenPlan resolveFrameGenPlan(const FrameGenSettings& settings,
                                 const CapabilitySet& caps,
                                 const RuntimeContext& ctx) {
    FrameGenPlan plan;
    plan.displaySize = ctx.displaySize;
    // Compatibility mode disables async compute at plan time rather than by
    // rewriting the settings file, so switching it back off restores the
    // user's choice.
    plan.asyncCompute = settings.asyncCompute && !settings.diagnostics.forceCompatibilityMode;
    plan.uiComposition = settings.uiComposition;
    plan.antiLagMaxFps = settings.antiLagMaxFps;

    if (settings.safeMode) {
        plan.notes.downgrades.emplace_back(
            "Safe mode is active because a previous session did not shut down cleanly. "
            "Frame generation is disabled. Clear safe mode from the Advanced panel once "
            "you have checked the log.");
        return plan;
    }
    if (!settings.enabled || settings.implementation == FrameGenSelection::Disabled) {
        return plan; // Off by user request; nothing to explain.
    }
    if (!ctx.displaySize.valid()) {
        plan.notes.downgrades.emplace_back(
            "Output resolution is not known yet, so frame generation was not started.");
        return plan;
    }
    if (!checkFrameInputs(ctx, plan.notes)) return plan;

    const bool mlOk = caps.isAvailable(Feature::FrameGenerationMl);
    const bool anaOk = caps.isAvailable(Feature::FrameGenerationAnalytical);

    switch (settings.implementation) {
        case FrameGenSelection::Ml:
            if (mlOk) {
                plan.backend = FrameGenBackend::Ml;
            } else {
                plan.notes.downgrades.push_back(refusalText(caps, Feature::FrameGenerationMl));
                if (anaOk) {
                    plan.backend = FrameGenBackend::Analytical;
                    plan.notes.downgrades.emplace_back(
                        "Fell back to analytical FSR Frame Generation (3.1.x).");
                }
            }
            break;

        case FrameGenSelection::Analytical:
            if (anaOk) plan.backend = FrameGenBackend::Analytical;
            else plan.notes.downgrades.push_back(refusalText(caps, Feature::FrameGenerationAnalytical));
            break;

        case FrameGenSelection::Auto: {
            // Prefer ML: it is the newer implementation and AMD documents a
            // lower usable base frame rate for it (30 fps vs 60), so it is the
            // better choice wherever it is available.
            const FrameGenBackend preferred =
                mlOk ? FrameGenBackend::Ml : (anaOk ? FrameGenBackend::Analytical
                                                    : FrameGenBackend::None);
            if (preferred == FrameGenBackend::None) {
                plan.notes.downgrades.push_back(refusalText(caps, Feature::FrameGenerationAnalytical));
                break;
            }

            // AMD's own documented threshold for this implementation, not a
            // number invented here.
            const float minimum = amdRecommendedMinimumBaseFps(preferred);
            if (ctx.measuredBaseFps > 0.0f && ctx.measuredBaseFps < minimum) {
                plan.notes.decisions.push_back(
                    "Auto left frame generation off: the rendered frame rate is " +
                    std::to_string(static_cast<int>(ctx.measuredBaseFps)) +
                    " fps, below the " + std::to_string(static_cast<int>(minimum)) +
                    " fps AMD documents for " +
                    std::string(preferred == FrameGenBackend::Ml ? "ML" : "analytical") +
                    " frame generation, under which generated frames can show severe "
                    "artefacts. Choose an implementation explicitly to override this.");
                break;
            }

            if (!vramAllows(ctx.availableVramBytes, ctx.estimatedFrameGenVramBytes)) {
                plan.notes.decisions.push_back(
                    "Auto left frame generation off: it reports a requirement of " +
                    bytesToMb(ctx.estimatedFrameGenVramBytes) + " of video memory and only " +
                    bytesToMb(ctx.availableVramBytes) + " is available on this adapter.");
                break;
            }

            plan.backend = preferred;
            if (preferred == FrameGenBackend::Ml) {
                plan.notes.decisions.emplace_back(
                    "Auto chose ML FSR Frame Generation: this GPU, OS and driver meet AMD's "
                    "documented requirements (RX 9000 series, Windows 11).");
            } else {
                const auto* a = caps.find(Feature::FrameGenerationMl);
                plan.notes.decisions.push_back(
                    "Auto chose analytical FSR Frame Generation because the ML "
                    "implementation is not available: " +
                    (a ? a->unavailableReason : std::string("no reason recorded.")));
            }
            break;
        }

        case FrameGenSelection::Disabled:
            break;
    }

    if (!plan.active()) return plan;

    // --- version pinning ---
    {
        const bool wantMl = plan.backend == FrameGenBackend::Ml;
        const FfxVersionEntry* chosen =
            settings.diagnostics.forceCompatibilityMode
                ? nullptr
                : findVersionById(ctx.frameGenVersions, settings.versionId);
        if (chosen && chosen->isMachineLearning != wantMl) {
            plan.notes.downgrades.push_back(
                "The pinned frame-generation version '" + chosen->name +
                "' does not match the selected implementation type, so it was ignored.");
            chosen = nullptr;
        }
        if (!chosen) chosen = pickVersion(ctx.frameGenVersions, wantMl);
        if (chosen) {
            plan.versionId = chosen->id;
            plan.versionName = chosen->name;
        } else if (!ctx.frameGenVersions.empty()) {
            // AMD reported providers but none of the requested class. Stop
            // rather than silently running a different technology.
            plan.notes.downgrades.emplace_back(
                "The installed AMD FSR runtime offers no frame-generation provider of the "
                "requested type on this device, so frame generation was disabled.");
            plan.backend = FrameGenBackend::None;
            return plan;
        }
    }

    // --- Anti-Lag 2 ---
    if (settings.antiLag2) {
        if (caps.isAvailable(Feature::AntiLag2)) {
            plan.antiLag2 = true;
        } else {
            const auto* a = caps.find(Feature::AntiLag2);
            plan.notes.downgrades.push_back(std::string("Anti-Lag 2 was not enabled. ") +
                                            (a ? a->unavailableReason : std::string{}));
        }
    }

    // --- honest warnings ---
    plan.notes.warnings.emplace_back(
        "Frame generation raises the displayed frame rate but also input latency. The "
        "diagnostics panel reports rendered and displayed frame rates separately; they "
        "are not equivalent.");

    const float minimum = amdRecommendedMinimumBaseFps(plan.backend);
    if (ctx.measuredBaseFps > 0.0f && ctx.measuredBaseFps < minimum) {
        plan.notes.warnings.push_back(
            "The rendered frame rate is " + std::to_string(static_cast<int>(ctx.measuredBaseFps)) +
            " fps, below the " + std::to_string(static_cast<int>(minimum)) +
            " fps AMD documents for this implementation. Generated frames may show "
            "noticeable artefacts.");
    }
    if (!plan.antiLag2 && caps.isAvailable(Feature::AntiLag2)) {
        plan.notes.warnings.emplace_back(
            "AMD recommends pairing frame generation with Anti-Lag 2 to offset the added "
            "latency. It is available on this system but switched off.");
    }

    return plan;
}

// ---------------------------------------------------------------------------
// Upscaling planner
// ---------------------------------------------------------------------------

UpscalePlan resolveUpscalePlan(const UpscaleSettings& settings,
                               const CapabilitySet& caps,
                               const RuntimeContext& ctx) {
    UpscalePlan plan;
    plan.upscaleSize = ctx.displaySize;
    plan.sharpness = settings.sharpness;
    plan.sharpeningEnabled = settings.sharpeningEnabled;
    plan.quality = settings.quality;

    if (settings.safeMode) {
        plan.notes.downgrades.emplace_back(
            "Safe mode is active because a previous session did not shut down cleanly. "
            "Upscaling is disabled. Clear safe mode from the Advanced panel once you have "
            "checked the log.");
        return plan;
    }
    if (!settings.enabled || settings.implementation == UpscalerSelection::Disabled) {
        return plan;
    }
    if (!ctx.displaySize.valid()) {
        plan.notes.downgrades.emplace_back(
            "Output resolution is not known yet, so upscaling was not started.");
        return plan;
    }
    if (!checkFrameInputs(ctx, plan.notes)) return plan;

    const bool mlOk = caps.isAvailable(Feature::UpscalingMl);
    const bool anaOk = caps.isAvailable(Feature::UpscalingAnalytical);

    switch (settings.implementation) {
        case UpscalerSelection::Ml:
            if (mlOk) {
                plan.backend = UpscalerBackend::Ml;
            } else {
                plan.notes.downgrades.push_back(refusalText(caps, Feature::UpscalingMl));
                if (anaOk) {
                    plan.backend = UpscalerBackend::Analytical;
                    plan.notes.downgrades.emplace_back(
                        "Fell back to analytical FSR upscaling (FSR 3.1.x).");
                }
            }
            break;

        case UpscalerSelection::Analytical:
            if (anaOk) plan.backend = UpscalerBackend::Analytical;
            else plan.notes.downgrades.push_back(refusalText(caps, Feature::UpscalingAnalytical));
            break;

        case UpscalerSelection::Auto:
            if (mlOk) {
                plan.backend = UpscalerBackend::Ml;
                plan.notes.decisions.emplace_back(
                    "Auto chose ML FSR Upscaling because this GPU and driver report support "
                    "for it, and AMD documents it as the higher-quality implementation.");
            } else if (anaOk) {
                plan.backend = UpscalerBackend::Analytical;
                const auto* a = caps.find(Feature::UpscalingMl);
                plan.notes.decisions.push_back(
                    "Auto chose analytical FSR upscaling (FSR 3.1.x) because the ML "
                    "implementation is not available: " +
                    (a ? a->unavailableReason : std::string("no reason recorded.")));
            } else {
                plan.notes.downgrades.push_back(refusalText(caps, Feature::UpscalingAnalytical));
            }
            break;

        case UpscalerSelection::Disabled:
            break;
    }

    if (!plan.active()) return plan;

    if (!vramAllows(ctx.availableVramBytes, ctx.estimatedUpscalerVramBytes)) {
        plan.notes.downgrades.push_back(
            "Disabled upscaling: it reports a requirement of " +
            bytesToMb(ctx.estimatedUpscalerVramBytes) + " of video memory and only " +
            bytesToMb(ctx.availableVramBytes) + " is available.");
        plan.backend = UpscalerBackend::None;
        return plan;
    }

    // --- version pinning ---
    {
        const bool wantMl = plan.backend == UpscalerBackend::Ml;
        const FfxVersionEntry* chosen =
            settings.diagnostics.forceCompatibilityMode
                ? nullptr
                : findVersionById(ctx.upscalerVersions, settings.versionId);
        if (chosen && chosen->isMachineLearning != wantMl) {
            plan.notes.downgrades.push_back(
                "The pinned upscaler version '" + chosen->name +
                "' does not match the selected implementation type, so it was ignored.");
            chosen = nullptr;
        }
        if (!chosen) chosen = pickVersion(ctx.upscalerVersions, wantMl);
        if (chosen) {
            plan.versionId = chosen->id;
            plan.versionName = chosen->name;
        } else if (!ctx.upscalerVersions.empty()) {
            plan.notes.downgrades.emplace_back(
                "The installed AMD FSR runtime offers no upscaler provider of the requested "
                "type on this device, so upscaling was disabled.");
            plan.backend = UpscalerBackend::None;
            return plan;
        }
    }

    // --- resolution ---
    if (settings.implementation == UpscalerSelection::Auto &&
        settings.quality != QualityMode::Custom) {
        plan.quality = autoQualityForResolution(ctx.displaySize);
        plan.notes.decisions.push_back(
            std::string("Auto selected the '") + toString(plan.quality) +
            "' quality preset for a " + std::to_string(ctx.displaySize.width) + "x" +
            std::to_string(ctx.displaySize.height) +
            " output. This is a heuristic of this mod, not an AMD recommendation.");
    }
    plan.renderSize = renderSizeForQuality(ctx.displaySize, plan.quality,
                                           settings.customUpscaleRatio);

    // --- honest warnings ---
    if (!ctx.cameraJitterAvailable) {
        plan.notes.warnings.emplace_back(
            "BeamNG does not expose a camera-jitter hook to mods, so FSR is running without "
            "the sub-pixel jitter its temporal upscaler is designed around. It will still "
            "reconstruct and stabilise the image, but it cannot recover sub-pixel detail the "
            "way a native FSR integration does. This is a limitation of what BeamNG exposes, "
            "not a setting you can fix.");
    }
    if (ctx.sceneRenderSize.valid() && ctx.sceneRenderSize == ctx.displaySize &&
        plan.renderSize != ctx.displaySize) {
        plan.notes.warnings.emplace_back(
            "BeamNG is still rendering at full output resolution, so upscaling cannot save "
            "any GPU time here. To gain performance, lower BeamNG's own resolution and let "
            "FSR upscale back to your display resolution.");
    }

    return plan;
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

namespace {

Json notesToJson(const PlanNotes& notes) {
    const auto arrayOf = [](const std::vector<std::string>& v) {
        Json a = Json::array();
        for (const auto& s : v) a.push(Json(s));
        return a;
    };
    Json j = Json::object();
    j.set("warnings", arrayOf(notes.warnings));
    j.set("downgrades", arrayOf(notes.downgrades));
    j.set("decisions", arrayOf(notes.decisions));
    return j;
}

} // namespace

Json FrameGenPlan::toJson() const {
    Json j = Json::object();
    j.set("backend", Json(std::string(toString(backend))));
    j.set("active", Json(active()));
    j.set("antiLag2", Json(antiLag2));
    j.set("antiLagMaxFps", Json(static_cast<unsigned long long>(antiLagMaxFps)));
    j.set("asyncCompute", Json(asyncCompute));
    j.set("uiComposition", Json(uiComposition));
    j.set("versionId", Json(std::to_string(versionId)));
    j.set("versionName", Json(versionName));
    j.set("displayWidth", Json(static_cast<unsigned long long>(displaySize.width)));
    j.set("displayHeight", Json(static_cast<unsigned long long>(displaySize.height)));
    j.set("amdMinimumBaseFps",
          Json(static_cast<double>(amdRecommendedMinimumBaseFps(backend))));
    j.set("notes", notesToJson(notes));
    return j;
}

Json UpscalePlan::toJson() const {
    Json j = Json::object();
    j.set("backend", Json(std::string(toString(backend))));
    j.set("active", Json(active()));
    j.set("versionId", Json(std::to_string(versionId)));
    j.set("versionName", Json(versionName));
    j.set("quality", Json(std::string(toString(quality))));
    j.set("renderWidth", Json(static_cast<unsigned long long>(renderSize.width)));
    j.set("renderHeight", Json(static_cast<unsigned long long>(renderSize.height)));
    j.set("outputWidth", Json(static_cast<unsigned long long>(upscaleSize.width)));
    j.set("outputHeight", Json(static_cast<unsigned long long>(upscaleSize.height)));
    j.set("sharpeningEnabled", Json(sharpeningEnabled));
    j.set("sharpness", Json(static_cast<double>(sharpness)));
    j.set("notes", notesToJson(notes));
    return j;
}

} // namespace bfsr
