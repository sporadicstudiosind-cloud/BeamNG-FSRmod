// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/plan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>

namespace bfsr {
namespace {

/// Below this rendered frame rate, interpolation has too little temporal
/// information to work with and the added latency dominates the smoothness
/// gain. This is a heuristic chosen by this project, not an AMD threshold, and
/// Auto is the only mode that applies it — an explicit user choice is honoured.
constexpr float kAutoFrameGenMinBaseFps = 45.0f;

/// Headroom multiplier applied to AMD's own reported VRAM requirement before
/// Auto enables a feature. Leaves room for the game's own allocations to grow
/// after we measured.
constexpr double kVramHeadroomFactor = 1.35;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Finds the newest entry matching a predicate, using AMD's own ordering
/// (the query returns the preferred provider first).
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

} // namespace

const char* toString(UpscalerBackend b) noexcept {
    switch (b) {
        case UpscalerBackend::None: return "none";
        case UpscalerBackend::Analytical: return "analytical";
        case UpscalerBackend::Ml: return "ml";
    }
    return "none";
}

const char* toString(FrameGenBackend b) noexcept {
    switch (b) {
        case FrameGenBackend::None: return "none";
        case FrameGenBackend::Analytical: return "analytical";
        case FrameGenBackend::Ml: return "ml";
    }
    return "none";
}

bool classifyFfxVersionAsMl(const std::string& versionName) noexcept {
    const std::string n = lower(versionName);
    // AMD's ML implementations are the 4.x lines of both the upscaler and frame
    // generation. Anything 3.x or 2.x is analytical.
    if (n.find("4.") != std::string::npos) return true;
    if (n.find(" 4") != std::string::npos) return true;
    if (n.find("redstone") != std::string::npos) return true;
    if (n.find("ml") != std::string::npos) return true;
    return false;
}

Extent2D renderSizeForQuality(Extent2D output, QualityMode mode, float customScale) noexcept {
    if (!output.valid()) return Extent2D{};
    float ratio = qualityModeRatio(mode);
    if (mode == QualityMode::Custom) ratio = customScale;
    if (ratio < 1.0f) ratio = 1.0f;
    if (ratio > 3.0f) ratio = 3.0f;

    Extent2D r;
    // Round to nearest, then clamp to at least 32px so a degenerate display size
    // cannot produce a zero-extent resource.
    r.width = static_cast<uint32_t>(std::lround(static_cast<double>(output.width) / ratio));
    r.height = static_cast<uint32_t>(std::lround(static_cast<double>(output.height) / ratio));
    r.width = std::max(32u, std::min(r.width, output.width));
    r.height = std::max(32u, std::min(r.height, output.height));
    return r;
}

QualityMode autoQualityForResolution(Extent2D output) noexcept {
    const uint64_t px = output.pixels();
    // 4K and above tolerates aggressive ratios because the render resolution
    // stays high in absolute terms; 1080p does not.
    if (px >= 3840ull * 2160ull) return QualityMode::Performance;
    if (px >= 2560ull * 1440ull) return QualityMode::Balanced;
    if (px >= 1920ull * 1080ull) return QualityMode::Quality;
    return QualityMode::NativeAA;
}

ResolvedPlan resolvePlan(const Settings& settings,
                         const CapabilitySet& caps,
                         const RuntimeContext& ctx) {
    ResolvedPlan plan;
    plan.upscaleSize = ctx.displaySize;
    plan.sharpness = settings.sharpness;
    plan.sharpeningEnabled = settings.sharpeningEnabled;

    // ---- hard stops --------------------------------------------------------
    if (settings.safeMode) {
        plan.downgrades.emplace_back(
            "Safe mode is active because a previous session did not shut down cleanly. "
            "All FSR features are disabled. Clear safe mode from the Advanced panel "
            "once you have checked the log.");
        return plan;
    }
    if (!settings.enabled) {
        return plan; // Off by user request; nothing to explain.
    }
    if (!ctx.displaySize.valid()) {
        plan.downgrades.emplace_back(
            "Output resolution is not known yet, so no FSR feature was started.");
        return plan;
    }

    const bool wantUpscale = settings.upscaler != UpscalerSelection::Disabled;
    const bool wantFrameGen = settings.frameGeneration != FrameGenSelection::Disabled;

    if ((wantUpscale || wantFrameGen) && !ctx.frameInputsAvailable) {
        std::string missing;
        if (!ctx.depthAvailable) missing += "depth";
        if (!ctx.motionVectorsAvailable) {
            if (!missing.empty()) missing += " and ";
            missing += "motion vectors";
        }
        if (missing.empty()) missing = "the required render inputs";
        plan.downgrades.push_back(
            "FSR needs " + missing +
            " from BeamNG and they are not available yet. Check that the BeamNG FSR "
            "mod is enabled and that Lighting Quality is set to Low or higher — "
            "BeamNG only generates motion vectors above that threshold. FSR stays "
            "off rather than running on substituted data.");
        return plan;
    }

    // ---- upscaler selection ------------------------------------------------
    if (wantUpscale) {
        const bool mlOk = caps.isAvailable(Feature::UpscalingMl);
        const bool anaOk = caps.isAvailable(Feature::UpscalingAnalytical);

        auto refuse = [&](Feature f) {
            const auto* a = caps.find(f);
            plan.downgrades.push_back(std::string(displayName(f)) + " is unavailable. " +
                                      (a ? a->unavailableReason : std::string{}));
        };

        switch (settings.upscaler) {
            case UpscalerSelection::Ml:
                if (mlOk) {
                    plan.upscaler = UpscalerBackend::Ml;
                } else {
                    refuse(Feature::UpscalingMl);
                    if (anaOk) {
                        plan.upscaler = UpscalerBackend::Analytical;
                        plan.downgrades.emplace_back(
                            "Fell back to analytical FSR upscaling (FSR 3.1.x).");
                    }
                }
                break;
            case UpscalerSelection::Analytical:
                if (anaOk) plan.upscaler = UpscalerBackend::Analytical;
                else refuse(Feature::UpscalingAnalytical);
                break;
            case UpscalerSelection::Auto:
                // Prefer ML: AMD positions FSR Upscaling 4.x as the higher-quality
                // successor to 3.1.x. Fall back rather than refuse.
                if (mlOk) {
                    plan.upscaler = UpscalerBackend::Ml;
                    plan.decisions.emplace_back(
                        "Auto chose ML FSR Upscaling because this GPU and driver report "
                        "support for it, and AMD documents it as the higher-quality "
                        "implementation.");
                } else if (anaOk) {
                    plan.upscaler = UpscalerBackend::Analytical;
                    const auto* a = caps.find(Feature::UpscalingMl);
                    plan.decisions.push_back(
                        "Auto chose analytical FSR upscaling (FSR 3.1.x) because the ML "
                        "implementation is not available: " +
                        (a ? a->unavailableReason : std::string("no reason recorded.")));
                } else {
                    refuse(Feature::UpscalingAnalytical);
                }
                break;
            case UpscalerSelection::Disabled:
                break;
        }
    }

    // ---- frame generation selection ----------------------------------------
    if (wantFrameGen) {
        const bool mlOk = caps.isAvailable(Feature::FrameGenerationMl);
        const bool anaOk = caps.isAvailable(Feature::FrameGenerationAnalytical);

        auto refuse = [&](Feature f) {
            const auto* a = caps.find(f);
            plan.downgrades.push_back(std::string(displayName(f)) + " is unavailable. " +
                                      (a ? a->unavailableReason : std::string{}));
        };

        switch (settings.frameGeneration) {
            case FrameGenSelection::Ml:
                if (mlOk) {
                    plan.frameGen = FrameGenBackend::Ml;
                } else {
                    refuse(Feature::FrameGenerationMl);
                    if (anaOk) {
                        plan.frameGen = FrameGenBackend::Analytical;
                        plan.downgrades.emplace_back(
                            "Fell back to analytical FSR Frame Generation (3.1.x).");
                    }
                }
                break;
            case FrameGenSelection::Analytical:
                if (anaOk) plan.frameGen = FrameGenBackend::Analytical;
                else refuse(Feature::FrameGenerationAnalytical);
                break;
            case FrameGenSelection::Auto:
                if (ctx.measuredBaseFps > 0.0f && ctx.measuredBaseFps < kAutoFrameGenMinBaseFps) {
                    plan.decisions.push_back(
                        "Auto left frame generation off: the rendered frame rate is " +
                        std::to_string(static_cast<int>(ctx.measuredBaseFps)) +
                        " fps, below this mod's " +
                        std::to_string(static_cast<int>(kAutoFrameGenMinBaseFps)) +
                        " fps threshold, where interpolation costs more in latency than "
                        "it returns in smoothness. Select an implementation explicitly "
                        "to override.");
                } else if (mlOk && vramAllows(ctx.availableVramBytes, ctx.estimatedFrameGenVramBytes)) {
                    plan.frameGen = FrameGenBackend::Ml;
                    plan.decisions.emplace_back(
                        "Auto chose ML FSR Frame Generation: this GPU, OS and driver meet "
                        "AMD's documented requirements (RX 9000 series, Windows 11).");
                } else if (anaOk && vramAllows(ctx.availableVramBytes, ctx.estimatedFrameGenVramBytes)) {
                    plan.frameGen = FrameGenBackend::Analytical;
                    const auto* a = caps.find(Feature::FrameGenerationMl);
                    plan.decisions.push_back(
                        "Auto chose analytical FSR Frame Generation because the ML "
                        "implementation is not available: " +
                        (a ? a->unavailableReason : std::string("no reason recorded.")));
                } else if ((mlOk || anaOk) &&
                           !vramAllows(ctx.availableVramBytes, ctx.estimatedFrameGenVramBytes)) {
                    plan.decisions.push_back(
                        "Auto left frame generation off: it reports a requirement of " +
                        bytesToMb(ctx.estimatedFrameGenVramBytes) + " of video memory and only " +
                        bytesToMb(ctx.availableVramBytes) + " is available on this adapter.");
                } else {
                    refuse(Feature::FrameGenerationAnalytical);
                }
                break;
            case FrameGenSelection::Disabled:
                break;
        }
    }

    // ---- VRAM guard on the upscaler ----------------------------------------
    if (plan.upscaler != UpscalerBackend::None &&
        !vramAllows(ctx.availableVramBytes, ctx.estimatedUpscalerVramBytes)) {
        plan.downgrades.push_back(
            "Disabled FSR upscaling: it reports a requirement of " +
            bytesToMb(ctx.estimatedUpscalerVramBytes) + " of video memory and only " +
            bytesToMb(ctx.availableVramBytes) + " is available.");
        plan.upscaler = UpscalerBackend::None;
    }

    // ---- version pinning ---------------------------------------------------
    if (plan.upscaler != UpscalerBackend::None) {
        const bool wantMl = plan.upscaler == UpscalerBackend::Ml;
        const FfxVersionEntry* chosen =
            settings.forceCompatibilityMode ? nullptr
                                            : findVersionById(ctx.upscalerVersions,
                                                              settings.upscalerVersionId);
        if (chosen && chosen->isMachineLearning != wantMl) {
            plan.downgrades.push_back(
                "The pinned upscaler version '" + chosen->name +
                "' does not match the selected implementation type, so it was ignored.");
            chosen = nullptr;
        }
        if (!chosen) chosen = pickVersion(ctx.upscalerVersions, wantMl);
        if (chosen) {
            plan.upscalerVersionId = chosen->id;
            plan.upscalerVersionName = chosen->name;
        } else if (!ctx.upscalerVersions.empty()) {
            // AMD reported providers but none of the requested class. Better to
            // stop than to silently run a different technology than advertised.
            plan.downgrades.emplace_back(
                "The installed AMD FSR runtime offers no upscaler provider of the "
                "requested type on this device, so upscaling was disabled.");
            plan.upscaler = UpscalerBackend::None;
        }
    }

    if (plan.frameGen != FrameGenBackend::None) {
        const bool wantMl = plan.frameGen == FrameGenBackend::Ml;
        const FfxVersionEntry* chosen =
            settings.forceCompatibilityMode ? nullptr
                                            : findVersionById(ctx.frameGenVersions,
                                                              settings.frameGenVersionId);
        if (chosen && chosen->isMachineLearning != wantMl) {
            plan.downgrades.push_back(
                "The pinned frame-generation version '" + chosen->name +
                "' does not match the selected implementation type, so it was ignored.");
            chosen = nullptr;
        }
        if (!chosen) chosen = pickVersion(ctx.frameGenVersions, wantMl);
        if (chosen) {
            plan.frameGenVersionId = chosen->id;
            plan.frameGenVersionName = chosen->name;
        } else if (!ctx.frameGenVersions.empty()) {
            plan.downgrades.emplace_back(
                "The installed AMD FSR runtime offers no frame-generation provider of "
                "the requested type on this device, so frame generation was disabled.");
            plan.frameGen = FrameGenBackend::None;
        }
    }

    // ---- resolution --------------------------------------------------------
    plan.quality = settings.quality;
    if (plan.upscaler != UpscalerBackend::None) {
        if (settings.upscaler == UpscalerSelection::Auto && settings.quality != QualityMode::Custom) {
            plan.quality = autoQualityForResolution(ctx.displaySize);
            plan.decisions.push_back(
                std::string("Auto selected the '") + toString(plan.quality) +
                "' quality preset for a " + std::to_string(ctx.displaySize.width) + "x" +
                std::to_string(ctx.displaySize.height) +
                " output. This is a heuristic of this mod, not an AMD recommendation.");
        }
        plan.renderSize = renderSizeForQuality(ctx.displaySize, plan.quality,
                                               settings.customUpscaleRatio);
    } else {
        plan.renderSize = ctx.sceneRenderSize.valid() ? ctx.sceneRenderSize : ctx.displaySize;
    }

    // ---- Anti-Lag 2 --------------------------------------------------------
    if (settings.antiLag2) {
        if (caps.isAvailable(Feature::AntiLag2)) {
            plan.antiLag2 = true;
        } else {
            const auto* a = caps.find(Feature::AntiLag2);
            plan.downgrades.push_back(std::string("Anti-Lag 2 was not enabled. ") +
                                      (a ? a->unavailableReason : std::string{}));
        }
    }

    // ---- honest warnings ---------------------------------------------------
    if (plan.upscaler != UpscalerBackend::None && !ctx.cameraJitterAvailable) {
        plan.warnings.emplace_back(
            "BeamNG does not expose a camera-jitter hook to mods, so FSR is running "
            "without the sub-pixel jitter its temporal upscaler is designed around. "
            "It will still reconstruct and stabilise the image, but it cannot recover "
            "sub-pixel detail the way a native FSR integration does. This is a "
            "limitation of what BeamNG exposes, not a setting you can fix.");
    }
    if (plan.upscaler != UpscalerBackend::None && ctx.sceneRenderSize.valid() &&
        ctx.sceneRenderSize == ctx.displaySize && plan.renderSize != ctx.displaySize) {
        plan.warnings.emplace_back(
            "BeamNG is still rendering at full output resolution, so upscaling cannot "
            "save any GPU time here. To gain performance, lower BeamNG's own resolution "
            "and let FSR upscale back to your display resolution.");
    }
    if (plan.frameGen != FrameGenBackend::None) {
        plan.warnings.emplace_back(
            "Frame generation increases displayed frame rate but also input latency. "
            "The diagnostics panel reports rendered and displayed frame rates "
            "separately; they are not equivalent.");
    }

    return plan;
}

Json ResolvedPlan::toJson() const {
    Json j = Json::object();
    j.set("upscaler", Json(std::string(toString(upscaler))));
    j.set("frameGeneration", Json(std::string(toString(frameGen))));
    j.set("antiLag2", Json(antiLag2));
    j.set("upscalerVersionId", Json(static_cast<unsigned long long>(upscalerVersionId)));
    j.set("upscalerVersionName", Json(upscalerVersionName));
    j.set("frameGenVersionId", Json(static_cast<unsigned long long>(frameGenVersionId)));
    j.set("frameGenVersionName", Json(frameGenVersionName));
    j.set("quality", Json(std::string(toString(quality))));
    j.set("renderWidth", Json(static_cast<unsigned long long>(renderSize.width)));
    j.set("renderHeight", Json(static_cast<unsigned long long>(renderSize.height)));
    j.set("outputWidth", Json(static_cast<unsigned long long>(upscaleSize.width)));
    j.set("outputHeight", Json(static_cast<unsigned long long>(upscaleSize.height)));
    j.set("sharpeningEnabled", Json(sharpeningEnabled));
    j.set("sharpness", Json(static_cast<double>(sharpness)));

    Json w = Json::array();
    for (const auto& s : warnings) w.push(Json(s));
    j.set("warnings", std::move(w));

    Json d = Json::array();
    for (const auto& s : downgrades) d.push(Json(s));
    j.set("downgrades", std::move(d));

    Json dec = Json::array();
    for (const auto& s : decisions) dec.push(Json(s));
    j.set("decisions", std::move(dec));
    return j;
}

} // namespace bfsr
