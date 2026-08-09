// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include "beamng_fsr/settings.hpp"

#include <cstdio>
#include <fstream>

using namespace bfsr;

TEST(Settings, DefaultsAreOff) {
    const Settings s;
    CHECK(!s.enabled);
    CHECK(s.upscaler == UpscalerSelection::Disabled);
    CHECK(s.frameGeneration == FrameGenSelection::Disabled);
    CHECK(!s.safeMode);
}

TEST(Settings, DefaultsSurviveSanitiseUnchanged) {
    // Regression guard: every default must already sit inside its own clamp
    // range. A default outside it silently mutates on first load and breaks
    // round-tripping, which is how the customUpscaleRatio default was caught.
    Settings s;
    Settings sanitised = s;
    sanitised.sanitise();
    CHECK(sanitised == s);
}

TEST(Settings, RoundTripsThroughJson) {
    Settings s;
    s.enabled = true;
    s.upscaler = UpscalerSelection::Ml;
    s.upscalerVersionId = "12345";
    s.quality = QualityMode::Balanced;
    s.sharpness = 0.75f;
    s.frameGeneration = FrameGenSelection::Analytical;
    s.antiLag2 = true;
    s.depthConvention = DepthConvention::ReversedZ;
    s.motionVectorScaleX = -1.0f;
    s.debugOverlay = true;
    s.logLevel = "debug";

    const Settings out = Settings::fromJson(s.toJson());
    CHECK(out == s);
}

TEST(Settings, SanitiseClampsRanges) {
    Settings s;
    s.enabled = true;
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

TEST(Settings, SafeModeForcesEverythingOff) {
    Settings s;
    s.enabled = true;
    s.safeMode = true;
    s.upscaler = UpscalerSelection::Ml;
    s.frameGeneration = FrameGenSelection::Ml;
    s.antiLag2 = true;
    s.sanitise();
    CHECK(!s.enabled);
    CHECK(s.upscaler == UpscalerSelection::Disabled);
    CHECK(s.frameGeneration == FrameGenSelection::Disabled);
    CHECK(!s.antiLag2);
}

TEST(Settings, MasterSwitchOffDisablesSubFeatures) {
    Settings s;
    s.enabled = false;
    s.upscaler = UpscalerSelection::Auto;
    s.frameGeneration = FrameGenSelection::Auto;
    s.sanitise();
    CHECK(s.upscaler == UpscalerSelection::Disabled);
    CHECK(s.frameGeneration == FrameGenSelection::Disabled);
}

TEST(Settings, CompatibilityModeDropsPinsAndAsync) {
    Settings s;
    s.enabled = true;
    s.forceCompatibilityMode = true;
    s.frameGenAsyncCompute = true;
    s.upscalerVersionId = "999";
    s.frameGenVersionId = "888";
    s.sanitise();
    CHECK(!s.frameGenAsyncCompute);
    CHECK_STR_EQ(s.upscalerVersionId, "auto");
    CHECK_STR_EQ(s.frameGenVersionId, "auto");
}

TEST(Settings, UnknownEnumStringsFallBackToDefaults) {
    Json j = Json::object();
    j.set("enabled", Json(true));
    j.set("upscaler", Json("teleportation"));
    j.set("quality", Json("supreme"));
    const Settings s = Settings::fromJson(j);
    CHECK(s.upscaler == UpscalerSelection::Disabled);
    CHECK(s.quality == QualityMode::Quality);
}

TEST(Settings, WronglyTypedFieldsDoNotCorruptState) {
    // A hand-edited or partially-written file must never destabilise the bridge.
    Json j = Json::object();
    j.set("enabled", Json("yes please"));
    j.set("sharpness", Json("very"));
    j.set("upscalerVersionId", Json(12345));
    const Settings s = Settings::fromJson(j);
    CHECK(!s.enabled);                  // string is not a bool -> default
    CHECK_NEAR(s.sharpness, 0.5f, 1e-6); // string is not a number -> default
    CHECK_STR_EQ(s.upscalerVersionId, "auto"); // number is not a string -> default
}

TEST(Settings, EmptyJsonYieldsDefaults) {
    CHECK(Settings::fromJson(Json{}) == Settings{});
    CHECK(Settings::fromJson(Json::parse("[]")) == Settings{});
}

TEST(Settings, QualityRatiosMatchAmdDocumentation) {
    CHECK_NEAR(qualityModeRatio(QualityMode::NativeAA), 1.0f, 1e-6);
    CHECK_NEAR(qualityModeRatio(QualityMode::Quality), 1.5f, 1e-6);
    CHECK_NEAR(qualityModeRatio(QualityMode::Balanced), 1.7f, 1e-6);
    CHECK_NEAR(qualityModeRatio(QualityMode::Performance), 2.0f, 1e-6);
    CHECK_NEAR(qualityModeRatio(QualityMode::UltraPerformance), 3.0f, 1e-6);
}

TEST(Settings, StorePersistsAndReloads) {
    const std::string path = "bfsr_test_settings.json";
    std::remove(path.c_str());

    SettingsStore store(path);
    Settings s;
    s.enabled = true;
    s.upscaler = UpscalerSelection::Analytical;
    s.sharpness = 0.25f;

    std::string diag;
    CHECK(store.save(s, &diag));
    CHECK(diag.empty());

    const Settings loaded = store.load(&diag);
    CHECK(diag.empty());
    CHECK(loaded == s);

    std::remove(path.c_str());
}

TEST(Settings, StoreReturnsDefaultsForMissingFile) {
    SettingsStore store("definitely_not_present_bfsr.json");
    std::string diag;
    const Settings loaded = store.load(&diag);
    CHECK(loaded == Settings{});
    CHECK_CONTAINS(diag, "no settings file");
}

TEST(Settings, StoreSurvivesCorruptFile) {
    const std::string path = "bfsr_test_corrupt.json";
    {
        std::ofstream out(path);
        out << "{ this is not json";
    }
    SettingsStore store(path);
    std::string diag;
    const Settings loaded = store.load(&diag);
    CHECK(loaded == Settings{});
    CHECK_CONTAINS(diag, "not valid JSON");
    std::remove(path.c_str());
}

TEST(Settings, SaveIsAtomicAndLeavesNoTempFile) {
    const std::string path = "bfsr_test_atomic.json";
    std::remove(path.c_str());
    SettingsStore store(path);
    Settings s;
    s.enabled = true;
    CHECK(store.save(s));
    // Writing twice must also work (the second overwrites an existing target).
    CHECK(store.save(s));
    std::ifstream tmp(path + ".tmp");
    CHECK(!tmp.good());
    std::remove(path.c_str());
}
