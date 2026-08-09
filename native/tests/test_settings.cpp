// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include "beamng_fsr/settings.hpp"

#include <cstdio>
#include <fstream>

using namespace bfsr;

// --- shared invariants ----------------------------------------------------

TEST(Settings, DefaultsSurviveSanitiseUnchanged) {
    // Regression guard: every default must already sit inside its own clamp
    // range. A default outside it silently mutates on first load and breaks
    // round-tripping.
    FrameGenSettings fg;
    FrameGenSettings fgSanitised = fg;
    fgSanitised.sanitise();
    CHECK(fgSanitised == fg);

    UpscaleSettings up;
    UpscaleSettings upSanitised = up;
    upSanitised.sanitise();
    CHECK(upSanitised == up);
}

TEST(Settings, BothProductsDefaultToOff) {
    CHECK(!FrameGenSettings{}.enabled);
    CHECK(!UpscaleSettings{}.enabled);
    CHECK(!FrameGenSettings{}.safeMode);
    CHECK(!UpscaleSettings{}.safeMode);
}

TEST(Settings, QualityRatiosMatchAmdDocumentation) {
    CHECK_NEAR(qualityModeRatio(QualityMode::NativeAA), 1.0f, 1e-6);
    CHECK_NEAR(qualityModeRatio(QualityMode::Quality), 1.5f, 1e-6);
    CHECK_NEAR(qualityModeRatio(QualityMode::Balanced), 1.7f, 1e-6);
    CHECK_NEAR(qualityModeRatio(QualityMode::Performance), 2.0f, 1e-6);
    CHECK_NEAR(qualityModeRatio(QualityMode::UltraPerformance), 3.0f, 1e-6);
}

// --- frame generation -----------------------------------------------------

TEST(FrameGenSettings, RoundTripsThroughJson) {
    FrameGenSettings s;
    s.enabled = true;
    s.implementation = FrameGenSelection::Ml;
    s.versionId = "12345";
    s.asyncCompute = false;
    s.antiLag2 = true;
    s.antiLagMaxFps = 144;
    s.calibration.depthConvention = DepthConvention::ReversedZ;
    s.calibration.motionVectorScaleX = -1.0f;
    s.diagnostics.overlay = true;
    s.diagnostics.logLevel = "debug";

    CHECK(FrameGenSettings::fromJson(s.toJson()) == s);
}

TEST(FrameGenSettings, SafeModeSwitchesOffWithoutDiscardingPreferences) {
    // Safe mode has to stop the product running, but destroying the user's
    // configuration on the way would make clearing it a re-setup exercise. The
    // planner is what refuses; sanitise only flips the master switch.
    FrameGenSettings s;
    s.enabled = true;
    s.safeMode = true;
    s.implementation = FrameGenSelection::Ml;
    s.antiLag2 = true;
    s.versionId = "2001";
    s.sanitise();

    CHECK(!s.enabled);
    CHECK(s.implementation == FrameGenSelection::Ml);
    CHECK(s.antiLag2);
    CHECK_STR_EQ(s.versionId, "2001");
}

TEST(FrameGenSettings, CompatibilityModeIsAPlanTimeChoiceNotADestructiveEdit) {
    // Turning compatibility mode on and back off must leave the async-compute
    // and version-pin choices intact.
    FrameGenSettings s;
    s.enabled = true;
    s.asyncCompute = true;
    s.versionId = "999";
    s.diagnostics.forceCompatibilityMode = true;
    s.sanitise();
    CHECK(s.asyncCompute);
    CHECK_STR_EQ(s.versionId, "999");

    s.diagnostics.forceCompatibilityMode = false;
    s.sanitise();
    CHECK(s.asyncCompute);
    CHECK_STR_EQ(s.versionId, "999");
}

TEST(FrameGenSettings, AntiLagFrameCapIsClampedOrUncapped) {
    FrameGenSettings s;
    s.antiLagMaxFps = 0;   // uncapped is a legal value and must survive
    s.sanitise();
    CHECK_EQ(s.antiLagMaxFps, 0u);

    s.antiLagMaxFps = 5;
    s.sanitise();
    CHECK_EQ(s.antiLagMaxFps, 20u);

    s.antiLagMaxFps = 100000;
    s.sanitise();
    CHECK_EQ(s.antiLagMaxFps, 1000u);
}

TEST(FrameGenSettings, WronglyTypedFieldsDoNotCorruptState) {
    Json j = Json::object();
    j.set("enabled", Json("yes please"));
    j.set("versionId", Json(12345));
    j.set("antiLagMaxFps", Json("lots"));
    const FrameGenSettings s = FrameGenSettings::fromJson(j);
    CHECK(!s.enabled);
    CHECK_STR_EQ(s.versionId, "auto");
    CHECK_EQ(s.antiLagMaxFps, 0u);
}

// --- upscaling ------------------------------------------------------------

TEST(UpscaleSettings, RoundTripsThroughJson) {
    UpscaleSettings s;
    s.enabled = true;
    s.implementation = UpscalerSelection::Analytical;
    s.versionId = "777";
    s.quality = QualityMode::Balanced;
    s.sharpness = 0.75f;
    s.calibration.motionVectorFlipY = false;
    s.diagnostics.debugView = true;

    CHECK(UpscaleSettings::fromJson(s.toJson()) == s);
}

TEST(UpscaleSettings, SanitiseClampsRanges) {
    UpscaleSettings s;
    s.sharpness = 5.0f;
    s.customUpscaleRatio = 99.0f;
    s.sanitise();
    CHECK_NEAR(s.sharpness, 1.0f, 1e-6);
    CHECK_NEAR(s.customUpscaleRatio, 3.0f, 1e-6);

    s.sharpness = -3.0f;
    s.customUpscaleRatio = 0.01f;
    s.sanitise();
    CHECK_NEAR(s.sharpness, 0.0f, 1e-6);
    CHECK_NEAR(s.customUpscaleRatio, 1.0f, 1e-6);
}

TEST(UpscaleSettings, SafeModeSwitchesOffWithoutDiscardingPreferences) {
    UpscaleSettings s;
    s.enabled = true;
    s.safeMode = true;
    s.implementation = UpscalerSelection::Ml;
    s.quality = QualityMode::Performance;
    s.sanitise();
    CHECK(!s.enabled);
    CHECK(s.implementation == UpscalerSelection::Ml);
    CHECK(s.quality == QualityMode::Performance);
}

TEST(UpscaleSettings, UnknownEnumStringsFallBackToDefaults) {
    Json j = Json::object();
    j.set("enabled", Json(true));
    j.set("implementation", Json("teleportation"));
    j.set("quality", Json("supreme"));
    const UpscaleSettings s = UpscaleSettings::fromJson(j);
    CHECK(s.implementation == UpscalerSelection::Auto);
    CHECK(s.quality == QualityMode::Quality);
}

TEST(UpscaleSettings, EmptyJsonYieldsDefaults) {
    CHECK(UpscaleSettings::fromJson(Json{}) == UpscaleSettings{});
    CHECK(FrameGenSettings::fromJson(Json::parse("[]")) == FrameGenSettings{});
}

// --- persistence ----------------------------------------------------------

TEST(SettingsStore, EachProductPersistsIndependently) {
    const std::string fgPath = "bfsr_test_fg.json";
    const std::string upPath = "bfsr_test_up.json";
    std::remove(fgPath.c_str());
    std::remove(upPath.c_str());

    SettingsStore<FrameGenSettings> fgStore(fgPath);
    SettingsStore<UpscaleSettings> upStore(upPath);

    FrameGenSettings fg;
    fg.enabled = true;
    fg.implementation = FrameGenSelection::Analytical;
    CHECK(fgStore.save(fg));

    // Loading the upscaling document must be unaffected by the frame
    // generation one existing — the two products share no state.
    std::string diagnostic;
    const UpscaleSettings up = upStore.load(&diagnostic);
    CHECK(up == UpscaleSettings{});
    CHECK_CONTAINS(diagnostic, "no settings file");

    CHECK(fgStore.load() == fg);

    std::remove(fgPath.c_str());
    std::remove(upPath.c_str());
}

TEST(SettingsStore, SurvivesCorruptFile) {
    const std::string path = "bfsr_test_corrupt.json";
    { std::ofstream out(path); out << "{ this is not json"; }
    SettingsStore<UpscaleSettings> store(path);
    std::string diagnostic;
    CHECK(store.load(&diagnostic) == UpscaleSettings{});
    CHECK_CONTAINS(diagnostic, "not valid JSON");
    std::remove(path.c_str());
}

TEST(SettingsStore, SaveIsAtomicAndLeavesNoTempFile) {
    const std::string path = "bfsr_test_atomic.json";
    std::remove(path.c_str());
    SettingsStore<FrameGenSettings> store(path);
    FrameGenSettings s;
    s.enabled = true;
    CHECK(store.save(s));
    CHECK(store.save(s)); // overwriting an existing target must also work
    std::ifstream tmp(path + ".tmp");
    CHECK(!tmp.good());
    std::remove(path.c_str());
}
