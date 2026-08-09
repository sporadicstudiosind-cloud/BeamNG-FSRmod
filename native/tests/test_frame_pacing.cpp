// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include "beamng_fsr/frame_pacing.hpp"

#include <limits>

using namespace bfsr;

namespace {

/// Runs enough clean frames to clear the settling window.
void settle(FrameGenerationGate& gate) {
    FrameGenGateInput in;
    for (uint32_t i = 0; i < FrameGenerationGate::kSettleFrames + 2; ++i) {
        gate.evaluate(in);
    }
}

} // namespace

TEST(FramePacing, StartsSuppressedThenSettlesIntoAllowing) {
    FrameGenerationGate gate;
    FrameGenGateInput in;

    // The very first frame has no previous frame to interpolate against.
    const auto first = gate.evaluate(in);
    CHECK(!first.allowInterpolation);

    settle(gate);
    CHECK(gate.evaluate(in).allowInterpolation);
}

TEST(FramePacing, PauseSuppressesAndRequestsResetOnce) {
    FrameGenerationGate gate;
    settle(gate);

    FrameGenGateInput in;
    in.paused = true;
    const auto d1 = gate.evaluate(in);
    CHECK(!d1.allowInterpolation);
    CHECK(d1.requestReset);
    CHECK_CONTAINS(describeSuppressMask(d1.suppressMask), "paused");

    // Repeated pause frames must not keep re-issuing a reset.
    const auto d2 = gate.evaluate(in);
    CHECK(!d2.allowInterpolation);
    CHECK(!d2.requestReset);
}

TEST(FramePacing, ResumeHoldsOffForTheSettleWindow) {
    FrameGenerationGate gate;
    settle(gate);

    FrameGenGateInput paused;
    paused.paused = true;
    gate.evaluate(paused);

    FrameGenGateInput running;
    for (uint32_t i = 0; i < FrameGenerationGate::kSettleFrames; ++i) {
        const auto d = gate.evaluate(running);
        CHECK(!d.allowInterpolation);
        CHECK(d.requestReset);
    }
    CHECK(gate.evaluate(running).allowInterpolation);
}

TEST(FramePacing, EveryDisruptionSuppresses) {
    struct Case { const char* label; void (*set)(FrameGenGateInput&); const char* expect; };
    const Case cases[] = {
        {"loading",   [](FrameGenGateInput& i) { i.loading = true; }, "loading"},
        {"ui",        [](FrameGenGateInput& i) { i.uiFocused = true; }, "menu"},
        {"photo",     [](FrameGenGateInput& i) { i.photoMode = true; }, "photo mode"},
        {"replay",    [](FrameGenGateInput& i) { i.replay = true; }, "replay"},
        {"occluded",  [](FrameGenGateInput& i) { i.windowOccluded = true; }, "occluded"},
        {"deviceLost",[](FrameGenGateInput& i) { i.deviceLost = true; }, "device lost"},
        {"resChange", [](FrameGenGateInput& i) { i.resolutionChangedThisFrame = true; }, "resolution"},
        {"camCut",    [](FrameGenGateInput& i) { i.cameraCutThisFrame = true; }, "camera cut"},
        {"noInputs",  [](FrameGenGateInput& i) { i.inputsAvailable = false; }, "render inputs"},
        {"disabled",  [](FrameGenGateInput& i) { i.enabledByPlan = false; }, "disabled"},
    };

    for (const auto& c : cases) {
        FrameGenerationGate gate;
        settle(gate);
        FrameGenGateInput in;
        c.set(in);
        const auto d = gate.evaluate(in);
        CHECK(!d.allowInterpolation);
        CHECK_CONTAINS(describeSuppressMask(d.suppressMask), c.expect);
    }
}

TEST(FramePacing, HitchIsDetectedAndSuppresses) {
    FrameGenerationGate gate;
    FrameGenGateInput in;
    in.frameTimeMs = 16.6f;
    for (int i = 0; i < 60; ++i) gate.evaluate(in);
    CHECK(gate.evaluate(in).allowInterpolation);

    in.frameTimeMs = 400.0f; // a level-stream stall
    const auto d = gate.evaluate(in);
    CHECK(!d.allowInterpolation);
    CHECK_CONTAINS(describeSuppressMask(d.suppressMask), "hitch");
}

TEST(FramePacing, NormalVarianceIsNotAHitch) {
    FrameGenerationGate gate;
    FrameGenGateInput in;
    in.frameTimeMs = 16.6f;
    for (int i = 0; i < 120; ++i) gate.evaluate(in);
    in.frameTimeMs = 22.0f; // ordinary jitter
    CHECK(gate.evaluate(in).allowInterpolation);
}

TEST(FramePacing, FirstFrameIsNotMisreadAsAHitch) {
    FrameGenerationGate gate;
    FrameGenGateInput in;
    in.frameTimeMs = 250.0f; // slow first frame, no baseline yet
    const auto d = gate.evaluate(in);
    const uint32_t anomaly = static_cast<uint32_t>(FrameGenSuppressReason::FrameTimeAnomaly);
    CHECK((d.suppressMask & anomaly) == 0);
}

TEST(FramePacing, MaskDescriptionListsMultipleReasons) {
    FrameGenerationGate gate;
    settle(gate);
    FrameGenGateInput in;
    in.paused = true;
    in.loading = true;
    const auto d = gate.evaluate(in);
    const std::string text = describeSuppressMask(d.suppressMask);
    CHECK_CONTAINS(text, "paused");
    CHECK_CONTAINS(text, "loading");
    CHECK_STR_EQ(describeSuppressMask(0), "none");
}

// ---------------------------------------------------------------------------

TEST(FrameTimeTracker, ComputesAveragesAndLows) {
    FrameTimeTracker t;
    for (int i = 0; i < 1000; ++i) t.addFrame(10.0f); // a clean 100 fps
    CHECK_NEAR(t.averageFps(), 100.0, 0.001);
    CHECK_NEAR(t.averageFrameTimeMs(), 10.0, 0.001);
    CHECK_NEAR(t.percentileLowFps(0.01), 100.0, 0.001);
}

TEST(FrameTimeTracker, LowsReflectTheWorstFrames) {
    FrameTimeTracker t;
    for (int i = 0; i < 990; ++i) t.addFrame(10.0f);
    for (int i = 0; i < 10; ++i) t.addFrame(100.0f); // 1% of frames at 10 fps
    CHECK_NEAR(t.percentileLowFps(0.01), 10.0, 0.001);
    CHECK(t.averageFps() > 50.0f); // the average is barely moved
    CHECK_NEAR(t.maxFrameTimeMs(), 100.0, 0.001);
    CHECK_NEAR(t.minFrameTimeMs(), 10.0, 0.001);
}

TEST(FrameTimeTracker, HandlesEmptyAndInvalidInput) {
    FrameTimeTracker t;
    CHECK_NEAR(t.averageFps(), 0.0, 1e-9);
    CHECK_NEAR(t.percentileLowFps(0.01), 0.0, 1e-9);
    t.addFrame(0.0f);
    t.addFrame(-5.0f);
    t.addFrame(std::numeric_limits<float>::quiet_NaN());
    t.addFrame(std::numeric_limits<float>::infinity());
    CHECK_EQ(t.sampleCount(), size_t{0});
}

TEST(FrameTimeTracker, RingBufferKeepsAverageCorrectAfterWrapping) {
    FrameTimeTracker t(100);
    for (int i = 0; i < 100; ++i) t.addFrame(50.0f);
    for (int i = 0; i < 100; ++i) t.addFrame(10.0f); // fully replaces the history
    CHECK_EQ(t.sampleCount(), size_t{100});
    CHECK_NEAR(t.averageFrameTimeMs(), 10.0, 0.001);
}

TEST(FrameTimeTracker, RejectsOutOfRangePercentiles) {
    FrameTimeTracker t;
    t.addFrame(10.0f);
    CHECK_NEAR(t.percentileLowFps(0.0), 0.0, 1e-9);
    CHECK_NEAR(t.percentileLowFps(1.5), 0.0, 1e-9);
    CHECK_NEAR(t.percentileLowFps(1.0), 100.0, 0.001);
}
