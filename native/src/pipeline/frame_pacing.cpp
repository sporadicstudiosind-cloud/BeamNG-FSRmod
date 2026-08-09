// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/frame_pacing.hpp"

#include <algorithm>
#include <cmath>

namespace bfsr {
namespace {

void appendReason(std::string& out, const char* text) {
    if (!out.empty()) out += ", ";
    out += text;
}

} // namespace

std::string describeSuppressMask(uint32_t mask) {
    if (mask == 0) return "none";
    std::string out;
    using R = FrameGenSuppressReason;
    const auto has = [mask](R r) { return (mask & static_cast<uint32_t>(r)) != 0; };
    if (has(R::Paused)) appendReason(out, "paused");
    if (has(R::Loading)) appendReason(out, "loading");
    if (has(R::MenuOrUiFocused)) appendReason(out, "menu/UI focused");
    if (has(R::PhotoMode)) appendReason(out, "photo mode");
    if (has(R::Replay)) appendReason(out, "replay");
    if (has(R::WindowOccludedOrMinimised)) appendReason(out, "window occluded or minimised");
    if (has(R::ResolutionChanged)) appendReason(out, "resolution changed");
    if (has(R::CameraCut)) appendReason(out, "camera cut");
    if (has(R::DeviceLost)) appendReason(out, "device lost");
    if (has(R::FrameTimeAnomaly)) appendReason(out, "frame-time hitch");
    if (has(R::ExplicitlyDisabled)) appendReason(out, "disabled");
    if (has(R::InputsUnavailable)) appendReason(out, "render inputs unavailable");
    return out;
}

FrameGenerationGate::Decision FrameGenerationGate::evaluate(const FrameGenGateInput& in) {
    using R = FrameGenSuppressReason;
    uint32_t mask = 0;

    if (!in.enabledByPlan) mask |= static_cast<uint32_t>(R::ExplicitlyDisabled);
    if (!in.inputsAvailable) mask |= static_cast<uint32_t>(R::InputsUnavailable);
    if (in.paused) mask |= static_cast<uint32_t>(R::Paused);
    if (in.loading) mask |= static_cast<uint32_t>(R::Loading);
    if (in.uiFocused) mask |= static_cast<uint32_t>(R::MenuOrUiFocused);
    if (in.photoMode) mask |= static_cast<uint32_t>(R::PhotoMode);
    if (in.replay) mask |= static_cast<uint32_t>(R::Replay);
    if (in.windowOccluded) mask |= static_cast<uint32_t>(R::WindowOccludedOrMinimised);
    if (in.deviceLost) mask |= static_cast<uint32_t>(R::DeviceLost);
    if (in.resolutionChangedThisFrame) mask |= static_cast<uint32_t>(R::ResolutionChanged);
    if (in.cameraCutThisFrame) mask |= static_cast<uint32_t>(R::CameraCut);

    // Hitch detection. Seed the average from the first sample so a slow first
    // frame does not read as an anomaly against a zero baseline.
    const float ft = in.frameTimeMs > 0.0f ? in.frameTimeMs : 0.0f;
    if (ft > 0.0f) {
        if (avgFrameTimeMs_ <= 0.0f) {
            avgFrameTimeMs_ = ft;
        } else {
            const float threshold = std::max(kHitchFloorMs, avgFrameTimeMs_ * kHitchMultiplier);
            if (ft > threshold) mask |= static_cast<uint32_t>(R::FrameTimeAnomaly);
            // Exponential moving average, ~1s worth of history at 60 fps.
            avgFrameTimeMs_ = avgFrameTimeMs_ * 0.98f + ft * 0.02f;
        }
    }

    Decision d;
    d.suppressMask = mask;

    if (mask != 0) {
        settleCounter_ = kSettleFrames;
        // A reset is only meaningful once, at the moment suppression begins;
        // repeating it every frame of a long pause would keep clearing history
        // that is already empty.
        d.requestReset = !wasSuppressed_;
        wasSuppressed_ = true;
        d.allowInterpolation = false;
    } else if (settleCounter_ > 0) {
        --settleCounter_;
        // Still settling: the previous frame is not a trustworthy interpolation
        // partner yet. Keep resetting so history rebuilds from clean frames.
        d.requestReset = true;
        d.allowInterpolation = false;
    } else {
        d.requestReset = wasSuppressed_;
        wasSuppressed_ = false;
        d.allowInterpolation = true;
    }

    lastMask_ = mask;
    return d;
}

void FrameGenerationGate::reset() {
    avgFrameTimeMs_ = 0.0f;
    settleCounter_ = kSettleFrames;
    lastMask_ = 0;
    wasSuppressed_ = true;
}

// ---------------------------------------------------------------------------

FrameTimeTracker::FrameTimeTracker(size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {
    samples_.reserve(capacity_);
}

void FrameTimeTracker::addFrame(float frameTimeMs) {
    if (!(frameTimeMs > 0.0f) || !std::isfinite(frameTimeMs)) return;
    if (samples_.size() < capacity_) {
        samples_.push_back(frameTimeMs);
        sumMs_ += frameTimeMs;
    } else {
        wrapped_ = true;
        sumMs_ -= samples_[writeIndex_];
        samples_[writeIndex_] = frameTimeMs;
        sumMs_ += frameTimeMs;
        writeIndex_ = (writeIndex_ + 1) % capacity_;
    }
}

void FrameTimeTracker::clear() {
    samples_.clear();
    samples_.reserve(capacity_);
    writeIndex_ = 0;
    wrapped_ = false;
    sumMs_ = 0.0;
}

float FrameTimeTracker::averageFrameTimeMs() const noexcept {
    if (samples_.empty()) return 0.0f;
    return static_cast<float>(sumMs_ / static_cast<double>(samples_.size()));
}

float FrameTimeTracker::averageFps() const noexcept {
    const float ms = averageFrameTimeMs();
    return ms > 0.0f ? 1000.0f / ms : 0.0f;
}

float FrameTimeTracker::percentileLowFps(double p) const {
    if (samples_.empty() || p <= 0.0 || p > 1.0) return 0.0f;

    std::vector<float> sorted(samples_);
    // Descending: the worst frames are the longest ones.
    std::sort(sorted.begin(), sorted.end(), std::greater<float>());

    size_t count = static_cast<size_t>(std::ceil(static_cast<double>(sorted.size()) * p));
    count = std::max<size_t>(1, std::min(count, sorted.size()));

    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) sum += sorted[i];
    const double meanMs = sum / static_cast<double>(count);
    return meanMs > 0.0 ? static_cast<float>(1000.0 / meanMs) : 0.0f;
}

float FrameTimeTracker::minFrameTimeMs() const noexcept {
    if (samples_.empty()) return 0.0f;
    return *std::min_element(samples_.begin(), samples_.end());
}

float FrameTimeTracker::maxFrameTimeMs() const noexcept {
    if (samples_.empty()) return 0.0f;
    return *std::max_element(samples_.begin(), samples_.end());
}

} // namespace bfsr
