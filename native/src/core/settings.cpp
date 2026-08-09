// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/settings.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace bfsr {
namespace {

template <typename T>
T clampValue(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float readFloat(const Json& j, const char* key, float def) {
    const Json& v = j[key];
    return v.isNumber() ? static_cast<float>(v.asNumber(def)) : def;
}

bool readBool(const Json& j, const char* key, bool def) {
    const Json& v = j[key];
    return v.isBool() || v.isNumber() ? v.asBool(def) : def;
}

std::string readString(const Json& j, const char* key, const std::string& def) {
    const Json& v = j[key];
    return v.isString() ? v.asString(def) : def;
}

} // namespace

// ---------------------------------------------------------------------------

const char* toString(QualityMode m) noexcept {
    switch (m) {
        case QualityMode::NativeAA: return "nativeAA";
        case QualityMode::Quality: return "quality";
        case QualityMode::Balanced: return "balanced";
        case QualityMode::Performance: return "performance";
        case QualityMode::UltraPerformance: return "ultraPerformance";
        case QualityMode::Custom: return "custom";
    }
    return "quality";
}

QualityMode qualityModeFromString(const std::string& s, QualityMode def) {
    if (s == "nativeAA" || s == "native") return QualityMode::NativeAA;
    if (s == "quality") return QualityMode::Quality;
    if (s == "balanced") return QualityMode::Balanced;
    if (s == "performance") return QualityMode::Performance;
    if (s == "ultraPerformance") return QualityMode::UltraPerformance;
    if (s == "custom") return QualityMode::Custom;
    return def;
}

float qualityModeRatio(QualityMode m) noexcept {
    switch (m) {
        case QualityMode::NativeAA: return 1.0f;
        case QualityMode::Quality: return 1.5f;
        case QualityMode::Balanced: return 1.7f;
        case QualityMode::Performance: return 2.0f;
        case QualityMode::UltraPerformance: return 3.0f;
        case QualityMode::Custom: return 0.0f;
    }
    return 1.5f;
}

const char* toString(UpscalerSelection s) noexcept {
    switch (s) {
        case UpscalerSelection::Disabled: return "disabled";
        case UpscalerSelection::Auto: return "auto";
        case UpscalerSelection::Analytical: return "analytical";
        case UpscalerSelection::Ml: return "ml";
    }
    return "disabled";
}

const char* toString(FrameGenSelection s) noexcept {
    switch (s) {
        case FrameGenSelection::Disabled: return "disabled";
        case FrameGenSelection::Auto: return "auto";
        case FrameGenSelection::Analytical: return "analytical";
        case FrameGenSelection::Ml: return "ml";
    }
    return "disabled";
}

UpscalerSelection upscalerSelectionFromString(const std::string& s, UpscalerSelection def) {
    if (s == "disabled" || s == "off") return UpscalerSelection::Disabled;
    if (s == "auto") return UpscalerSelection::Auto;
    if (s == "analytical" || s == "fsr31") return UpscalerSelection::Analytical;
    if (s == "ml" || s == "fsr4") return UpscalerSelection::Ml;
    return def;
}

FrameGenSelection frameGenSelectionFromString(const std::string& s, FrameGenSelection def) {
    if (s == "disabled" || s == "off") return FrameGenSelection::Disabled;
    if (s == "auto") return FrameGenSelection::Auto;
    if (s == "analytical" || s == "fsr31") return FrameGenSelection::Analytical;
    if (s == "ml" || s == "fsr4") return FrameGenSelection::Ml;
    return def;
}

const char* toString(DepthConvention c) noexcept {
    switch (c) {
        case DepthConvention::Auto: return "auto";
        case DepthConvention::ReversedZ: return "reversedZ";
        case DepthConvention::StandardZ: return "standardZ";
    }
    return "auto";
}

DepthConvention depthConventionFromString(const std::string& s, DepthConvention def) {
    if (s == "auto") return DepthConvention::Auto;
    if (s == "reversedZ" || s == "reversed") return DepthConvention::ReversedZ;
    if (s == "standardZ" || s == "standard") return DepthConvention::StandardZ;
    return def;
}

// ---------------------------------------------------------------------------

Json Settings::toJson() const {
    Json j = Json::object();
    j.set("schemaVersion", Json(1));
    j.set("enabled", Json(enabled));
    j.set("safeMode", Json(safeMode));

    j.set("upscaler", Json(std::string(toString(upscaler))));
    j.set("upscalerVersionId", Json(upscalerVersionId));
    j.set("quality", Json(std::string(toString(quality))));
    j.set("customUpscaleRatio", Json(static_cast<double>(customUpscaleRatio)));
    j.set("sharpeningEnabled", Json(sharpeningEnabled));
    j.set("sharpness", Json(static_cast<double>(sharpness)));

    j.set("frameGeneration", Json(std::string(toString(frameGeneration))));
    j.set("frameGenVersionId", Json(frameGenVersionId));
    j.set("frameGenAsyncCompute", Json(frameGenAsyncCompute));
    j.set("frameGenUiComposition", Json(frameGenUiComposition));

    j.set("antiLag2", Json(antiLag2));

    j.set("depthConvention", Json(std::string(toString(depthConvention))));
    j.set("depthInfinite", Json(depthInfinite));
    j.set("motionVectorScaleX", Json(static_cast<double>(motionVectorScaleX)));
    j.set("motionVectorScaleY", Json(static_cast<double>(motionVectorScaleY)));
    j.set("motionVectorFlipY", Json(motionVectorFlipY));
    j.set("motionVectorsAreUvSpace", Json(motionVectorsAreUvSpace));

    j.set("debugOverlay", Json(debugOverlay));
    j.set("debugView", Json(debugView));
    j.set("benchmarkMode", Json(benchmarkMode));
    j.set("logLevel", Json(logLevel));
    j.set("forceCompatibilityMode", Json(forceCompatibilityMode));
    return j;
}

Settings Settings::fromJson(const Json& j) {
    Settings s;
    if (!j.isObject()) return s;

    s.enabled = readBool(j, "enabled", s.enabled);
    s.safeMode = readBool(j, "safeMode", s.safeMode);

    s.upscaler = upscalerSelectionFromString(readString(j, "upscaler", toString(s.upscaler)), s.upscaler);
    s.upscalerVersionId = readString(j, "upscalerVersionId", s.upscalerVersionId);
    s.quality = qualityModeFromString(readString(j, "quality", toString(s.quality)), s.quality);
    s.customUpscaleRatio = readFloat(j, "customUpscaleRatio", s.customUpscaleRatio);
    s.sharpeningEnabled = readBool(j, "sharpeningEnabled", s.sharpeningEnabled);
    s.sharpness = readFloat(j, "sharpness", s.sharpness);

    s.frameGeneration = frameGenSelectionFromString(
        readString(j, "frameGeneration", toString(s.frameGeneration)), s.frameGeneration);
    s.frameGenVersionId = readString(j, "frameGenVersionId", s.frameGenVersionId);
    s.frameGenAsyncCompute = readBool(j, "frameGenAsyncCompute", s.frameGenAsyncCompute);
    s.frameGenUiComposition = readBool(j, "frameGenUiComposition", s.frameGenUiComposition);

    s.antiLag2 = readBool(j, "antiLag2", s.antiLag2);

    s.depthConvention = depthConventionFromString(
        readString(j, "depthConvention", toString(s.depthConvention)), s.depthConvention);
    s.depthInfinite = readBool(j, "depthInfinite", s.depthInfinite);
    s.motionVectorScaleX = readFloat(j, "motionVectorScaleX", s.motionVectorScaleX);
    s.motionVectorScaleY = readFloat(j, "motionVectorScaleY", s.motionVectorScaleY);
    s.motionVectorFlipY = readBool(j, "motionVectorFlipY", s.motionVectorFlipY);
    s.motionVectorsAreUvSpace = readBool(j, "motionVectorsAreUvSpace", s.motionVectorsAreUvSpace);

    s.debugOverlay = readBool(j, "debugOverlay", s.debugOverlay);
    s.debugView = readBool(j, "debugView", s.debugView);
    s.benchmarkMode = readBool(j, "benchmarkMode", s.benchmarkMode);
    s.logLevel = readString(j, "logLevel", s.logLevel);
    s.forceCompatibilityMode = readBool(j, "forceCompatibilityMode", s.forceCompatibilityMode);

    s.sanitise();
    return s;
}

void Settings::sanitise() {
    sharpness = clampValue(sharpness, 0.0f, 1.0f);
    // 1.0 = native, 3.0 = FSR's ultra-performance ratio. Expressed as a scale
    // factor here (render = display / scale), so the legal range is [1, 3].
    customUpscaleRatio = clampValue(customUpscaleRatio, 1.0f, 3.0f);
    motionVectorScaleX = clampValue(motionVectorScaleX, -8192.0f, 8192.0f);
    motionVectorScaleY = clampValue(motionVectorScaleY, -8192.0f, 8192.0f);

    // Safe mode is a hard override: it exists because the previous session did
    // not survive, so nothing else in the file gets a vote.
    if (safeMode) {
        enabled = false;
        upscaler = UpscalerSelection::Disabled;
        frameGeneration = FrameGenSelection::Disabled;
        antiLag2 = false;
    }

    if (!enabled) {
        upscaler = UpscalerSelection::Disabled;
        frameGeneration = FrameGenSelection::Disabled;
    }

    // Compatibility mode trades performance for the widest chance of working.
    if (forceCompatibilityMode) {
        frameGenAsyncCompute = false;
        upscalerVersionId = "auto";
        frameGenVersionId = "auto";
    }

    if (upscalerVersionId.empty()) upscalerVersionId = "auto";
    if (frameGenVersionId.empty()) frameGenVersionId = "auto";
    if (logLevel.empty()) logLevel = "info";
}

bool Settings::operator==(const Settings& o) const {
    return enabled == o.enabled && safeMode == o.safeMode &&
           upscaler == o.upscaler && upscalerVersionId == o.upscalerVersionId &&
           quality == o.quality && customUpscaleRatio == o.customUpscaleRatio &&
           sharpeningEnabled == o.sharpeningEnabled && sharpness == o.sharpness &&
           frameGeneration == o.frameGeneration && frameGenVersionId == o.frameGenVersionId &&
           frameGenAsyncCompute == o.frameGenAsyncCompute &&
           frameGenUiComposition == o.frameGenUiComposition &&
           antiLag2 == o.antiLag2 && depthConvention == o.depthConvention &&
           depthInfinite == o.depthInfinite &&
           motionVectorScaleX == o.motionVectorScaleX &&
           motionVectorScaleY == o.motionVectorScaleY &&
           motionVectorFlipY == o.motionVectorFlipY &&
           motionVectorsAreUvSpace == o.motionVectorsAreUvSpace &&
           debugOverlay == o.debugOverlay && debugView == o.debugView &&
           benchmarkMode == o.benchmarkMode && logLevel == o.logLevel &&
           forceCompatibilityMode == o.forceCompatibilityMode;
}

// ---------------------------------------------------------------------------

SettingsStore::SettingsStore(std::string path) : path_(std::move(path)) {}

Settings SettingsStore::load(std::string* diagnostic) const {
    if (diagnostic) diagnostic->clear();

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        if (diagnostic) *diagnostic = "no settings file at '" + path_ + "'; using defaults";
        return Settings{};
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    std::string parseError;
    const Json j = Json::parse(text, &parseError);
    if (j.isNull()) {
        if (diagnostic)
            *diagnostic = "settings file is not valid JSON (" + parseError + "); using defaults";
        return Settings{};
    }

    return Settings::fromJson(j);
}

bool SettingsStore::save(const Settings& settings, std::string* diagnostic) const {
    if (diagnostic) diagnostic->clear();

    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (diagnostic) *diagnostic = "cannot open '" + tmp + "' for writing";
            return false;
        }
        out << settings.toJson().dump(2) << "\n";
        if (!out) {
            if (diagnostic) *diagnostic = "write to '" + tmp + "' failed";
            return false;
        }
    }

    // std::rename will not overwrite on Windows, so clear the target first. The
    // brief window where neither file exists is why load() tolerates a missing
    // file by returning defaults rather than failing.
    std::remove(path_.c_str());
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        if (diagnostic) *diagnostic = "could not replace '" + path_ + "'";
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace bfsr
