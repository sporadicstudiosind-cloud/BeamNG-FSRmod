// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include "beamng_fsr/beacon_signature.hpp"

using namespace bfsr;
using namespace bfsr::dxgi_format;

namespace {

ResourceDescriptor tex(uint64_t handle, uint32_t w, uint32_t h, uint32_t format,
                       uint64_t seq, bool rt = true, bool ds = false, bool uav = false) {
    ResourceDescriptor d;
    d.handle = handle;
    d.width = w;
    d.height = h;
    d.format = format;
    d.creationSequence = seq;
    d.allowRenderTarget = rt;
    d.allowDepthStencil = ds;
    d.allowUnorderedAccess = uav;
    return d;
}

BeaconSignature signature(uint32_t w, uint32_t h, uint32_t format) {
    BeaconSignature s;
    s.valid = true;
    s.width = w;
    s.height = h;
    s.format = format;
    s.label = "fsrDepthMvBeacon";
    return s;
}

} // namespace

TEST(Beacon, ExactMatchOnUniqueDescriptor) {
    const std::vector<ResourceDescriptor> res = {
        tex(1, 1920, 1080, kR16G16B16A16Float, 1),
        tex(2, 1600, 900, kR32G32Float, 2),      // the beacon
        tex(3, 1920, 1080, kD32Float, 3, false, true),
    };
    const auto m = matchBeacon(signature(1600, 900, kR32G32Float), res);
    CHECK(m.confidence == MatchConfidence::Exact);
    CHECK_EQ(m.handle, uint64_t{2});
    CHECK_EQ(m.candidateCount, size_t{1});
}

TEST(Beacon, NoMatchIsReportedClearly) {
    const std::vector<ResourceDescriptor> res = {tex(1, 1920, 1080, kR16G16Float, 1)};
    const auto m = matchBeacon(signature(1600, 900, kR32G32Float), res);
    CHECK(m.confidence == MatchConfidence::None);
    CHECK_EQ(m.handle, uint64_t{0});
    CHECK_CONTAINS(m.explanation, "no live resource matches");
    CHECK_CONTAINS(m.explanation, "1600x900");
}

TEST(Beacon, AmbiguityPrefersTheNewestResource) {
    // A resolution change leaves an identically-described stale target alive
    // until the engine releases it; the newest one is the live one.
    const std::vector<ResourceDescriptor> res = {
        tex(10, 1600, 900, kR32G32Float, 5),
        tex(11, 1600, 900, kR32G32Float, 42),
    };
    const auto m = matchBeacon(signature(1600, 900, kR32G32Float), res);
    CHECK(m.confidence == MatchConfidence::Ambiguous);
    CHECK_EQ(m.handle, uint64_t{11});
    CHECK_EQ(m.candidateCount, size_t{2});
    CHECK_CONTAINS(m.explanation, "most recently created");
}

TEST(Beacon, InvalidSignatureIsRejected) {
    const std::vector<ResourceDescriptor> res = {tex(1, 1600, 900, kR32G32Float, 1)};
    BeaconSignature s;
    s.valid = false;
    const auto m = matchBeacon(s, res);
    CHECK(m.confidence == MatchConfidence::None);
    CHECK_CONTAINS(m.explanation, "no beacon signature");
}

TEST(Beacon, MultisampledResourcesAreNeverMatched) {
    // FSR inputs must be single-sampled.
    auto r = tex(1, 1600, 900, kR32G32Float, 1);
    r.sampleCount = 4;
    const auto m = matchBeacon(signature(1600, 900, kR32G32Float), {r});
    CHECK(m.confidence == MatchConfidence::None);
}

TEST(Beacon, RenderTargetFlagIsRequiredWhenRequested) {
    const auto r = tex(1, 1600, 900, kR32G32Float, 1, /*rt=*/false);
    const auto m = matchBeacon(signature(1600, 900, kR32G32Float), {r});
    CHECK(m.confidence == MatchConfidence::None);
}

TEST(Beacon, UavFlagIsRequiredWhenRequested) {
    auto sig = signature(1600, 900, kR32G32Float);
    sig.expectUnorderedAccess = true;
    const auto withoutUav = tex(1, 1600, 900, kR32G32Float, 1, true, false, false);
    CHECK(matchBeacon(sig, {withoutUav}).confidence == MatchConfidence::None);
    const auto withUav = tex(2, 1600, 900, kR32G32Float, 1, true, false, true);
    CHECK(matchBeacon(sig, {withUav}).confidence == MatchConfidence::Exact);
}

TEST(Beacon, NonTexture2dIsIgnored) {
    auto r = tex(1, 1600, 900, kR32G32Float, 1);
    r.isTexture2D = false;
    CHECK(matchBeacon(signature(1600, 900, kR32G32Float), {r}).confidence == MatchConfidence::None);
}

// ---------------------------------------------------------------------------

TEST(Heuristic, IdentifiesUniqueDepthAndMotionVectors) {
    const std::vector<ResourceDescriptor> res = {
        tex(1, 1920, 1080, kD32Float, 1, false, true),
        tex(2, 1920, 1080, kR16G16Float, 2),
        tex(3, 1920, 1080, kR8G8B8A8Unorm, 3),
        tex(4, 512, 512, kR16G16Float, 4), // wrong size, must not confuse it
    };
    const auto c = classifyHeuristically(1920, 1080, res);
    CHECK(c.depth.confidence == MatchConfidence::Exact);
    CHECK_EQ(c.depth.handle, uint64_t{1});
    CHECK(c.motionVectors.confidence == MatchConfidence::Exact);
    CHECK_EQ(c.motionVectors.handle, uint64_t{2});
}

TEST(Heuristic, ReportsAmbiguityRatherThanGuessing) {
    const std::vector<ResourceDescriptor> res = {
        tex(1, 1920, 1080, kR16G16Float, 1),
        tex(2, 1920, 1080, kR16G16Float, 2),
        tex(3, 1920, 1080, kR16G16Float, 3),
    };
    const auto c = classifyHeuristically(1920, 1080, res);
    CHECK(c.motionVectors.confidence == MatchConfidence::Ambiguous);
    CHECK_EQ(c.motionVectors.candidateCount, size_t{3});
    CHECK_CONTAINS(c.motionVectors.explanation, "BeamNG FSR mod half");
}

TEST(Heuristic, TypelessDepthFormatsAreRecognised) {
    // Depth is commonly created typeless with separate DSV/SRV views.
    const std::vector<ResourceDescriptor> res = {
        tex(1, 1920, 1080, kR32Typeless, 1, false, true),
    };
    const auto c = classifyHeuristically(1920, 1080, res);
    CHECK(c.depth.confidence == MatchConfidence::Exact);
}

TEST(Heuristic, RequiresTheDepthStencilFlag) {
    // A colour texture that merely uses a depth-ish format is not the depth buffer.
    const std::vector<ResourceDescriptor> res = {
        tex(1, 1920, 1080, kD32Float, 1, true, /*ds=*/false),
    };
    const auto c = classifyHeuristically(1920, 1080, res);
    CHECK(c.depth.confidence == MatchConfidence::None);
}

TEST(Heuristic, EmptySetYieldsNothingFound) {
    const auto c = classifyHeuristically(1920, 1080, {});
    CHECK(c.depth.confidence == MatchConfidence::None);
    CHECK(c.motionVectors.confidence == MatchConfidence::None);
    CHECK_CONTAINS(c.depth.explanation, "no candidate depth");
}

TEST(Heuristic, UnknownRenderSizeIsHandled) {
    const auto c = classifyHeuristically(0, 0, {tex(1, 1920, 1080, kD32Float, 1, false, true)});
    CHECK(c.depth.confidence == MatchConfidence::None);
    CHECK_CONTAINS(c.depth.explanation, "render resolution unknown");
}

TEST(Heuristic, MippedOrArrayResourcesAreExcluded) {
    auto r = tex(1, 1920, 1080, kD32Float, 1, false, true);
    r.mipLevels = 5;
    CHECK(classifyHeuristically(1920, 1080, {r}).depth.confidence == MatchConfidence::None);

    auto a = tex(2, 1920, 1080, kR16G16Float, 1);
    a.depthOrArraySize = 6;
    CHECK(classifyHeuristically(1920, 1080, {a}).motionVectors.confidence == MatchConfidence::None);
}

TEST(FormatPredicates, MatchExpectedSets) {
    CHECK(isDepthFormat(kD32Float));
    CHECK(isDepthFormat(kD24UnormS8Uint));
    CHECK(isDepthFormat(kD16Unorm));
    CHECK(isDepthFormat(kR32Typeless));
    CHECK(!isDepthFormat(kR8G8B8A8Unorm));
    CHECK(!isDepthFormat(kR16G16Float));

    CHECK(isTypicalMotionVectorFormat(kR16G16Float));
    CHECK(isTypicalMotionVectorFormat(kR32G32Float));
    CHECK(!isTypicalMotionVectorFormat(kR8G8B8A8Unorm));
    CHECK(!isTypicalMotionVectorFormat(kD32Float));
}
