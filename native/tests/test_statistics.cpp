// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include "beamng_fsr/statistics.hpp"

using namespace bfsr;

namespace {

/// Feeds `renderedFrames` real frames at `frameTimeMs`, optionally interleaving
/// one generated frame after each.
void feed(BenchmarkSession& b, int renderedFrames, float frameTimeMs, bool withFrameGen) {
    double t = 0.0;
    uint64_t index = 0;
    for (int i = 0; i < renderedFrames; ++i) {
        FrameSample s;
        s.index = index++;
        s.timestampSeconds = t;
        s.cpuFrameTimeMs = frameTimeMs;
        s.upscalerGpuMs = 1.25f;
        s.vramBytes = 1024ull * 1024ull * 500ull;
        b.addSample(s);
        t += frameTimeMs / 1000.0;

        if (withFrameGen) {
            FrameSample g;
            g.index = index++;
            g.timestampSeconds = t - (frameTimeMs / 2000.0);
            g.wasGenerated = true;
            g.frameGenGpuMs = 0.9f;
            b.addSample(g);
        }
    }
}

} // namespace

TEST(Benchmark, IgnoresSamplesWhenNotRunning) {
    BenchmarkSession b;
    FrameSample s;
    s.cpuFrameTimeMs = 10.0f;
    b.addSample(s);
    CHECK_EQ(b.sampleCount(), size_t{0});
}

TEST(Benchmark, CountsRenderedAndGeneratedFramesSeparately) {
    BenchmarkSession b;
    b.start("test");
    feed(b, 100, 10.0f, /*withFrameGen=*/true);
    b.stop();
    CHECK_EQ(b.renderedFrameCount(), uint64_t{100});
    CHECK_EQ(b.generatedFrameCount(), uint64_t{100});

    const auto s = b.summarise();
    // Rendered fps must reflect real frames only, never the doubled figure.
    CHECK_NEAR(s.averageRenderedFps, 100.0, 0.5);
    CHECK(s.averageDisplayedFps > s.averageRenderedFps * 1.8f);
}

TEST(Benchmark, GeneratedFramesDoNotPolluteFrameTimeLows) {
    BenchmarkSession b;
    b.start("test");
    feed(b, 1000, 10.0f, true);
    b.stop();
    const auto s = b.summarise();
    CHECK_NEAR(s.averageFrameTimeMs, 10.0, 0.001);
    CHECK_NEAR(s.onePercentLowFps, 100.0, 0.5);
}

TEST(Benchmark, PercentileLowsTrackStutter) {
    BenchmarkSession b;
    b.start("stutter");
    feed(b, 990, 10.0f, false);
    feed(b, 10, 100.0f, false);
    b.stop();
    const auto s = b.summarise();
    CHECK(s.onePercentLowFps < 20.0f);
    CHECK(s.averageRenderedFps > 40.0f);
    CHECK_NEAR(s.maxFrameTimeMs, 100.0, 0.001);
}

TEST(Benchmark, UnmeasuredGpuTimingIsReportedAsUnmeasured) {
    // A stage that never reported a timing must not appear as "0 ms cost".
    BenchmarkSession b;
    b.start("no-fg");
    feed(b, 50, 10.0f, /*withFrameGen=*/false);
    b.stop();
    const auto s = b.summarise();
    CHECK(s.upscalerGpuTimingAvailable);
    CHECK(!s.frameGenGpuTimingAvailable);
    CHECK_NEAR(s.averageUpscalerGpuMs, 1.25, 1e-4);
    CHECK_NEAR(s.averageFrameGenGpuMs, 0.0, 1e-9);
}

TEST(Benchmark, TracksPeakVram) {
    BenchmarkSession b;
    b.start("vram");
    FrameSample a;
    a.cpuFrameTimeMs = 10.0f;
    a.vramBytes = 100;
    b.addSample(a);
    FrameSample c = a;
    c.vramBytes = 900;
    b.addSample(c);
    FrameSample d = a;
    d.vramBytes = 300;
    b.addSample(d);
    b.stop();
    CHECK_EQ(b.summarise().peakVramBytes, uint64_t{900});
}

TEST(Benchmark, CsvHasAHeaderAndOneRowPerFrame) {
    BenchmarkSession b;
    b.start("csv");
    feed(b, 3, 10.0f, false);
    b.stop();

    const std::string csv = b.toCsv();
    CHECK_CONTAINS(csv, "frame_index,timestamp_s,cpu_frame_time_ms");
    size_t newlines = 0;
    for (char c : csv) {
        if (c == '\n') ++newlines;
    }
    CHECK_EQ(newlines, size_t{4}); // header + 3 rows
}

TEST(Benchmark, CsvLeavesUnmeasuredTimingsBlankNotZero) {
    BenchmarkSession b;
    b.start("csv");
    FrameSample s;
    s.cpuFrameTimeMs = 10.0f;
    s.upscalerGpuMs = 0.0f; // unmeasured
    s.frameGenGpuMs = 2.5f;
    b.addSample(s);
    b.stop();
    // ...,<blank>,2.500,...
    CHECK_CONTAINS(b.toCsv(), ",,2.500,");
}

TEST(Benchmark, JsonExportCarriesEnvironmentAndSummary) {
    BenchmarkSession b;
    b.start("json");
    feed(b, 10, 10.0f, true);
    b.stop();

    Json env = Json::object();
    env.set("gpu", Json("AMD Radeon RX 9070 XT"));

    const Json j = b.toJson(env, /*includeFrames=*/false);
    CHECK_STR_EQ(j["schema"].asString(), "beamng-fsr-benchmark/1");
    CHECK_STR_EQ(j["environment"]["gpu"].asString(), "AMD Radeon RX 9070 XT");
    CHECK_STR_EQ(j["summary"]["renderedFrames"].asString(), "10");
    CHECK(j["summary"]["frameGenGpu"]["measured"].asBool());
    CHECK(j["frames"].isNull());
    CHECK(!Json::parse(j.dump(2)).isNull());
}

TEST(Benchmark, JsonExportCanIncludePerFrameData) {
    BenchmarkSession b;
    b.start("json");
    feed(b, 5, 10.0f, false);
    b.stop();
    const Json j = b.toJson(Json::object(), /*includeFrames=*/true);
    CHECK_EQ(j["frames"].size(), size_t{5});
}

TEST(Benchmark, RestartClearsPreviousRun) {
    BenchmarkSession b;
    b.start("first");
    feed(b, 10, 10.0f, false);
    b.start("second");
    CHECK_EQ(b.sampleCount(), size_t{0});
    CHECK_EQ(b.renderedFrameCount(), uint64_t{0});
    CHECK_STR_EQ(b.label(), "second");
}
