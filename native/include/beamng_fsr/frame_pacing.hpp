// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bfsr {

/// Reasons frame generation must be suppressed for a frame.
///
/// Interpolating between two frames that are not part of continuous motion
/// produces visible artefacts (a menu fading in, a loading screen, the first
/// frame after a camera cut) and interpolating while the game is paused
/// produces duplicate frames that add latency for nothing. Each of these is a
/// distinct condition with a distinct recovery, so they are tracked separately
/// rather than collapsed into one boolean.
enum class FrameGenSuppressReason : uint32_t {
    None = 0,
    Paused = 1u << 0,
    Loading = 1u << 1,
    MenuOrUiFocused = 1u << 2,
    PhotoMode = 1u << 3,
    Replay = 1u << 4,
    WindowOccludedOrMinimised = 1u << 5,
    ResolutionChanged = 1u << 6,
    CameraCut = 1u << 7,
    DeviceLost = 1u << 8,
    FrameTimeAnomaly = 1u << 9,   ///< A hitch large enough that interpolation would smear.
    ExplicitlyDisabled = 1u << 10,
    InputsUnavailable = 1u << 11,
};

std::string describeSuppressMask(uint32_t mask);

/// Live state pushed in from the mod half over IPC plus what the bridge itself
/// observes. Kept as plain data so the gate is a pure function of it.
struct FrameGenGateInput {
    bool paused = false;
    bool loading = false;
    bool uiFocused = false;
    bool photoMode = false;
    bool replay = false;
    bool windowOccluded = false;
    bool deviceLost = false;
    bool inputsAvailable = true;
    bool enabledByPlan = true;
    bool resolutionChangedThisFrame = false;
    bool cameraCutThisFrame = false;
    /// Milliseconds since the previous rendered frame.
    float frameTimeMs = 16.6f;
};

/// Decides whether interpolation may run this frame, and enforces a settling
/// period after any disruption.
///
/// The settling period matters: FSR's frame-generation context resets its
/// history when `reset` is set, and re-enabling interpolation on the very next
/// frame would interpolate against a stale previous frame. Holding off for a
/// few frames costs nothing perceptible and removes a whole class of artefacts
/// around level loads, camera changes and alt-tab.
class FrameGenerationGate {
public:
    /// Frames to stay suppressed after a disruption clears.
    static constexpr uint32_t kSettleFrames = 3;
    /// A frame this much longer than the running average is treated as a hitch.
    static constexpr float kHitchMultiplier = 4.0f;
    static constexpr float kHitchFloorMs = 50.0f;

    struct Decision {
        bool allowInterpolation = false;
        bool requestReset = false; ///< Pass to FSR's `reset` on the next dispatch.
        uint32_t suppressMask = 0;
    };

    Decision evaluate(const FrameGenGateInput& in);
    void reset();

    [[nodiscard]] uint32_t lastSuppressMask() const noexcept { return lastMask_; }
    [[nodiscard]] float averageFrameTimeMs() const noexcept { return avgFrameTimeMs_; }

private:
    float avgFrameTimeMs_ = 0.0f;
    /// Starts at the full settle count, not zero: the first frames of a session
    /// have no previous frame to interpolate against, exactly like the frames
    /// after a disruption.
    uint32_t settleCounter_ = kSettleFrames;
    uint32_t lastMask_ = 0;
    bool wasSuppressed_ = true;
};

/// Rolling frame-time statistics for the diagnostics panel and benchmark export.
///
/// Keeps a bounded history so percentile lows can be computed exactly rather
/// than estimated — 1% and 0.1% lows are the numbers that actually describe
/// stutter, and an approximation of them is not worth reporting.
class FrameTimeTracker {
public:
    explicit FrameTimeTracker(size_t capacity = 4096);

    void addFrame(float frameTimeMs);
    void clear();

    [[nodiscard]] size_t sampleCount() const noexcept { return samples_.size(); }
    [[nodiscard]] float averageFps() const noexcept;
    [[nodiscard]] float averageFrameTimeMs() const noexcept;
    /// Percentile *low* in the gaming sense: the mean of the worst `p` fraction
    /// of frames, expressed as fps. `p` is a fraction, e.g. 0.01 for 1% lows.
    [[nodiscard]] float percentileLowFps(double p) const;
    [[nodiscard]] float minFrameTimeMs() const noexcept;
    [[nodiscard]] float maxFrameTimeMs() const noexcept;
    [[nodiscard]] const std::vector<float>& samples() const noexcept { return samples_; }

private:
    std::vector<float> samples_;
    size_t capacity_;
    size_t writeIndex_ = 0;
    bool wrapped_ = false;
    double sumMs_ = 0.0;
};

} // namespace bfsr
