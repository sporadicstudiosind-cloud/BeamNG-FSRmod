// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include "beamng_fsr/ipc_protocol.hpp"

using namespace bfsr;

TEST(Ipc, RequestRoundTrips) {
    IpcRequest r;
    r.sequence = 12345678901234ull;
    r.modVersion = "0.1.0";
    r.beamngVersion = "0.39.2";
    r.settings.enabled = true;
    r.settings.upscaler = UpscalerSelection::Ml;
    r.paused = true;
    r.replay = true;
    r.cameraNear = 0.05f;
    r.cameraFar = 12000.0f;
    r.cameraFovVerticalRadians = 1.05f;
    r.cameraParamsValid = true;
    r.depthMvBeacon.valid = true;
    r.depthMvBeacon.width = 1600;
    r.depthMvBeacon.height = 900;
    r.depthMvBeacon.format = 16;
    r.depthMvBeacon.label = "fsrDepthMvBeacon";

    const IpcRequest out = IpcRequest::fromJson(r.toJson());
    CHECK_EQ(out.sequence, r.sequence);
    CHECK_STR_EQ(out.beamngVersion, "0.39.2");
    CHECK(out.settings.enabled);
    CHECK(out.settings.upscaler == UpscalerSelection::Ml);
    CHECK(out.paused);
    CHECK(out.replay);
    CHECK(!out.loading);
    CHECK_NEAR(out.cameraNear, 0.05, 1e-6);
    CHECK_NEAR(out.cameraFar, 12000.0, 1e-3);
    CHECK(out.cameraParamsValid);
    CHECK(out.depthMvBeacon.valid);
    CHECK_EQ(out.depthMvBeacon.width, 1600u);
    CHECK_STR_EQ(out.depthMvBeacon.label, "fsrDepthMvBeacon");
}

TEST(Ipc, LargeSequenceNumbersSurviveAsStrings) {
    // JSON numbers are doubles; a 64-bit counter would lose precision.
    IpcRequest r;
    r.sequence = 9007199254740993ull; // 2^53 + 1, not representable as a double
    CHECK_EQ(IpcRequest::fromJson(r.toJson()).sequence, r.sequence);
}

TEST(Ipc, StatusRoundTrips) {
    IpcStatus s;
    s.sequence = 42;
    s.bridgeVersion = "0.1.0";
    s.bridgeLoaded = true;
    s.initialised = true;
    s.renderedFps = 78.5f;
    s.displayedFps = 152.0f;
    s.onePercentLowFps = 55.25f;
    s.pointOnePercentLowFps = 40.0f;
    s.upscalerVramBytes = 5368709120ull;
    s.framesRendered = 100000;
    s.framesGenerated = 99999;
    s.depthSource = "beacon (exact)";
    s.frameGenSuppressReason = "paused";
    s.upscalerVersions = {{1001, "FSR Upscaling 4.1.1", true}};
    s.frameGenVersions = {{2002, "FSR Frame Generation 3.1.6", false}};
    s.recentLog = {"line one", "line two"};

    const IpcStatus out = IpcStatus::fromJson(s.toJson());
    CHECK_EQ(out.sequence, uint64_t{42});
    CHECK(out.bridgeLoaded);
    CHECK_NEAR(out.renderedFps, 78.5, 1e-4);
    CHECK_NEAR(out.displayedFps, 152.0, 1e-4);
    CHECK_EQ(out.upscalerVramBytes, uint64_t{5368709120});
    CHECK_EQ(out.framesGenerated, uint64_t{99999});
    CHECK_STR_EQ(out.depthSource, "beacon (exact)");
    CHECK_EQ(out.upscalerVersions.size(), size_t{1});
    CHECK_EQ(out.upscalerVersions[0].id, uint64_t{1001});
    CHECK(out.upscalerVersions[0].isMachineLearning);
    CHECK_EQ(out.recentLog.size(), size_t{2});
    CHECK_STR_EQ(out.recentLog[1], "line two");
}

TEST(Ipc, StatusEmitsHumanReadableEnvironment) {
    IpcStatus s;
    s.gpu.vendor = GpuVendor::AMD;
    s.gpu.name = "AMD Radeon RX 9070 XT";
    s.system.renderer = RendererApi::D3D12;
    s.capabilities = evaluateCapabilities(s.gpu, s.system);

    const Json j = s.toJson();
    CHECK_STR_EQ(j["environment"]["gpu"]["name"].asString(), "AMD Radeon RX 9070 XT");
    CHECK_EQ(j["environment"]["features"].size(), size_t{5});
    CHECK(!Json::parse(j.dump(2)).isNull());
}

TEST(Ipc, ProtocolVersionGate) {
    CHECK(isProtocolCompatible(kIpcProtocolVersion));
    CHECK(!isProtocolCompatible(kIpcProtocolVersion + 1));
    CHECK(!isProtocolCompatible(0));
}

TEST(Ipc, GarbageDocumentsYieldSafeDefaults) {
    // A half-written file must never be interpreted as "turn everything on".
    const IpcRequest r = IpcRequest::fromJson(Json::parse("{\"settings\":\"nope\"}"));
    CHECK(!r.settings.enabled);
    CHECK_EQ(r.protocolVersion, 0); // mismatched -> the caller rejects it
    CHECK(!isProtocolCompatible(r.protocolVersion));

    const IpcStatus s = IpcStatus::fromJson(Json{});
    CHECK(!s.bridgeLoaded);
    CHECK(!s.initialised);
}

TEST(Ipc, TruncatedJsonIsRejectedNotPartiallyApplied) {
    IpcRequest full;
    full.settings.enabled = true;
    full.settings.upscaler = UpscalerSelection::Ml;
    const std::string text = full.toJson().dump(-1);
    const std::string truncated = text.substr(0, text.size() / 2);

    std::string err;
    const Json j = Json::parse(truncated, &err);
    CHECK(j.isNull());
    CHECK(!err.empty());
    CHECK(!IpcRequest::fromJson(j).settings.enabled);
}
