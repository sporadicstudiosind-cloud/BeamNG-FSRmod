// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include "beamng_fsr/plan.hpp"

using namespace bfsr;

namespace {

CapabilitySet allAvailable() {
    CapabilitySet c;
    for (Feature f : {Feature::UpscalingAnalytical, Feature::UpscalingMl,
                      Feature::FrameGenerationAnalytical, Feature::FrameGenerationMl,
                      Feature::AntiLag2}) {
        FeatureAvailability a;
        a.feature = f;
        a.available = true;
        a.requirement = "test";
        c.features.push_back(a);
    }
    return c;
}

CapabilitySet analyticalOnly() {
    CapabilitySet c = allAvailable();
    for (auto& f : c.features) {
        if (f.feature == Feature::UpscalingMl || f.feature == Feature::FrameGenerationMl) {
            f.available = false;
            f.unavailableReason = "Requires a Radeon RX 9000 series GPU for this test.";
        }
    }
    return c;
}

RuntimeContext readyContext() {
    RuntimeContext ctx;
    ctx.displaySize = {2560, 1440};
    ctx.sceneRenderSize = {2560, 1440};
    ctx.availableVramBytes = 16ull << 30;
    ctx.frameInputsAvailable = true;
    ctx.depthAvailable = true;
    ctx.motionVectorsAvailable = true;
    ctx.cameraJitterAvailable = false; // BeamNG reality
    ctx.measuredBaseFps = 90.0f;
    ctx.upscalerVersions = {
        {1001, "FSR Upscaling 4.1.1", true},
        {1002, "FSR 3.1.5", false},
    };
    ctx.frameGenVersions = {
        {2001, "FSR Frame Generation 4.0.1", true},
        {2002, "FSR Frame Generation 3.1.6", false},
    };
    return ctx;
}

Settings enabledSettings() {
    Settings s;
    s.enabled = true;
    return s;
}

} // namespace

TEST(Plan, DisabledByDefaultProducesNothing) {
    const auto plan = resolvePlan(Settings{}, allAvailable(), readyContext());
    CHECK(!plan.anythingActive());
    CHECK(plan.downgrades.empty());
}

TEST(Plan, SafeModeOverridesEverythingAndExplains) {
    Settings s = enabledSettings();
    s.safeMode = true;
    s.upscaler = UpscalerSelection::Ml;
    s.frameGeneration = FrameGenSelection::Ml;
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    CHECK(!plan.anythingActive());
    CHECK_EQ(plan.downgrades.size(), size_t{1});
    CHECK_CONTAINS(plan.downgrades[0], "Safe mode");
}

TEST(Plan, MissingRenderInputsRefusesRatherThanSubstituting) {
    RuntimeContext ctx = readyContext();
    ctx.frameInputsAvailable = false;
    ctx.motionVectorsAvailable = false;
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Auto;
    const auto plan = resolvePlan(s, allAvailable(), ctx);
    CHECK(!plan.anythingActive());
    CHECK_CONTAINS(plan.downgrades[0], "motion vectors");
    CHECK_CONTAINS(plan.downgrades[0], "Lighting Quality");
}

TEST(Plan, AutoPrefersMlUpscalingWhenAvailable) {
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Auto;
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    CHECK(plan.upscaler == UpscalerBackend::Ml);
    CHECK_EQ(plan.upscalerVersionId, uint64_t{1001});
    CHECK(!plan.decisions.empty());
}

TEST(Plan, AutoFallsBackToAnalyticalAndSaysWhy) {
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Auto;
    const auto plan = resolvePlan(s, analyticalOnly(), readyContext());
    CHECK(plan.upscaler == UpscalerBackend::Analytical);
    CHECK_EQ(plan.upscalerVersionId, uint64_t{1002});
    CHECK_CONTAINS(plan.decisions[0], "RX 9000");
}

TEST(Plan, ExplicitMlRequestOnUnsupportedHardwareDowngradesWithReason) {
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Ml;
    const auto plan = resolvePlan(s, analyticalOnly(), readyContext());
    CHECK(plan.upscaler == UpscalerBackend::Analytical);
    CHECK_EQ(plan.downgrades.size(), size_t{2});
    CHECK_CONTAINS(plan.downgrades[0], "unavailable");
    CHECK_CONTAINS(plan.downgrades[1], "Fell back");
}

TEST(Plan, JitterWarningIsAlwaysPresentWhenUpscaling) {
    // BeamNG exposes no jitter hook; the user must be told, every time.
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Auto;
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    bool found = false;
    for (const auto& w : plan.warnings) {
        if (w.find("jitter") != std::string::npos) found = true;
    }
    CHECK(found);
}

TEST(Plan, NoJitterWarningWhenJitterBecomesAvailable) {
    // Guards the day BeamNG (or a future API) provides one.
    RuntimeContext ctx = readyContext();
    ctx.cameraJitterAvailable = true;
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Auto;
    const auto plan = resolvePlan(s, allAvailable(), ctx);
    for (const auto& w : plan.warnings) {
        CHECK(w.find("jitter") == std::string::npos);
    }
}

TEST(Plan, WarnsWhenUpscalingCannotSaveTime) {
    // Scene already rendering at output resolution: upscaling buys nothing.
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Analytical;
    s.quality = QualityMode::Performance;
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    bool found = false;
    for (const auto& w : plan.warnings) {
        if (w.find("lower BeamNG's own resolution") != std::string::npos) found = true;
    }
    CHECK(found);
}

TEST(Plan, AutoSkipsFrameGenAtLowBaseFps) {
    RuntimeContext ctx = readyContext();
    ctx.measuredBaseFps = 30.0f;
    Settings s = enabledSettings();
    s.frameGeneration = FrameGenSelection::Auto;
    const auto plan = resolvePlan(s, allAvailable(), ctx);
    CHECK(plan.frameGen == FrameGenBackend::None);
    CHECK_CONTAINS(plan.decisions[0], "30 fps");
}

TEST(Plan, ExplicitFrameGenRequestIsHonouredAtLowFps) {
    // The threshold is Auto's opinion, not a veto over an explicit choice.
    RuntimeContext ctx = readyContext();
    ctx.measuredBaseFps = 30.0f;
    Settings s = enabledSettings();
    s.frameGeneration = FrameGenSelection::Ml;
    const auto plan = resolvePlan(s, allAvailable(), ctx);
    CHECK(plan.frameGen == FrameGenBackend::Ml);
}

TEST(Plan, VramGuardUsesTheRuntimeReportedRequirement) {
    RuntimeContext ctx = readyContext();
    ctx.availableVramBytes = 1ull << 30;          // 1 GB free
    ctx.estimatedFrameGenVramBytes = 900ull << 20; // 900 MB needed, over headroom
    Settings s = enabledSettings();
    s.frameGeneration = FrameGenSelection::Auto;
    const auto plan = resolvePlan(s, allAvailable(), ctx);
    CHECK(plan.frameGen == FrameGenBackend::None);
    CHECK_CONTAINS(plan.decisions[0], "video memory");
}

TEST(Plan, UnmeasuredVramDoesNotInventARefusal) {
    RuntimeContext ctx = readyContext();
    ctx.estimatedFrameGenVramBytes = 0; // not queried yet
    ctx.availableVramBytes = 0;
    Settings s = enabledSettings();
    s.frameGeneration = FrameGenSelection::Auto;
    const auto plan = resolvePlan(s, allAvailable(), ctx);
    CHECK(plan.frameGen == FrameGenBackend::Ml);
}

TEST(Plan, FrameGenAlwaysWarnsAboutLatency) {
    Settings s = enabledSettings();
    s.frameGeneration = FrameGenSelection::Auto;
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    CHECK(plan.frameGen != FrameGenBackend::None);
    bool found = false;
    for (const auto& w : plan.warnings) {
        if (w.find("latency") != std::string::npos) found = true;
    }
    CHECK(found);
}

TEST(Plan, UpscalerWithoutFrameGenIsSupported) {
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Ml;
    s.frameGeneration = FrameGenSelection::Disabled;
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    CHECK(plan.upscaler == UpscalerBackend::Ml);
    CHECK(plan.frameGen == FrameGenBackend::None);
}

TEST(Plan, FrameGenWithoutUpscalerIsSupported) {
    // AMD designed FSR 3.1+ frame generation to be usable independently of the
    // FSR upscaler; this is the configuration that works best in BeamNG.
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Disabled;
    s.frameGeneration = FrameGenSelection::Ml;
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    CHECK(plan.upscaler == UpscalerBackend::None);
    CHECK(plan.frameGen == FrameGenBackend::Ml);
    CHECK_EQ(plan.frameGenVersionId, uint64_t{2001});
}

TEST(Plan, PinnedVersionIsHonoured) {
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Analytical;
    s.upscalerVersionId = "1002";
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    CHECK_EQ(plan.upscalerVersionId, uint64_t{1002});
    CHECK_STR_EQ(plan.upscalerVersionName, "FSR 3.1.5");
}

TEST(Plan, MismatchedPinnedVersionIsRejectedNotSilentlyUsed) {
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Ml;
    s.upscalerVersionId = "1002"; // an analytical version pinned to the ML slot
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    CHECK_EQ(plan.upscalerVersionId, uint64_t{1001});
    CHECK_CONTAINS(plan.downgrades[0], "does not match");
}

TEST(Plan, CompatibilityModeIgnoresVersionPins) {
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Ml;
    s.upscalerVersionId = "1001";
    s.forceCompatibilityMode = true;
    s.sanitise(); // compatibility mode resets the pin
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    CHECK(plan.upscaler == UpscalerBackend::Ml);
    CHECK_EQ(plan.upscalerVersionId, uint64_t{1001}); // AMD's own first-listed provider
}

TEST(Plan, NoMatchingProviderDisablesRatherThanSubstitutes) {
    RuntimeContext ctx = readyContext();
    ctx.upscalerVersions = {{1002, "FSR 3.1.5", false}}; // no ML provider at all
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Ml;
    const auto plan = resolvePlan(s, allAvailable(), ctx);
    CHECK(plan.upscaler == UpscalerBackend::None);
    CHECK_CONTAINS(plan.downgrades.back(), "no upscaler provider");
}

TEST(Plan, AntiLagRefusalExplainsItself) {
    CapabilitySet caps = allAvailable();
    for (auto& f : caps.features) {
        if (f.feature == Feature::AntiLag2) {
            f.available = false;
            f.unavailableReason = "Anti-Lag 2 is an AMD driver extension.";
        }
    }
    Settings s = enabledSettings();
    s.antiLag2 = true;
    const auto plan = resolvePlan(s, caps, readyContext());
    CHECK(!plan.antiLag2);
    CHECK_CONTAINS(plan.downgrades[0], "Anti-Lag 2");
}

TEST(Plan, RenderSizeMatchesAmdRatios) {
    // AMD's documented per-dimension ratios from ffx_upscale.h.
    const Extent2D out{3840, 2160};
    CHECK(renderSizeForQuality(out, QualityMode::NativeAA, 0) == (Extent2D{3840, 2160}));
    CHECK(renderSizeForQuality(out, QualityMode::Quality, 0) == (Extent2D{2560, 1440}));
    CHECK(renderSizeForQuality(out, QualityMode::Performance, 0) == (Extent2D{1920, 1080}));
    CHECK(renderSizeForQuality(out, QualityMode::UltraPerformance, 0) == (Extent2D{1280, 720}));
    const Extent2D balanced = renderSizeForQuality(out, QualityMode::Balanced, 0);
    CHECK_EQ(balanced.width, 2259u);  // 3840 / 1.7
    CHECK_EQ(balanced.height, 1271u); // 2160 / 1.7
}

TEST(Plan, CustomRenderScaleIsRespectedAndClamped) {
    const Extent2D out{1920, 1080};
    CHECK(renderSizeForQuality(out, QualityMode::Custom, 2.0f) == (Extent2D{960, 540}));
    // Out-of-range scales clamp instead of producing a degenerate target.
    CHECK(renderSizeForQuality(out, QualityMode::Custom, 0.1f) == (Extent2D{1920, 1080}));
    CHECK(renderSizeForQuality(out, QualityMode::Custom, 99.0f) == (Extent2D{640, 360}));
}

TEST(Plan, AutoQualityScalesWithOutputResolution) {
    CHECK(autoQualityForResolution({3840, 2160}) == QualityMode::Performance);
    CHECK(autoQualityForResolution({2560, 1440}) == QualityMode::Balanced);
    CHECK(autoQualityForResolution({1920, 1080}) == QualityMode::Quality);
    CHECK(autoQualityForResolution({1280, 720}) == QualityMode::NativeAA);
}

TEST(Plan, VersionClassificationSplitsMlFromAnalytical) {
    CHECK(classifyFfxVersionAsMl("FSR Upscaling 4.1.1"));
    CHECK(classifyFfxVersionAsMl("FSR Frame Generation 4.0.1"));
    CHECK(!classifyFfxVersionAsMl("FSR 3.1.5"));
    CHECK(!classifyFfxVersionAsMl("FSR 2.3.4"));
    CHECK(!classifyFfxVersionAsMl("FSR Frame Generation 3.1.6"));
}

TEST(Plan, IsSerialisable) {
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Auto;
    s.frameGeneration = FrameGenSelection::Auto;
    const auto plan = resolvePlan(s, allAvailable(), readyContext());
    const Json j = plan.toJson();
    CHECK_STR_EQ(j["upscaler"].asString(), "ml");
    CHECK_STR_EQ(j["frameGeneration"].asString(), "ml");
    CHECK(!Json::parse(j.dump(2)).isNull());
}

TEST(Plan, IsDeterministic) {
    Settings s = enabledSettings();
    s.upscaler = UpscalerSelection::Auto;
    s.frameGeneration = FrameGenSelection::Auto;
    const auto a = resolvePlan(s, allAvailable(), readyContext());
    const auto b = resolvePlan(s, allAvailable(), readyContext());
    CHECK_STR_EQ(a.toJson().dump(-1), b.toJson().dump(-1));
}
