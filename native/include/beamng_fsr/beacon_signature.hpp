// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bfsr {

/// Portable mirror of the parts of D3D12_RESOURCE_DESC that matter for
/// identification. Kept free of Windows types so the matching logic — the part
/// most likely to be subtly wrong — is unit-testable on any platform.
struct ResourceDescriptor {
    uint64_t handle = 0;     ///< Opaque; the ID3D12Resource* on Windows.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depthOrArraySize = 1;
    uint32_t mipLevels = 1;
    uint32_t format = 0;     ///< DXGI_FORMAT value.
    uint32_t sampleCount = 1;
    bool allowRenderTarget = false;
    bool allowDepthStencil = false;
    bool allowUnorderedAccess = false;
    bool isTexture2D = true;
    uint64_t creationSequence = 0; ///< Monotonic counter; higher == created later.
};

/// DXGI_FORMAT values used by the matcher. Duplicated here rather than
/// including dxgiformat.h so the logic stays portable; the D3D12 translation
/// unit static_asserts these against the real enum.
namespace dxgi_format {
constexpr uint32_t kUnknown = 0;
constexpr uint32_t kR32G32B32A32Float = 2;
constexpr uint32_t kR16G16B16A16Float = 10;
constexpr uint32_t kR32G32Float = 16;
constexpr uint32_t kR32G8X24Typeless = 19;
constexpr uint32_t kD32FloatS8X24Uint = 20;
constexpr uint32_t kR11G11B10Float = 26;
constexpr uint32_t kR8G8B8A8Unorm = 28;
constexpr uint32_t kR16G16Float = 34;
constexpr uint32_t kR32Typeless = 39;
constexpr uint32_t kD32Float = 40;
constexpr uint32_t kR32Float = 41;
constexpr uint32_t kR24G8Typeless = 44;
constexpr uint32_t kD24UnormS8Uint = 45;
constexpr uint32_t kR16Typeless = 53;
constexpr uint32_t kD16Unorm = 55;
constexpr uint32_t kB8G8R8A8Unorm = 87;
} // namespace dxgi_format

bool isDepthFormat(uint32_t format) noexcept;
bool isTypicalMotionVectorFormat(uint32_t format) noexcept;

/// What the mod half published over IPC about the render target it created.
///
/// Identification is by construction rather than inference: the mod tells the
/// bridge the exact dimensions and format of a target it created itself, so the
/// bridge is looking for something it has a description of, not guessing which
/// of the engine's textures is the depth buffer. That is what makes this robust
/// across BeamNG updates — nothing here depends on BeamNG's internals.
struct BeaconSignature {
    bool valid = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    bool expectRenderTarget = true;
    bool expectUnorderedAccess = false;
    /// Human-readable label for logs, e.g. "fsrDepthMvBeacon".
    std::string label;
};

enum class MatchConfidence {
    None,        ///< Nothing matched.
    Ambiguous,   ///< Several candidates matched; one was chosen by tie-break.
    Exact,       ///< Exactly one candidate matched the signature.
};

const char* toString(MatchConfidence c) noexcept;

struct MatchResult {
    MatchConfidence confidence = MatchConfidence::None;
    uint64_t handle = 0;
    size_t candidateCount = 0;
    std::string explanation;
};

/// Finds the resource matching a beacon signature.
///
/// Ties are broken toward the most recently created resource, because a
/// renderer restart or resolution change leaves stale resources of identical
/// description alive until the engine releases them, and the newest is the one
/// currently in use.
MatchResult matchBeacon(const BeaconSignature& signature,
                        const std::vector<ResourceDescriptor>& resources);

/// Heuristic classification used only when the mod half is absent.
///
/// Deliberately conservative: it reports Ambiguous (and the caller then refuses
/// to run) far more readily than it guesses. Feeding FSR the wrong texture
/// produces a broken image, and silently doing that is worse than staying off.
struct HeuristicClassification {
    MatchResult depth;
    MatchResult motionVectors;
};

HeuristicClassification classifyHeuristically(uint32_t renderWidth,
                                              uint32_t renderHeight,
                                              const std::vector<ResourceDescriptor>& resources);

} // namespace bfsr
