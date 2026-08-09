// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/ipc_protocol.hpp"

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
        const std::string idText = e["id"].asString("0");
        v.id = std::strtoull(idText.c_str(), nullptr, 10);
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

} // namespace

bool isProtocolCompatible(int otherVersion) noexcept {
    return otherVersion == kIpcProtocolVersion;
}

// ---------------------------------------------------------------------------

Json IpcRequest::toJson() const {
    Json j = Json::object();
    j.set("protocolVersion", Json(protocolVersion));
    j.set("sequence", Json(std::to_string(sequence)));
    j.set("modVersion", Json(modVersion));
    j.set("beamngVersion", Json(beamngVersion));
    j.set("settings", settings.toJson());

    Json state = Json::object();
    state.set("paused", Json(paused));
    state.set("loading", Json(loading));
    state.set("uiFocused", Json(uiFocused));
    state.set("photoMode", Json(photoMode));
    state.set("replay", Json(replay));
    state.set("cameraCut", Json(cameraCut));
    j.set("gameState", std::move(state));

    Json cam = Json::object();
    cam.set("valid", Json(cameraParamsValid));
    cam.set("near", Json(static_cast<double>(cameraNear)));
    cam.set("far", Json(static_cast<double>(cameraFar)));
    cam.set("fovVerticalRadians", Json(static_cast<double>(cameraFovVerticalRadians)));
    j.set("camera", std::move(cam));

    j.set("depthMvBeacon", beaconToJson(depthMvBeacon));
    j.set("clearSafeMode", Json(clearSafeMode));
    return j;
}

IpcRequest IpcRequest::fromJson(const Json& j) {
    IpcRequest r;
    if (!j.isObject()) return r;

    r.protocolVersion = j["protocolVersion"].asInt(0);
    r.sequence = std::strtoull(j["sequence"].asString("0").c_str(), nullptr, 10);
    r.modVersion = j["modVersion"].asString();
    r.beamngVersion = j["beamngVersion"].asString();
    r.settings = Settings::fromJson(j["settings"]);

    const Json& state = j["gameState"];
    r.paused = state["paused"].asBool(false);
    r.loading = state["loading"].asBool(false);
    r.uiFocused = state["uiFocused"].asBool(false);
    r.photoMode = state["photoMode"].asBool(false);
    r.replay = state["replay"].asBool(false);
    r.cameraCut = state["cameraCut"].asBool(false);

    const Json& cam = j["camera"];
    r.cameraParamsValid = cam["valid"].asBool(false);
    r.cameraNear = static_cast<float>(cam["near"].asNumber(0.1));
    r.cameraFar = static_cast<float>(cam["far"].asNumber(10000.0));
    r.cameraFovVerticalRadians = static_cast<float>(cam["fovVerticalRadians"].asNumber(1.2));

    r.depthMvBeacon = beaconFromJson(j["depthMvBeacon"]);
    r.clearSafeMode = j["clearSafeMode"].asBool(false);
    return r;
}

// ---------------------------------------------------------------------------

Json IpcStatus::toJson() const {
    Json j = Json::object();
    j.set("protocolVersion", Json(protocolVersion));
    j.set("sequence", Json(std::to_string(sequence)));
    j.set("bridgeVersion", Json(bridgeVersion));
    j.set("bridgeLoaded", Json(bridgeLoaded));
    j.set("initialised", Json(initialised));
    j.set("initFailureReason", Json(initFailureReason));

    j.set("environment", describeEnvironment(gpu, system, capabilities));
    j.set("plan", plan.toJson());
    j.set("upscalerVersions", versionsToJson(upscalerVersions));
    j.set("frameGenVersions", versionsToJson(frameGenVersions));

    Json m = Json::object();
    m.set("renderedFps", Json(static_cast<double>(renderedFps)));
    m.set("displayedFps", Json(static_cast<double>(displayedFps)));
    m.set("onePercentLowFps", Json(static_cast<double>(onePercentLowFps)));
    m.set("pointOnePercentLowFps", Json(static_cast<double>(pointOnePercentLowFps)));
    m.set("frameTimeMs", Json(static_cast<double>(frameTimeMs)));
    m.set("upscalerGpuMs", Json(static_cast<double>(upscalerGpuMs)));
    m.set("frameGenGpuMs", Json(static_cast<double>(frameGenGpuMs)));
    m.set("upscalerVramBytes", Json(std::to_string(upscalerVramBytes)));
    m.set("frameGenVramBytes", Json(std::to_string(frameGenVramBytes)));
    m.set("framesRendered", Json(std::to_string(framesRendered)));
    m.set("framesGenerated", Json(std::to_string(framesGenerated)));
    j.set("metrics", std::move(m));

    j.set("frameGenSuppressReason", Json(frameGenSuppressReason));
    j.set("depthSource", Json(depthSource));
    j.set("motionVectorSource", Json(motionVectorSource));
    j.set("safeModeActive", Json(safeModeActive));
    j.set("recentLog", stringsToJson(recentLog));
    return j;
}

IpcStatus IpcStatus::fromJson(const Json& j) {
    IpcStatus s;
    if (!j.isObject()) return s;

    s.protocolVersion = j["protocolVersion"].asInt(0);
    s.sequence = std::strtoull(j["sequence"].asString("0").c_str(), nullptr, 10);
    s.bridgeVersion = j["bridgeVersion"].asString();
    s.bridgeLoaded = j["bridgeLoaded"].asBool(false);
    s.initialised = j["initialised"].asBool(false);
    s.initFailureReason = j["initFailureReason"].asString();

    s.upscalerVersions = versionsFromJson(j["upscalerVersions"]);
    s.frameGenVersions = versionsFromJson(j["frameGenVersions"]);

    const Json& m = j["metrics"];
    s.renderedFps = static_cast<float>(m["renderedFps"].asNumber(0));
    s.displayedFps = static_cast<float>(m["displayedFps"].asNumber(0));
    s.onePercentLowFps = static_cast<float>(m["onePercentLowFps"].asNumber(0));
    s.pointOnePercentLowFps = static_cast<float>(m["pointOnePercentLowFps"].asNumber(0));
    s.frameTimeMs = static_cast<float>(m["frameTimeMs"].asNumber(0));
    s.upscalerGpuMs = static_cast<float>(m["upscalerGpuMs"].asNumber(0));
    s.frameGenGpuMs = static_cast<float>(m["frameGenGpuMs"].asNumber(0));
    s.upscalerVramBytes = std::strtoull(m["upscalerVramBytes"].asString("0").c_str(), nullptr, 10);
    s.frameGenVramBytes = std::strtoull(m["frameGenVramBytes"].asString("0").c_str(), nullptr, 10);
    s.framesRendered = std::strtoull(m["framesRendered"].asString("0").c_str(), nullptr, 10);
    s.framesGenerated = std::strtoull(m["framesGenerated"].asString("0").c_str(), nullptr, 10);

    s.frameGenSuppressReason = j["frameGenSuppressReason"].asString();
    s.depthSource = j["depthSource"].asString();
    s.motionVectorSource = j["motionVectorSource"].asString();
    s.safeModeActive = j["safeModeActive"].asBool(false);
    s.recentLog = stringsFromJson(j["recentLog"]);
    return s;
}

} // namespace bfsr
