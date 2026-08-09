// SPDX-License-Identifier: MIT
//
// The two planners are tested separately and asserted to be independent:
// neither reads the other's settings, and neither's result changes when the
// other product is absent. That independence is the whole premise of shipping
// them as two installable products.
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

CapabilitySet without(std::initializer_list<Feature> unavailable, const char* reason) {
    CapabilitySet c = allAvailable();
    for (auto& f : c.features) {
        for (Feature target : unavailable) {
            if (f.feature != target) continue;
            f.available = false;
            f.unavailableReason = reason;
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
    ctx.upscalerVersions = {{1001, "FSR Upscaling 4.1.1", true}, {1002, "FSR 3.1.5", false}};
    ctx.frameGenVersions = {{2001, "FSR Frame Generation 4.0.1", true},
                            {2002, "FSR Frame Generation 3.1.6", false}};
    return ctx;
}

FrameGenSettings fgOn(FrameGenSelection impl = FrameGenSelection::Auto) {
    FrameGenSettings s;
    s.enabled = true;
    s.implementation = impl;
    return s;
}

UpscaleSettings upOn(UpscalerSelection impl = UpscalerSelection::Auto) {
    UpscaleSettings s;
    s.enabled = true;
    s.implementation = impl;
    return s;
}

bool hasNote(const std::vector<std::string>& notes, const char* needle) {
    for (const auto& n : notes) {
        if (n.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Independence
// ---------------------------------------------------------------------------

TEST(PlanIndependence, FrameGenWorksWithNoUpscalingInstalled) {
    // The upscaling product being absent is modelled by simply never calling
    // its planner. Frame generation must be unaffected.
    const auto plan = resolveFrameGenPlan(fgOn(), allAvailable(), readyContext());
    CHECK(plan.active());
    CHECK(plan.backend == FrameGenBackend::Ml);
}

TEST(PlanIndependence, UpscalingWorksWithNoFrameGenInstalled) {
    const auto plan = resolveUpscalePlan(upOn(), allAvailable(), readyContext());
    CHECK(plan.active());
    CHECK(plan.backend == UpscalerBackend::Ml);
}

TEST(PlanIndependence, NeitherPlannerIsAffectedByTheOthersSettings) {
    RuntimeContext ctx = readyContext();
    const auto fgAlone = resolveFrameGenPlan(fgOn(), allAvailable(), ctx);
    const auto upAlone = resolveUpscalePlan(upOn(), allAvailable(), ctx);

    // Running both against the same context must produce byte-identical plans.
    const auto fgTogether = resolveFrameGenPlan(fgOn(), allAvailable(), ctx);
    const auto upTogether = resolveUpscalePlan(upOn(), allAvailable(), ctx);

    CHECK_STR_EQ(fgAlone.toJson().dump(-1), fgTogether.toJson().dump(-1));
    CHECK_STR_EQ(upAlone.toJson().dump(-1), upTogether.toJson().dump(-1));
}

TEST(PlanIndependence, UpscalerUnavailabilityDoesNotDisableFrameGen) {
    const auto caps = without({Feature::UpscalingMl, Feature::UpscalingAnalytical},
                              "no upscaling on this GPU");
    const auto plan = resolveFrameGenPlan(fgOn(), caps, readyContext());
    CHECK(plan.active());
}

TEST(PlanIndependence, FrameGenUnavailabilityDoesNotDisableUpscaling) {
    const auto caps = without({Feature::FrameGenerationMl, Feature::FrameGenerationAnalytical},
                              "no frame generation on this GPU");
    const auto plan = resolveUpscalePlan(upOn(), caps, readyContext());
    CHECK(plan.active());
}

// ---------------------------------------------------------------------------
// Frame generation
// ---------------------------------------------------------------------------

TEST(FrameGenPlan, DisabledByDefault) {
    CHECK(!resolveFrameGenPlan(FrameGenSettings{}, allAvailable(), readyContext()).active());
}

TEST(FrameGenPlan, SafeModeOverridesEverything) {
    FrameGenSettings s = fgOn(FrameGenSelection::Ml);
    s.safeMode = true;
    const auto plan = resolveFrameGenPlan(s, allAvailable(), readyContext());
    CHECK(!plan.active());
    CHECK_CONTAINS(plan.notes.downgrades[0], "Safe mode");
}

TEST(FrameGenPlan, CompatibilityModeDisablesAsyncComputeAtPlanTime) {
    FrameGenSettings s = fgOn();
    s.asyncCompute = true;
    s.diagnostics.forceCompatibilityMode = true;
    const auto plan = resolveFrameGenPlan(s, allAvailable(), readyContext());
    CHECK(!plan.asyncCompute);

    s.diagnostics.forceCompatibilityMode = false;
    CHECK(resolveFrameGenPlan(s, allAvailable(), readyContext()).asyncCompute);
}

TEST(FrameGenPlan, CompatibilityModeIgnoresTheVersionPin) {
    FrameGenSettings s = fgOn(FrameGenSelection::Analytical);
    s.versionId = "2002";
    s.diagnostics.forceCompatibilityMode = true;
    const auto plan = resolveFrameGenPlan(s, allAvailable(), readyContext());
    // Falls through to AMD's own first-listed analytical provider.
    CHECK_EQ(plan.versionId, uint64_t{2002});
    CHECK(plan.active());
}

TEST(UpscalePlan, SafeModeStillRefusesEvenWithPreferencesIntact) {
    UpscaleSettings s = upOn(UpscalerSelection::Ml);
    s.safeMode = true;
    const auto plan = resolveUpscalePlan(s, allAvailable(), readyContext());
    CHECK(!plan.active());
    CHECK_CONTAINS(plan.notes.downgrades[0], "Safe mode");
}

TEST(FrameGenPlan, MissingRenderInputsRefusesRatherThanSubstituting) {
    RuntimeContext ctx = readyContext();
    ctx.frameInputsAvailable = false;
    ctx.motionVectorsAvailable = false;
    const auto plan = resolveFrameGenPlan(fgOn(), allAvailable(), ctx);
    CHECK(!plan.active());
    CHECK_CONTAINS(plan.notes.downgrades[0], "motion vectors");
    CHECK_CONTAINS(plan.notes.downgrades[0], "Lighting Quality");
}

TEST(FrameGenPlan, AutoPrefersMl) {
    const auto plan = resolveFrameGenPlan(fgOn(), allAvailable(), readyContext());
    CHECK(plan.backend == FrameGenBackend::Ml);
    CHECK_EQ(plan.versionId, uint64_t{2001});
    CHECK(hasNote(plan.notes.decisions, "ML FSR Frame Generation"));
}

TEST(FrameGenPlan, Rdna3FallsBackToAnalyticalAndSaysWhy) {
    // The RDNA 3 case: ML frame generation unavailable, analytical available.
    const auto caps = without({Feature::FrameGenerationMl},
                              "Requires a Radeon RX 9000 series (RDNA 4) GPU or newer.");
    const auto plan = resolveFrameGenPlan(fgOn(), caps, readyContext());
    CHECK(plan.backend == FrameGenBackend::Analytical);
    CHECK_EQ(plan.versionId, uint64_t{2002});
    CHECK_CONTAINS(plan.notes.decisions[0], "RX 9000");
}

TEST(FrameGenPlan, ExplicitMlOnUnsupportedHardwareDowngradesWithReason) {
    const auto caps = without({Feature::FrameGenerationMl}, "needs RDNA 4");
    const auto plan = resolveFrameGenPlan(fgOn(FrameGenSelection::Ml), caps, readyContext());
    CHECK(plan.backend == FrameGenBackend::Analytical);
    CHECK_EQ(plan.notes.downgrades.size(), size_t{2});
    CHECK_CONTAINS(plan.notes.downgrades[1], "Fell back");
}

TEST(FrameGenPlan, AutoUsesAmdsDocumentedThresholdPerImplementation) {
    // AMD documents 30 fps for FG 4.0.1 and 60 fps for FG 3.1.6. A 45 fps base
    // must therefore be fine for ML and refused for analytical — the same input
    // producing different answers is the whole point of using AMD's numbers
    // rather than one invented threshold.
    RuntimeContext ctx = readyContext();
    ctx.measuredBaseFps = 45.0f;

    const auto ml = resolveFrameGenPlan(fgOn(), allAvailable(), ctx);
    CHECK(ml.backend == FrameGenBackend::Ml);

    const auto caps = without({Feature::FrameGenerationMl}, "needs RDNA 4");
    const auto analytical = resolveFrameGenPlan(fgOn(), caps, ctx);
    CHECK(analytical.backend == FrameGenBackend::None);
    CHECK_CONTAINS(analytical.notes.decisions.back(), "60 fps");
}

TEST(FrameGenPlan, AmdThresholdsMatchTheDocumentedValues) {
    CHECK_NEAR(amdRecommendedMinimumBaseFps(FrameGenBackend::Analytical), 60.0, 1e-6);
    CHECK_NEAR(amdRecommendedMinimumBaseFps(FrameGenBackend::Ml), 30.0, 1e-6);
    CHECK_NEAR(amdRecommendedMinimumBaseFps(FrameGenBackend::None), 0.0, 1e-6);
}

TEST(FrameGenPlan, ExplicitChoiceOverridesTheThreshold) {
    RuntimeContext ctx = readyContext();
    ctx.measuredBaseFps = 20.0f;
    const auto plan = resolveFrameGenPlan(fgOn(FrameGenSelection::Ml), allAvailable(), ctx);
    CHECK(plan.backend == FrameGenBackend::Ml);
    // ...but it still warns, because the artefacts are real.
    CHECK(hasNote(plan.notes.warnings, "30 fps"));
}

TEST(FrameGenPlan, AlwaysWarnsAboutLatency) {
    const auto plan = resolveFrameGenPlan(fgOn(), allAvailable(), readyContext());
    CHECK(hasNote(plan.notes.warnings, "latency"));
}

TEST(FrameGenPlan, SuggestsAntiLagWhenAvailableButOff) {
    const auto plan = resolveFrameGenPlan(fgOn(), allAvailable(), readyContext());
    CHECK(!plan.antiLag2);
    CHECK(hasNote(plan.notes.warnings, "Anti-Lag 2"));
}

TEST(FrameGenPlan, AntiLagEnabledWhenRequestedAndAvailable) {
    FrameGenSettings s = fgOn();
    s.antiLag2 = true;
    s.antiLagMaxFps = 120;
    const auto plan = resolveFrameGenPlan(s, allAvailable(), readyContext());
    CHECK(plan.antiLag2);
    CHECK_EQ(plan.antiLagMaxFps, 120u);
    CHECK(!hasNote(plan.notes.warnings, "switched off"));
}

TEST(FrameGenPlan, AntiLagRefusalExplainsItself) {
    FrameGenSettings s = fgOn();
    s.antiLag2 = true;
    const auto caps = without({Feature::AntiLag2}, "Anti-Lag 2 is an AMD driver extension.");
    const auto plan = resolveFrameGenPlan(s, caps, readyContext());
    CHECK(!plan.antiLag2);
    CHECK(hasNote(plan.notes.downgrades, "Anti-Lag 2"));
}

TEST(FrameGenPlan, VramGuardUsesTheRuntimeReportedRequirement) {
    RuntimeContext ctx = readyContext();
    ctx.availableVramBytes = 1ull << 30;
    ctx.estimatedFrameGenVramBytes = 900ull << 20;
    const auto plan = resolveFrameGenPlan(fgOn(), allAvailable(), ctx);
    CHECK(!plan.active());
    CHECK_CONTAINS(plan.notes.decisions.back(), "video memory");
}

TEST(FrameGenPlan, UnmeasuredVramDoesNotInventARefusal) {
    RuntimeContext ctx = readyContext();
    ctx.estimatedFrameGenVramBytes = 0;
    ctx.availableVramBytes = 0;
    CHECK(resolveFrameGenPlan(fgOn(), allAvailable(), ctx).active());
}

TEST(FrameGenPlan, MismatchedPinnedVersionIsRejected) {
    FrameGenSettings s = fgOn(FrameGenSelection::Ml);
    s.versionId = "2002"; // an analytical version pinned to the ML slot
    const auto plan = resolveFrameGenPlan(s, allAvailable(), readyContext());
    CHECK_EQ(plan.versionId, uint64_t{2001});
    CHECK(hasNote(plan.notes.downgrades, "does not match"));
}

TEST(FrameGenPlan, NoMatchingProviderDisablesRatherThanSubstitutes) {
    RuntimeContext ctx = readyContext();
    ctx.frameGenVersions = {{2002, "FSR Frame Generation 3.1.6", false}};
    const auto plan = resolveFrameGenPlan(fgOn(FrameGenSelection::Ml), allAvailable(), ctx);
    CHECK(!plan.active());
    CHECK(hasNote(plan.notes.downgrades, "no frame-generation provider"));
}

TEST(FrameGenPlan, ReportsAmdMinimumInSerialisedForm) {
    const auto plan = resolveFrameGenPlan(fgOn(), allAvailable(), readyContext());
    const Json j = plan.toJson();
    CHECK_NEAR(j["amdMinimumBaseFps"].asNumber(), 30.0, 1e-6);
    CHECK(!Json::parse(j.dump(2)).isNull());
}

// ---------------------------------------------------------------------------
// Upscaling
// ---------------------------------------------------------------------------

TEST(UpscalePlan, DisabledByDefault) {
    CHECK(!resolveUpscalePlan(UpscaleSettings{}, allAvailable(), readyContext()).active());
}

TEST(UpscalePlan, AutoPrefersMl) {
    const auto plan = resolveUpscalePlan(upOn(), allAvailable(), readyContext());
    CHECK(plan.backend == UpscalerBackend::Ml);
    CHECK_EQ(plan.versionId, uint64_t{1001});
}

TEST(UpscalePlan, FallsBackToAnalyticalAndSaysWhy) {
    const auto caps = without({Feature::UpscalingMl},
                              "Requires a Radeon RX 9000 or RX 7000 series GPU.");
    const auto plan = resolveUpscalePlan(upOn(), caps, readyContext());
    CHECK(plan.backend == UpscalerBackend::Analytical);
    CHECK_EQ(plan.versionId, uint64_t{1002});
    CHECK_CONTAINS(plan.notes.decisions[0], "RX 9000");
}

TEST(UpscalePlan, JitterWarningIsAlwaysPresent) {
    // BeamNG exposes no jitter hook; the user must be told, every time.
    const auto plan = resolveUpscalePlan(upOn(), allAvailable(), readyContext());
    CHECK(hasNote(plan.notes.warnings, "jitter"));
}

TEST(UpscalePlan, NoJitterWarningWhenJitterBecomesAvailable) {
    RuntimeContext ctx = readyContext();
    ctx.cameraJitterAvailable = true;
    const auto plan = resolveUpscalePlan(upOn(), allAvailable(), ctx);
    CHECK(!hasNote(plan.notes.warnings, "jitter"));
}

TEST(UpscalePlan, WarnsWhenUpscalingCannotSaveTime) {
    UpscaleSettings s = upOn(UpscalerSelection::Analytical);
    s.quality = QualityMode::Performance;
    const auto plan = resolveUpscalePlan(s, allAvailable(), readyContext());
    CHECK(hasNote(plan.notes.warnings, "lower BeamNG's own resolution"));
}

TEST(UpscalePlan, RenderSizeMatchesAmdRatios) {
    const Extent2D out{3840, 2160};
    CHECK(renderSizeForQuality(out, QualityMode::NativeAA, 0) == (Extent2D{3840, 2160}));
    CHECK(renderSizeForQuality(out, QualityMode::Quality, 0) == (Extent2D{2560, 1440}));
    CHECK(renderSizeForQuality(out, QualityMode::Performance, 0) == (Extent2D{1920, 1080}));
    CHECK(renderSizeForQuality(out, QualityMode::UltraPerformance, 0) == (Extent2D{1280, 720}));
    const Extent2D balanced = renderSizeForQuality(out, QualityMode::Balanced, 0);
    CHECK_EQ(balanced.width, 2259u);  // 3840 / 1.7
    CHECK_EQ(balanced.height, 1271u); // 2160 / 1.7
}

TEST(UpscalePlan, CustomRatioIsRespectedAndClamped) {
    const Extent2D out{1920, 1080};
    CHECK(renderSizeForQuality(out, QualityMode::Custom, 2.0f) == (Extent2D{960, 540}));
    CHECK(renderSizeForQuality(out, QualityMode::Custom, 0.1f) == (Extent2D{1920, 1080}));
    CHECK(renderSizeForQuality(out, QualityMode::Custom, 99.0f) == (Extent2D{640, 360}));
}

TEST(UpscalePlan, AutoQualityScalesWithOutputResolution) {
    CHECK(autoQualityForResolution({3840, 2160}) == QualityMode::Performance);
    CHECK(autoQualityForResolution({2560, 1440}) == QualityMode::Balanced);
    CHECK(autoQualityForResolution({1920, 1080}) == QualityMode::Quality);
    CHECK(autoQualityForResolution({1280, 720}) == QualityMode::NativeAA);
}

TEST(UpscalePlan, VramGuardDisablesAndExplains) {
    RuntimeContext ctx = readyContext();
    ctx.availableVramBytes = 1ull << 30;
    ctx.estimatedUpscalerVramBytes = 900ull << 20;
    const auto plan = resolveUpscalePlan(upOn(), allAvailable(), ctx);
    CHECK(!plan.active());
    CHECK(hasNote(plan.notes.downgrades, "video memory"));
}

TEST(UpscalePlan, IsSerialisableAndDeterministic) {
    const auto a = resolveUpscalePlan(upOn(), allAvailable(), readyContext());
    const auto b = resolveUpscalePlan(upOn(), allAvailable(), readyContext());
    CHECK_STR_EQ(a.toJson().dump(-1), b.toJson().dump(-1));
    CHECK_STR_EQ(a.toJson()["backend"].asString(), "ml");
    CHECK(!Json::parse(a.toJson().dump(2)).isNull());
}

TEST(Versions, ClassificationSplitsMlFromAnalytical) {
    CHECK(classifyFfxVersionAsMl("FSR Upscaling 4.1.1"));
    CHECK(classifyFfxVersionAsMl("FSR Frame Generation 4.0.1"));
    CHECK(!classifyFfxVersionAsMl("FSR 3.1.5"));
    CHECK(!classifyFfxVersionAsMl("FSR 2.3.4"));
    CHECK(!classifyFfxVersionAsMl("FSR Frame Generation 3.1.6"));
}
