// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/ipc_protocol.hpp"

#include <cstdlib>

namespace bfsr {
namespace {

Json beaconToJson(const BeaconSignature& b) {
    Json j = Json::object();
    j.set("valid", Json(b.valid));
    j.set("width", Json(static_cast<unsigned long long>(b.width)));
    j.set("height", Json(static_cast<unsigned long long>(b.height)));
    j.set("format", Json(static_cast<unsigned long long>(b.format)));
    j.set("expectRenderTarget", Json(b.expectRenderTarget));
    j.set("expectUnorderedAccess", Json(b.expectUnorderedAccess));
    j.set("label", Json(b.label));
    j.set("ordinal", Json(static_cast<unsigned long long>(b.ordinal)));
    j.set("expectedCount", Json(static_cast<unsigned long long>(b.expectedCount)));
    return j;
}

BeaconSignature beaconFromJson(const Json& j) {
    BeaconSignature b;
    b.valid = j["valid"].asBool(false);
    b.width = static_cast<uint32_t>(j["width"].asNumber(0));
    b.height = static_cast<uint32_t>(j["height"].asNumber(0));
    b.format = static_cast<uint32_t>(j["format"].asNumber(0));
    b.expectRenderTarget = j["expectRenderTarget"].asBool(true);
    b.expectUnorderedAccess = j["expectUnorderedAccess"].asBool(false);
    b.label = j["label"].asString();
    b.ordinal = static_cast<uint32_t>(j["ordinal"].asNumber(0));
    b.expectedCount = static_cast<uint32_t>(j["expectedCount"].asNumber(1));
    return b;
}

Json versionsToJson(const std::vector<FfxVersionEntry>& versions) {
    Json arr = Json::array();
    for (const auto& v : versions) {
        Json e = Json::object();
        // Ids are 64-bit and JSON numbers are doubles, so they travel as strings
        // to avoid silent precision loss on a value AMD treats as opaque.
        e.set("id", Json(std::to_string(v.id)));
        e.set("name", Json(v.name));
        e.set("ml", Json(v.isMachineLearning));
        arr.push(std::move(e));
    }
    return arr;
}

std::vector<FfxVersionEntry> versionsFromJson(const Json& arr) {
    std::vector<FfxVersionEntry> out;
    for (size_t i = 0; i < arr.size(); ++i) {
        const Json& e = arr.at(i);
        FfxVersionEntry v;
        v.id = std::strtoull(e["id"].asString("0").c_str(), nullptr, 10);
        v.name = e["name"].asString();
        v.isMachineLearning = e["ml"].asBool(false);
        out.push_back(std::move(v));
    }
    return out;
}

Json stringsToJson(const std::vector<std::string>& v) {
    Json arr = Json::array();
    for (const auto& s : v) arr.push(Json(s));
    return arr;
}

std::vector<std::string> stringsFromJson(const Json& arr) {
    std::vector<std::string> out;
    for (size_t i = 0; i < arr.size(); ++i) out.push_back(arr.at(i).asString());
    return out;
}

uint64_t readU64(const Json& j, const char* key) {
    return std::strtoull(j[key].asString("0").c_str(), nullptr, 10);
}

} // namespace

bool isProtocolCompatible(int otherVersion) noexcept {
    return otherVersion == kIpcProtocolVersion;
}

// ---------------------------------------------------------------------------

Json SharedFrameState::toJson() const {
    Json state = Json::object();
    state.set("paused", Json(paused));
    state.set("loading", Json(loading));
    state.set("uiFocused", Json(uiFocused));
    state.set("photoMode", Json(photoMode));
    state.set("replay", Json(replay));
    state.set("cameraCut", Json(cameraCut));

    Json cam = Json::object();
    cam.set("valid", Json(cameraValid));
    cam.set("near", Json(static_cast<double>(cameraNear)));
    cam.set("far", Json(static_cast<double>(cameraFar)));
    cam.set("fovVerticalRadians", Json(static_cast<double>(cameraFovVerticalRadians)));

    Json j = Json::object();
    j.set("gameState", std::move(state));
    j.set("camera", std::move(cam));
    j.set("depthBeacon", beaconToJson(depthBeacon));
    j.set("motionBeacon", beaconToJson(motionBeacon));
    return j;
}

SharedFrameState SharedFrameState::fromJson(const Json& j) {
    SharedFrameState s;
    if (!j.isObject()) return s;

    const Json& state = j["gameState"];
    s.paused = state["paused"].asBool(false);
    s.loading = state["loading"].asBool(false);
    s.uiFocused = state["uiFocused"].asBool(false);
    s.photoMode = state["photoMode"].asBool(false);
    s.replay = state["replay"].asBool(false);
    s.cameraCut = state["cameraCut"].asBool(false);

    const Json& cam = j["camera"];
    s.cameraValid = cam["valid"].asBool(false);
    s.cameraNear = static_cast<float>(cam["near"].asNumber(0.1));
    s.cameraFar = static_cast<float>(cam["far"].asNumber(10000.0));
    s.cameraFovVerticalRadians = static_cast<float>(cam["fovVerticalRadians"].asNumber(1.2));

    s.depthBeacon = beaconFromJson(j["depthBeacon"]);
    s.motionBeacon = beaconFromJson(j["motionBeacon"]);
    return s;
}

// ---------------------------------------------------------------------------

Json FrameGenRequest::toJson() const {
    Json j = Json::object();
    j.set("protocolVersion", Json(protocolVersion));
    j.set("module", Json("framegen"));
    j.set("sequence", Json(std::to_string(sequence)));
    j.set("modVersion", Json(modVersion));
    j.set("beamngVersion", Json(beamngVersion));
    j.set("settings", settings.toJson());
    j.set("shared", shared.toJson());
    j.set("clearSafeMode", Json(clearSafeMode));
    return j;
}

FrameGenRequest FrameGenRequest::fromJson(const Json& j) {
    FrameGenRequest r;
    if (!j.isObject()) { r.protocolVersion = 0; return r; }
    r.protocolVersion = j["protocolVersion"].asInt(0);
    r.sequence = readU64(j, "sequence");
    r.modVersion = j["modVersion"].asString();
    r.beamngVersion = j["beamngVersion"].asString();
    r.settings = FrameGenSettings::fromJson(j["settings"]);
    r.shared = SharedFrameState::fromJson(j["shared"]);
    r.clearSafeMode = j["clearSafeMode"].asBool(false);
    return r;
}

Json UpscaleRequest::toJson() const {
    Json j = Json::object();
    j.set("protocolVersion", Json(protocolVersion));
    j.set("module", Json("upscaling"));
    j.set("sequence", Json(std::to_string(sequence)));
    j.set("modVersion", Json(modVersion));
    j.set("beamngVersion", Json(beamngVersion));
    j.set("settings", settings.toJson());
    j.set("shared", shared.toJson());
    j.set("clearSafeMode", Json(clearSafeMode));
    return j;
}

UpscaleRequest UpscaleRequest::fromJson(const Json& j) {
    UpscaleRequest r;
    if (!j.isObject()) { r.protocolVersion = 0; return r; }
    r.protocolVersion = j["protocolVersion"].asInt(0);
    r.sequence = readU64(j, "sequence");
    r.modVersion = j["modVersion"].asString();
    r.beamngVersion = j["beamngVersion"].asString();
    r.settings = UpscaleSettings::fromJson(j["settings"]);
    r.shared = SharedFrameState::fromJson(j["shared"]);
    r.clearSafeMode = j["clearSafeMode"].asBool(false);
    return r;
}

// ---------------------------------------------------------------------------

Json StatusCommon::toJson() const {
    Json j = Json::object();
    j.set("bridgeVersion", Json(bridgeVersion));
    j.set("bridgeLoaded", Json(bridgeLoaded));
    j.set("initialised", Json(initialised));
    j.set("initFailureReason", Json(initFailureReason));
    j.set("environment", describeEnvironment(gpu, system, capabilities));
    j.set("installedModules", installedModules);

    Json m = Json::object();
    m.set("renderedFps", Json(static_cast<double>(renderedFps)));
    m.set("displayedFps", Json(static_cast<double>(displayedFps)));
    m.set("onePercentLowFps", Json(static_cast<double>(onePercentLowFps)));
    m.set("pointOnePercentLowFps", Json(static_cast<double>(pointOnePercentLowFps)));
    m.set("frameTimeMs", Json(static_cast<double>(frameTimeMs)));
    m.set("framesRendered", Json(std::to_string(framesRendered)));
    j.set("metrics", std::move(m));

    j.set("depthSource", Json(depthSource));
    j.set("motionVectorSource", Json(motionVectorSource));
    j.set("recentLog", stringsToJson(recentLog));
    return j;
}

StatusCommon StatusCommon::fromJson(const Json& j) {
    StatusCommon s;
    if (!j.isObject()) return s;
    s.bridgeVersion = j["bridgeVersion"].asString();
    s.bridgeLoaded = j["bridgeLoaded"].asBool(false);
    s.initialised = j["initialised"].asBool(false);
    s.initFailureReason = j["initFailureReason"].asString();
    s.installedModules = j["installedModules"];

    const Json& m = j["metrics"];
    s.renderedFps = static_cast<float>(m["renderedFps"].asNumber(0));
    s.displayedFps = static_cast<float>(m["displayedFps"].asNumber(0));
    s.onePercentLowFps = static_cast<float>(m["onePercentLowFps"].asNumber(0));
    s.pointOnePercentLowFps = static_cast<float>(m["pointOnePercentLowFps"].asNumber(0));
    s.frameTimeMs = static_cast<float>(m["frameTimeMs"].asNumber(0));
    s.framesRendered = readU64(m, "framesRendered");

    s.depthSource = j["depthSource"].asString();
    s.motionVectorSource = j["motionVectorSource"].asString();
    s.recentLog = stringsFromJson(j["recentLog"]);
    return s;
}

// ---------------------------------------------------------------------------

Json FrameGenStatus::toJson() const {
    Json j = Json::object();
    j.set("protocolVersion", Json(protocolVersion));
    j.set("module", Json("framegen"));
    j.set("sequence", Json(std::to_string(sequence)));
    j.set("moduleInstalled", Json(moduleInstalled));
    j.set("common", common.toJson());
    j.set("plan", plan.toJson());
    j.set("versions", versionsToJson(versions));
    j.set("gpuMs", Json(static_cast<double>(gpuMs)));
    j.set("vramBytes", Json(std::to_string(vramBytes)));
    j.set("framesGenerated", Json(std::to_string(framesGenerated)));
    j.set("observedGeneratedPerFrame",
          Json(static_cast<unsigned long long>(observedGeneratedPerFrame)));
    j.set("suppressReason", Json(suppressReason));
    j.set("antiLagActive", Json(antiLagActive));
    j.set("safeModeActive", Json(safeModeActive));
    return j;
}

FrameGenStatus FrameGenStatus::fromJson(const Json& j) {
    FrameGenStatus s;
    if (!j.isObject()) { s.protocolVersion = 0; return s; }
    s.protocolVersion = j["protocolVersion"].asInt(0);
    s.sequence = readU64(j, "sequence");
    s.moduleInstalled = j["moduleInstalled"].asBool(false);
    s.common = StatusCommon::fromJson(j["common"]);
    s.versions = versionsFromJson(j["versions"]);
    s.gpuMs = static_cast<float>(j["gpuMs"].asNumber(0));
    s.vramBytes = readU64(j, "vramBytes");
    s.framesGenerated = readU64(j, "framesGenerated");
    s.observedGeneratedPerFrame =
        static_cast<uint32_t>(j["observedGeneratedPerFrame"].asNumber(0));
    s.suppressReason = j["suppressReason"].asString();
    s.antiLagActive = j["antiLagActive"].asBool(false);
    s.safeModeActive = j["safeModeActive"].asBool(false);
    return s;
}

Json UpscaleStatus::toJson() const {
    Json j = Json::object();
    j.set("protocolVersion", Json(protocolVersion));
    j.set("module", Json("upscaling"));
    j.set("sequence", Json(std::to_string(sequence)));
    j.set("moduleInstalled", Json(moduleInstalled));
    j.set("common", common.toJson());
    j.set("plan", plan.toJson());
    j.set("versions", versionsToJson(versions));
    j.set("gpuMs", Json(static_cast<double>(gpuMs)));
    j.set("vramBytes", Json(std::to_string(vramBytes)));
    j.set("jitterAvailable", Json(jitterAvailable));
    j.set("safeModeActive", Json(safeModeActive));
    return j;
}

UpscaleStatus UpscaleStatus::fromJson(const Json& j) {
    UpscaleStatus s;
    if (!j.isObject()) { s.protocolVersion = 0; return s; }
    s.protocolVersion = j["protocolVersion"].asInt(0);
    s.sequence = readU64(j, "sequence");
    s.moduleInstalled = j["moduleInstalled"].asBool(false);
    s.common = StatusCommon::fromJson(j["common"]);
    s.versions = versionsFromJson(j["versions"]);
    s.gpuMs = static_cast<float>(j["gpuMs"].asNumber(0));
    s.vramBytes = readU64(j, "vramBytes");
    s.jitterAvailable = j["jitterAvailable"].asBool(false);
    s.safeModeActive = j["safeModeActive"].asBool(false);
    return s;
}

} // namespace bfsr
