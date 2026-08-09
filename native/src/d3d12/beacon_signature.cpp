// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/beacon_signature.hpp"

#include <algorithm>

namespace bfsr {

bool isDepthFormat(uint32_t format) noexcept {
    using namespace dxgi_format;
    switch (format) {
        case kD32Float:
        case kD32FloatS8X24Uint:
        case kD24UnormS8Uint:
        case kD16Unorm:
        case kR32Typeless:      // A depth texture is frequently created typeless
        case kR24G8Typeless:    // and given depth/SRV views separately.
        case kR32G8X24Typeless:
        case kR16Typeless:
            return true;
        default:
            return false;
    }
}

bool isTypicalMotionVectorFormat(uint32_t format) noexcept {
    using namespace dxgi_format;
    // FSR accepts a 2-component signed velocity buffer. R16G16_FLOAT is the
    // near-universal choice; R32G32_FLOAT appears in higher-precision paths.
    return format == kR16G16Float || format == kR32G32Float;
}

const char* toString(MatchConfidence c) noexcept {
    switch (c) {
        case MatchConfidence::None: return "none";
        case MatchConfidence::Ambiguous: return "ambiguous";
        case MatchConfidence::Exact: return "exact";
    }
    return "none";
}

namespace {

const ResourceDescriptor* newest(const std::vector<const ResourceDescriptor*>& v) {
    const ResourceDescriptor* best = nullptr;
    for (const auto* d : v) {
        if (!best || d->creationSequence > best->creationSequence) best = d;
    }
    return best;
}

} // namespace

MatchResult matchBeacon(const BeaconSignature& sig,
                        const std::vector<ResourceDescriptor>& resources) {
    MatchResult result;
    if (!sig.valid || sig.width == 0 || sig.height == 0) {
        result.explanation = "no beacon signature has been published by the BeamNG mod yet";
        return result;
    }

    std::vector<const ResourceDescriptor*> candidates;
    for (const auto& r : resources) {
        if (!r.isTexture2D) continue;
        if (r.width != sig.width || r.height != sig.height) continue;
        if (r.format != sig.format) continue;
        if (sig.expectRenderTarget && !r.allowRenderTarget) continue;
        if (sig.expectUnorderedAccess && !r.allowUnorderedAccess) continue;
        if (r.sampleCount != 1) continue; // FSR inputs must be single-sampled.
        candidates.push_back(&r);
    }

    result.candidateCount = candidates.size();
    if (candidates.empty()) {
        result.explanation = "no live resource matches beacon '" + sig.label + "' (" +
                             std::to_string(sig.width) + "x" + std::to_string(sig.height) +
                             ", format " + std::to_string(sig.format) + ")";
        return result;
    }

    const uint32_t expected = sig.expectedCount == 0 ? 1u : sig.expectedCount;
    if (sig.ordinal >= expected) {
        result.explanation = "beacon '" + sig.label + "' declares ordinal " +
                             std::to_string(sig.ordinal) + " of " + std::to_string(expected) +
                             ", which is out of range; the mod half and the bridge are not "
                             "the same version";
        return result;
    }

    if (candidates.size() < expected) {
        // Fewer targets are live than the mod says it created. Picking one
        // would be a coin flip between depth and motion vectors, and feeding
        // FSR the wrong one produces a broken image rather than a visible
        // failure, so this reports nothing found.
        result.explanation = "beacon '" + sig.label + "' expected " + std::to_string(expected) +
                             " matching resources but found " +
                             std::to_string(candidates.size()) +
                             "; the beacon targets have not all been created yet";
        return result;
    }

    // Order by creation, oldest first. The mod's PostEffect render priorities
    // fix the order the engine allocates these targets in, so position within
    // the newest group of `expected` is a reliable identity.
    std::sort(candidates.begin(), candidates.end(),
              [](const ResourceDescriptor* a, const ResourceDescriptor* b) {
                  return a->creationSequence < b->creationSequence;
              });
    const size_t base = candidates.size() - expected;
    const ResourceDescriptor* chosen = candidates[base + sig.ordinal];
    result.handle = chosen->handle;

    if (candidates.size() == expected) {
        result.confidence = MatchConfidence::Exact;
        result.explanation = "beacon '" + sig.label + "' matched resource " +
                             std::to_string(sig.ordinal + 1) + " of " + std::to_string(expected);
    } else {
        // More than the mod created: something else in the engine happens to
        // share the description, or a previous resolution's targets are still
        // alive. Taking the newest group is the right guess for the stale case,
        // which is the common one, but it is a guess and is reported as such.
        result.confidence = MatchConfidence::Ambiguous;
        result.explanation = "beacon '" + sig.label + "' matched " +
                             std::to_string(candidates.size()) + " resources but expected " +
                             std::to_string(expected) +
                             "; selected from the most recently created group (stale targets "
                             "from a previous resolution are the usual cause)";
    }
    return result;
}

HeuristicClassification classifyHeuristically(uint32_t renderWidth,
                                              uint32_t renderHeight,
                                              const std::vector<ResourceDescriptor>& resources) {
    HeuristicClassification out;
    if (renderWidth == 0 || renderHeight == 0) {
        out.depth.explanation = "render resolution unknown";
        out.motionVectors.explanation = "render resolution unknown";
        return out;
    }

    std::vector<const ResourceDescriptor*> depthCandidates;
    std::vector<const ResourceDescriptor*> mvCandidates;

    for (const auto& r : resources) {
        if (!r.isTexture2D || r.sampleCount != 1) continue;
        if (r.width != renderWidth || r.height != renderHeight) continue;
        if (r.mipLevels > 1 || r.depthOrArraySize != 1) continue;

        if (r.allowDepthStencil && isDepthFormat(r.format)) {
            depthCandidates.push_back(&r);
        } else if (isTypicalMotionVectorFormat(r.format) &&
                   (r.allowRenderTarget || r.allowUnorderedAccess)) {
            mvCandidates.push_back(&r);
        }
    }

    const auto finish = [](const std::vector<const ResourceDescriptor*>& c,
                           const char* what) {
        MatchResult m;
        m.candidateCount = c.size();
        if (c.empty()) {
            m.explanation = std::string("no candidate ") + what +
                            " texture at the render resolution was observed";
            return m;
        }
        const ResourceDescriptor* chosen = newest(c);
        m.handle = chosen->handle;
        if (c.size() == 1) {
            m.confidence = MatchConfidence::Exact;
            m.explanation = std::string("one candidate ") + what + " texture identified";
        } else {
            m.confidence = MatchConfidence::Ambiguous;
            m.explanation = std::string(what) + " could not be identified unambiguously (" +
                            std::to_string(c.size()) +
                            " candidates). Install the BeamNG FSR mod half so the buffers "
                            "are published explicitly instead of guessed.";
        }
        return m;
    };

    out.depth = finish(depthCandidates, "depth");
    out.motionVectors = finish(mvCandidates, "motion vector");
    return out;
}

} // namespace bfsr
