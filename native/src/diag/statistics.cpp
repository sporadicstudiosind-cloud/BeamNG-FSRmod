// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/statistics.hpp"

#include <algorithm>
#include <cstdio>

namespace bfsr {
namespace {

std::string fmt(double v, int decimals = 3) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

} // namespace

void BenchmarkSession::start(std::string label) {
    running_ = true;
    label_ = std::move(label);
    samples_.clear();
    renderedFrameTimes_.clear();
    renderedCount_ = 0;
    generatedCount_ = 0;
    firstTimestamp_ = 0.0;
    lastTimestamp_ = 0.0;
    peakVram_ = 0;
}

void BenchmarkSession::stop() { running_ = false; }

void BenchmarkSession::addSample(const FrameSample& sample) {
    if (!running_) return;

    if (samples_.empty()) firstTimestamp_ = sample.timestampSeconds;
    lastTimestamp_ = sample.timestampSeconds;

    if (sample.wasGenerated) {
        ++generatedCount_;
    } else {
        ++renderedCount_;
        // Only real frames feed the frame-time distribution: mixing interpolated
        // frames in would inflate the lows and hide the stutter they mask.
        renderedFrameTimes_.addFrame(sample.cpuFrameTimeMs);
    }

    peakVram_ = std::max(peakVram_, sample.vramBytes);
    samples_.push_back(sample);
}

BenchmarkSession::Summary BenchmarkSession::summarise() const {
    Summary s;
    s.label = label_;
    s.renderedFrames = renderedCount_;
    s.generatedFrames = generatedCount_;
    s.durationSeconds = lastTimestamp_ > firstTimestamp_ ? lastTimestamp_ - firstTimestamp_ : 0.0;
    s.averageFrameTimeMs = renderedFrameTimes_.averageFrameTimeMs();
    s.averageRenderedFps = renderedFrameTimes_.averageFps();
    s.onePercentLowFps = renderedFrameTimes_.percentileLowFps(0.01);
    s.pointOnePercentLowFps = renderedFrameTimes_.percentileLowFps(0.001);
    s.minFrameTimeMs = renderedFrameTimes_.minFrameTimeMs();
    s.maxFrameTimeMs = renderedFrameTimes_.maxFrameTimeMs();
    s.peakVramBytes = peakVram_;

    if (s.durationSeconds > 0.0) {
        s.averageDisplayedFps =
            static_cast<float>(static_cast<double>(renderedCount_ + generatedCount_) /
                               s.durationSeconds);
    }

    double upscalerSum = 0.0;
    size_t upscalerN = 0;
    double frameGenSum = 0.0;
    size_t frameGenN = 0;
    for (const auto& f : samples_) {
        if (f.upscalerGpuMs > 0.0f) { upscalerSum += f.upscalerGpuMs; ++upscalerN; }
        if (f.frameGenGpuMs > 0.0f) { frameGenSum += f.frameGenGpuMs; ++frameGenN; }
    }
    s.upscalerGpuTimingAvailable = upscalerN > 0;
    s.frameGenGpuTimingAvailable = frameGenN > 0;
    if (upscalerN > 0) s.averageUpscalerGpuMs = static_cast<float>(upscalerSum / static_cast<double>(upscalerN));
    if (frameGenN > 0) s.averageFrameGenGpuMs = static_cast<float>(frameGenSum / static_cast<double>(frameGenN));
    return s;
}

std::string BenchmarkSession::toCsv() const {
    std::string out =
        "frame_index,timestamp_s,cpu_frame_time_ms,upscaler_gpu_ms,framegen_gpu_ms,"
        "is_generated_frame,vram_bytes\n";
    out.reserve(out.size() + samples_.size() * 64);
    for (const auto& f : samples_) {
        out += std::to_string(f.index);
        out += ',' + fmt(f.timestampSeconds, 6);
        out += ',' + fmt(f.cpuFrameTimeMs);
        // Empty rather than 0 when unmeasured, so a spreadsheet does not average
        // a missing measurement in as a zero.
        out += ',' + (f.upscalerGpuMs > 0.0f ? fmt(f.upscalerGpuMs) : std::string{});
        out += ',' + (f.frameGenGpuMs > 0.0f ? fmt(f.frameGenGpuMs) : std::string{});
        out += ',' + std::string(f.wasGenerated ? "1" : "0");
        out += ',' + std::to_string(f.vramBytes);
        out += '\n';
    }
    return out;
}

Json BenchmarkSession::toJson(const Json& environment, bool includeFrames) const {
    const Summary s = summarise();

    Json summary = Json::object();
    summary.set("label", Json(s.label));
    summary.set("renderedFrames", Json(std::to_string(s.renderedFrames)));
    summary.set("generatedFrames", Json(std::to_string(s.generatedFrames)));
    summary.set("durationSeconds", Json(s.durationSeconds));
    summary.set("averageRenderedFps", Json(static_cast<double>(s.averageRenderedFps)));
    summary.set("averageDisplayedFps", Json(static_cast<double>(s.averageDisplayedFps)));
    summary.set("onePercentLowFps", Json(static_cast<double>(s.onePercentLowFps)));
    summary.set("pointOnePercentLowFps", Json(static_cast<double>(s.pointOnePercentLowFps)));
    summary.set("averageFrameTimeMs", Json(static_cast<double>(s.averageFrameTimeMs)));
    summary.set("minFrameTimeMs", Json(static_cast<double>(s.minFrameTimeMs)));
    summary.set("maxFrameTimeMs", Json(static_cast<double>(s.maxFrameTimeMs)));
    summary.set("peakVramBytes", Json(std::to_string(s.peakVramBytes)));

    // Report availability alongside the value so a consumer cannot mistake an
    // unmeasured stage for a free one.
    Json upscalerGpu = Json::object();
    upscalerGpu.set("measured", Json(s.upscalerGpuTimingAvailable));
    upscalerGpu.set("averageMs", Json(static_cast<double>(s.averageUpscalerGpuMs)));
    summary.set("upscalerGpu", std::move(upscalerGpu));

    Json fgGpu = Json::object();
    fgGpu.set("measured", Json(s.frameGenGpuTimingAvailable));
    fgGpu.set("averageMs", Json(static_cast<double>(s.averageFrameGenGpuMs)));
    summary.set("frameGenGpu", std::move(fgGpu));

    Json root = Json::object();
    root.set("schema", Json("beamng-fsr-benchmark/1"));
    root.set("environment", environment);
    root.set("summary", std::move(summary));

    if (includeFrames) {
        Json frames = Json::array();
        for (const auto& f : samples_) {
            Json e = Json::object();
            e.set("i", Json(std::to_string(f.index)));
            e.set("t", Json(f.timestampSeconds));
            e.set("cpuMs", Json(static_cast<double>(f.cpuFrameTimeMs)));
            if (f.upscalerGpuMs > 0.0f) e.set("upscalerMs", Json(static_cast<double>(f.upscalerGpuMs)));
            if (f.frameGenGpuMs > 0.0f) e.set("frameGenMs", Json(static_cast<double>(f.frameGenGpuMs)));
            e.set("generated", Json(f.wasGenerated));
            frames.push(std::move(e));
        }
        root.set("frames", std::move(frames));
    }
    return root;
}

} // namespace bfsr
