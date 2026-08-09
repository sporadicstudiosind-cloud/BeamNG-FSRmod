// SPDX-License-Identifier: MIT
//
// Each product owns a separate channel. These tests pin the property that makes
// the split work: neither document can be produced or consumed by the other's
// code path, and each carries everything its product needs on its own.
#include "test_framework.hpp"

#include "beamng_fsr/ipc_protocol.hpp"

using namespace bfsr;

namespace {

SharedFrameState sharedState() {
    SharedFrameState s;
    s.paused = true;
    s.replay = true;
    s.cameraValid = true;
    s.cameraNear = 0.05f;
    s.cameraFar = 12000.0f;
    s.cameraFovVerticalRadians = 1.05f;
    s.beacon.valid = true;
    s.beacon.width = 1600;
    s.beacon.height = 900;
    s.beacon.format = 2;
    s.beacon.label = "fsrDepthMvBeacon";
    return s;
}

} // namespace

TEST(Ipc, FrameGenRequestRoundTrips) {
    FrameGenRequest r;
    r.sequence = 12345678901234ull;
    r.modVersion = "0.2.0";
    r.beamngVersion = "0.39.2";
    r.settings.enabled = true;
    r.settings.implementation = FrameGenSelection::Ml;
    r.settings.antiLag2 = true;
    r.settings.antiLagMaxFps = 144;
    r.shared = sharedState();

    const FrameGenRequest out = FrameGenRequest::fromJson(r.toJson());
    CHECK_EQ(out.sequence, r.sequence);
    CHECK(out.settings.enabled);
    CHECK(out.settings.implementation == FrameGenSelection::Ml);
    CHECK(out.settings.antiLag2);
    CHECK_EQ(out.settings.antiLagMaxFps, 144u);
    CHECK(out.shared.paused);
    CHECK(out.shared.replay);
    CHECK(!out.shared.loading);
    CHECK(out.shared.beacon.valid);
    CHECK_EQ(out.shared.beacon.width, 1600u);
    CHECK_NEAR(out.shared.cameraNear, 0.05, 1e-6);
}

TEST(Ipc, UpscaleRequestRoundTrips) {
    UpscaleRequest r;
    r.sequence = 99;
    r.settings.enabled = true;
    r.settings.implementation = UpscalerSelection::Analytical;
    r.settings.quality = QualityMode::Balanced;
    r.settings.sharpness = 0.25f;
    r.shared = sharedState();

    const UpscaleRequest out = UpscaleRequest::fromJson(r.toJson());
    CHECK_EQ(out.sequence, uint64_t{99});
    CHECK(out.settings.implementation == UpscalerSelection::Analytical);
    CHECK(out.settings.quality == QualityMode::Balanced);
    CHECK_NEAR(out.settings.sharpness, 0.25, 1e-6);
    CHECK(out.shared.beacon.valid);
}

TEST(Ipc, BothRequestsCarryTheSharedStateIndependently) {
    // Duplication is deliberate: it is what lets either product run without the
    // other's mod half being installed to produce a shared file.
    FrameGenRequest fg;
    fg.shared = sharedState();
    UpscaleRequest up;
    up.shared = sharedState();

    CHECK_STR_EQ(fg.toJson()["shared"].dump(-1), up.toJson()["shared"].dump(-1));
}

TEST(Ipc, DocumentsIdentifyTheirProduct) {
    CHECK_STR_EQ(FrameGenRequest{}.toJson()["module"].asString(), "framegen");
    CHECK_STR_EQ(UpscaleRequest{}.toJson()["module"].asString(), "upscaling");
    CHECK_STR_EQ(FrameGenStatus{}.toJson()["module"].asString(), "framegen");
    CHECK_STR_EQ(UpscaleStatus{}.toJson()["module"].asString(), "upscaling");
}

TEST(Ipc, LargeSequenceNumbersSurviveAsStrings) {
    // JSON numbers are doubles; a 64-bit counter would lose precision.
    FrameGenRequest r;
    r.sequence = 9007199254740993ull; // 2^53 + 1
    CHECK_EQ(FrameGenRequest::fromJson(r.toJson()).sequence, r.sequence);
}

TEST(Ipc, FrameGenStatusRoundTrips) {
    FrameGenStatus s;
    s.moduleInstalled = true;
    s.common.bridgeVersion = "0.2.0";
    s.common.bridgeLoaded = true;
    s.common.initialised = true;
    s.common.renderedFps = 78.5f;
    s.common.displayedFps = 152.0f;
    s.common.framesRendered = 100000;
    s.gpuMs = 1.75f;
    s.vramBytes = 5368709120ull;
    s.framesGenerated = 99999;
    s.observedGeneratedPerFrame = 1;
    s.suppressReason = "paused";
    s.antiLagActive = true;
    s.versions = {{2001, "FSR Frame Generation 4.0.1", true}};

    const FrameGenStatus out = FrameGenStatus::fromJson(s.toJson());
    CHECK(out.moduleInstalled);
    CHECK_NEAR(out.common.renderedFps, 78.5, 1e-4);
    CHECK_NEAR(out.common.displayedFps, 152.0, 1e-4);
    CHECK_EQ(out.vramBytes, uint64_t{5368709120});
    CHECK_EQ(out.framesGenerated, uint64_t{99999});
    CHECK_EQ(out.observedGeneratedPerFrame, 1u);
    CHECK(out.antiLagActive);
    CHECK_EQ(out.versions.size(), size_t{1});
    CHECK(out.versions[0].isMachineLearning);
}

TEST(Ipc, UpscaleStatusRoundTrips) {
    UpscaleStatus s;
    s.moduleInstalled = true;
    s.common.bridgeLoaded = true;
    s.common.depthSource = "beacon (exact)";
    s.gpuMs = 0.9f;
    s.vramBytes = 1073741824ull;
    s.jitterAvailable = false;
    s.versions = {{1002, "FSR 3.1.5", false}};

    const UpscaleStatus out = UpscaleStatus::fromJson(s.toJson());
    CHECK(out.moduleInstalled);
    CHECK_STR_EQ(out.common.depthSource, "beacon (exact)");
    CHECK_EQ(out.vramBytes, uint64_t{1073741824});
    CHECK(!out.jitterAvailable);
    CHECK(!out.versions[0].isMachineLearning);
}

TEST(Ipc, StatusEmitsHumanReadableEnvironment) {
    FrameGenStatus s;
    s.common.gpu.vendor = GpuVendor::AMD;
    s.common.gpu.name = "AMD Radeon RX 9070 XT";
    s.common.system.renderer = RendererApi::D3D12;
    s.common.capabilities = evaluateCapabilities(s.common.gpu, s.common.system);

    const Json j = s.toJson();
    CHECK_STR_EQ(j["common"]["environment"]["gpu"]["name"].asString(), "AMD Radeon RX 9070 XT");
    CHECK_EQ(j["common"]["environment"]["features"].size(), size_t{5});
    CHECK(!Json::parse(j.dump(2)).isNull());
}

TEST(Ipc, ProtocolVersionGate) {
    CHECK(isProtocolCompatible(kIpcProtocolVersion));
    CHECK(!isProtocolCompatible(kIpcProtocolVersion + 1));
    CHECK(!isProtocolCompatible(kIpcProtocolVersion - 1));
    CHECK(!isProtocolCompatible(0));
}

TEST(Ipc, GarbageDocumentsYieldSafeDefaults) {
    // A half-written file must never be interpreted as "turn everything on".
    const FrameGenRequest fg = FrameGenRequest::fromJson(Json::parse("{\"settings\":\"nope\"}"));
    CHECK(!fg.settings.enabled);
    CHECK(!isProtocolCompatible(fg.protocolVersion));

    const UpscaleRequest up = UpscaleRequest::fromJson(Json{});
    CHECK(!up.settings.enabled);
    CHECK(!isProtocolCompatible(up.protocolVersion));

    CHECK(!FrameGenStatus::fromJson(Json{}).common.bridgeLoaded);
    CHECK(!UpscaleStatus::fromJson(Json{}).moduleInstalled);
}

TEST(Ipc, TruncatedJsonIsRejectedNotPartiallyApplied) {
    FrameGenRequest full;
    full.settings.enabled = true;
    full.settings.implementation = FrameGenSelection::Ml;
    const std::string text = full.toJson().dump(-1);

    std::string err;
    const Json j = Json::parse(text.substr(0, text.size() / 2), &err);
    CHECK(j.isNull());
    CHECK(!err.empty());
    CHECK(!FrameGenRequest::fromJson(j).settings.enabled);
}
