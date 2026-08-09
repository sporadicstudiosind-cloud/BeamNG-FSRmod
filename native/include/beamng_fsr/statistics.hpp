// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "beamng_fsr/frame_pacing.hpp"
#include "beamng_fsr/json.hpp"

namespace bfsr {

/// One recorded frame. GPU timings are only populated when a timestamp query
/// resolved; a zero means "not measured", never "zero cost". The distinction
/// matters because reporting an unmeasured stage as free would be a fabricated
/// performance claim.
struct FrameSample {
    uint64_t index = 0;
    double timestampSeconds = 0.0;
    float cpuFrameTimeMs = 0.0f;
    float upscalerGpuMs = 0.0f;
    float frameGenGpuMs = 0.0f;
    bool wasGenerated = false;
    uint64_t vramBytes = 0;
};

/// Collects frames for the optional benchmark mode and exports them.
///
/// Rendered and generated frames are counted separately throughout: a
/// frame-generation result that quoted a single blended "FPS" would be exactly
/// the misleading number the brief prohibits.
class BenchmarkSession {
public:
    BenchmarkSession() = default;

    void start(std::string label);
    void stop();
    [[nodiscard]] bool isRunning() const noexcept { return running_; }
    [[nodiscard]] const std::string& label() const noexcept { return label_; }

    void addSample(const FrameSample& sample);

    [[nodiscard]] size_t sampleCount() const noexcept { return samples_.size(); }
    [[nodiscard]] uint64_t renderedFrameCount() const noexcept { return renderedCount_; }
    [[nodiscard]] uint64_t generatedFrameCount() const noexcept { return generatedCount_; }

    struct Summary {
        std::string label;
        uint64_t renderedFrames = 0;
        uint64_t generatedFrames = 0;
        double durationSeconds = 0.0;
        float averageRenderedFps = 0.0f;
        float averageDisplayedFps = 0.0f;
        float onePercentLowFps = 0.0f;
        float pointOnePercentLowFps = 0.0f;
        float averageFrameTimeMs = 0.0f;
        float minFrameTimeMs = 0.0f;
        float maxFrameTimeMs = 0.0f;
        /// Averages over the frames where the stage actually reported a timing.
        float averageUpscalerGpuMs = 0.0f;
        float averageFrameGenGpuMs = 0.0f;
        bool upscalerGpuTimingAvailable = false;
        bool frameGenGpuTimingAvailable = false;
        uint64_t peakVramBytes = 0;
    };

    [[nodiscard]] Summary summarise() const;

    /// One row per frame; the header names units explicitly.
    [[nodiscard]] std::string toCsv() const;
    /// Summary plus environment block; per-frame data included when `includeFrames`.
    [[nodiscard]] Json toJson(const Json& environment, bool includeFrames) const;

private:
    bool running_ = false;
    std::string label_;
    std::vector<FrameSample> samples_;
    FrameTimeTracker renderedFrameTimes_{1u << 16};
    uint64_t renderedCount_ = 0;
    uint64_t generatedCount_ = 0;
    double firstTimestamp_ = 0.0;
    double lastTimestamp_ = 0.0;
    uint64_t peakVram_ = 0;
};

} // namespace bfsr
