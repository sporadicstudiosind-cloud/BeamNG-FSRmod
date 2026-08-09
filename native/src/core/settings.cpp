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

// Readers that ignore wrongly-typed fields rather than coercing them. A
// hand-edited or partially-written file must degrade to defaults for the field
// it broke, never for the whole document and never into a surprising value.
float readFloat(const Json& j, const char* key, float def) {
    const Json& v = j[key];
    return v.isNumber() ? static_cast<float>(v.asNumber(def)) : def;
}

uint32_t readUint(const Json& j, const char* key, uint32_t def) {
    const Json& v = j[key];
    if (!v.isNumber()) return def;
    const double n = v.asNumber(def);
    return n < 0.0 ? 0u : static_cast<uint32_t>(n);
}

bool readBool(const Json& j, const char* key, bool def) {
    const Json& v = j[key];
    return (v.isBool() || v.isNumber()) ? v.asBool(def) : def;
}

std::string readString(const Json& j, const char* key, const std::string& def) {
    const Json& v = j[key];
    return v.isString() ? v.asString(def) : def;
}

} // namespace

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

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

const char* toString(FrameGenSelection s) noexcept {
    switch (s) {
        case FrameGenSelection::Disabled: return "disabled";
        case FrameGenSelection::Auto: return "auto";
        case FrameGenSelection::Analytical: return "analytical";
        case FrameGenSelection::Ml: return "ml";
    }
    return "disabled";
}

FrameGenSelection frameGenSelectionFromString(const std::string& s, FrameGenSelection def) {
    if (s == "disabled" || s == "off") return FrameGenSelection::Disabled;
    if (s == "auto") return FrameGenSelection::Auto;
    if (s == "analytical" || s == "fsr31") return FrameGenSelection::Analytical;
    if (s == "ml" || s == "fsr4") return FrameGenSelection::Ml;
    return def;
}

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

UpscalerSelection upscalerSelectionFromString(const std::string& s, UpscalerSelection def) {
    if (s == "disabled" || s == "off") return UpscalerSelection::Disabled;
    if (s == "auto") return UpscalerSelection::Auto;
    if (s == "analytical" || s == "fsr31") return UpscalerSelection::Analytical;
    if (s == "ml" || s == "fsr4") return UpscalerSelection::Ml;
    return def;
}

// ---------------------------------------------------------------------------
// RenderCalibration
// ---------------------------------------------------------------------------

Json RenderCalibration::toJson() const {
    Json j = Json::object();
    j.set("depthConvention", Json(std::string(toString(depthConvention))));
    j.set("depthInfinite", Json(depthInfinite));
    j.set("motionVectorScaleX", Json(static_cast<double>(motionVectorScaleX)));
    j.set("motionVectorScaleY", Json(static_cast<double>(motionVectorScaleY)));
    j.set("motionVectorFlipY", Json(motionVectorFlipY));
    j.set("motionVectorsAreUvSpace", Json(motionVectorsAreUvSpace));
    j.set("beaconResourceState", Json(beaconResourceState));
    return j;
}

RenderCalibration RenderCalibration::fromJson(const Json& j) {
    RenderCalibration c;
    if (!j.isObject()) return c;
    c.depthConvention = depthConventionFromString(
        readString(j, "depthConvention", toString(c.depthConvention)), c.depthConvention);
    c.depthInfinite = readBool(j, "depthInfinite", c.depthInfinite);
    c.motionVectorScaleX = readFloat(j, "motionVectorScaleX", c.motionVectorScaleX);
    c.motionVectorScaleY = readFloat(j, "motionVectorScaleY", c.motionVectorScaleY);
    c.motionVectorFlipY = readBool(j, "motionVectorFlipY", c.motionVectorFlipY);
    c.motionVectorsAreUvSpace = readBool(j, "motionVectorsAreUvSpace", c.motionVectorsAreUvSpace);
    c.beaconResourceState = readString(j, "beaconResourceState", c.beaconResourceState);
    c.sanitise();
    return c;
}

void RenderCalibration::sanitise() {
    motionVectorScaleX = clampValue(motionVectorScaleX, -8192.0f, 8192.0f);
    motionVectorScaleY = clampValue(motionVectorScaleY, -8192.0f, 8192.0f);
    // An unrecognised state would be silently coerced by the translation in
    // bridge_runtime.cpp; rejecting it here instead means a typo in the config
    // file surfaces as "reverted to the default" rather than as wrong barriers.
    if (beaconResourceState != "pixelComputeRead" && beaconResourceState != "pixelRead" &&
        beaconResourceState != "computeRead" && beaconResourceState != "renderTarget" &&
        beaconResourceState != "genericRead" && beaconResourceState != "common") {
        beaconResourceState = "pixelComputeRead";
    }
}

bool RenderCalibration::operator==(const RenderCalibration& o) const {
    return depthConvention == o.depthConvention && depthInfinite == o.depthInfinite &&
           motionVectorScaleX == o.motionVectorScaleX &&
           motionVectorScaleY == o.motionVectorScaleY &&
           motionVectorFlipY == o.motionVectorFlipY &&
           motionVectorsAreUvSpace == o.motionVectorsAreUvSpace &&
           beaconResourceState == o.beaconResourceState;
}

// ---------------------------------------------------------------------------
// DiagnosticsSettings
// ---------------------------------------------------------------------------

Json DiagnosticsSettings::toJson() const {
    Json j = Json::object();
    j.set("overlay", Json(overlay));
    j.set("debugView", Json(debugView));
    j.set("benchmarkMode", Json(benchmarkMode));
    j.set("logLevel", Json(logLevel));
    j.set("forceCompatibilityMode", Json(forceCompatibilityMode));
    return j;
}

DiagnosticsSettings DiagnosticsSettings::fromJson(const Json& j) {
    DiagnosticsSettings d;
    if (!j.isObject()) return d;
    d.overlay = readBool(j, "overlay", d.overlay);
    d.debugView = readBool(j, "debugView", d.debugView);
    d.benchmarkMode = readBool(j, "benchmarkMode", d.benchmarkMode);
    d.logLevel = readString(j, "logLevel", d.logLevel);
    d.forceCompatibilityMode = readBool(j, "forceCompatibilityMode", d.forceCompatibilityMode);
    d.sanitise();
    return d;
}

void DiagnosticsSettings::sanitise() {
    if (logLevel.empty()) logLevel = "info";
}

bool DiagnosticsSettings::operator==(const DiagnosticsSettings& o) const {
    return overlay == o.overlay && debugView == o.debugView &&
           benchmarkMode == o.benchmarkMode && logLevel == o.logLevel &&
           forceCompatibilityMode == o.forceCompatibilityMode;
}

// ---------------------------------------------------------------------------
// FrameGenSettings
// ---------------------------------------------------------------------------

Json FrameGenSettings::toJson() const {
    Json j = Json::object();
    j.set("schemaVersion", Json(1));
    j.set("module", Json("framegen"));
    j.set("enabled", Json(enabled));
    j.set("safeMode", Json(safeMode));
    j.set("implementation", Json(std::string(toString(implementation))));
    j.set("versionId", Json(versionId));
    j.set("asyncCompute", Json(asyncCompute));
    j.set("uiComposition", Json(uiComposition));
    j.set("antiLag2", Json(antiLag2));
    j.set("antiLagMaxFps", Json(static_cast<unsigned long long>(antiLagMaxFps)));
    j.set("calibration", calibration.toJson());
    j.set("diagnostics", diagnostics.toJson());
    return j;
}

FrameGenSettings FrameGenSettings::fromJson(const Json& j) {
    FrameGenSettings s;
    if (!j.isObject()) return s;

    s.enabled = readBool(j, "enabled", s.enabled);
    s.safeMode = readBool(j, "safeMode", s.safeMode);
    s.implementation = frameGenSelectionFromString(
        readString(j, "implementation", toString(s.implementation)), s.implementation);
    s.versionId = readString(j, "versionId", s.versionId);
    s.asyncCompute = readBool(j, "asyncCompute", s.asyncCompute);
    s.uiComposition = readBool(j, "uiComposition", s.uiComposition);
    s.antiLag2 = readBool(j, "antiLag2", s.antiLag2);
    s.antiLagMaxFps = readUint(j, "antiLagMaxFps", s.antiLagMaxFps);
    s.calibration = RenderCalibration::fromJson(j["calibration"]);
    s.diagnostics = DiagnosticsSettings::fromJson(j["diagnostics"]);
    s.sanitise();
    return s;
}

void FrameGenSettings::sanitise() {
    calibration.sanitise();
    diagnostics.sanitise();

    // A frame cap below this is not a latency tool, it is a broken setting.
    if (antiLagMaxFps != 0) antiLagMaxFps = clampValue<uint32_t>(antiLagMaxFps, 20u, 1000u);

    // Safe mode switches the product off, and that is all it does to the
    // document. It deliberately does not clear the implementation choice, the
    // version pin or the Anti-Lag preference: the planner already refuses to
    // act while safe mode is set, so overwriting them would only destroy the
    // user's configuration and make clearing safe mode a re-setup exercise.
    if (safeMode) enabled = false;

    // Compatibility mode is likewise honoured by the planner rather than baked
    // into the document, so turning it off restores the pinned version and
    // async compute instead of silently having lost them.

    if (versionId.empty()) versionId = "auto";
}

bool FrameGenSettings::operator==(const FrameGenSettings& o) const {
    return enabled == o.enabled && safeMode == o.safeMode &&
           implementation == o.implementation && versionId == o.versionId &&
           asyncCompute == o.asyncCompute && uiComposition == o.uiComposition &&
           antiLag2 == o.antiLag2 && antiLagMaxFps == o.antiLagMaxFps &&
           calibration == o.calibration && diagnostics == o.diagnostics;
}

// ---------------------------------------------------------------------------
// UpscaleSettings
// ---------------------------------------------------------------------------

Json UpscaleSettings::toJson() const {
    Json j = Json::object();
    j.set("schemaVersion", Json(1));
    j.set("module", Json("upscaling"));
    j.set("enabled", Json(enabled));
    j.set("safeMode", Json(safeMode));
    j.set("implementation", Json(std::string(toString(implementation))));
    j.set("versionId", Json(versionId));
    j.set("quality", Json(std::string(toString(quality))));
    j.set("customUpscaleRatio", Json(static_cast<double>(customUpscaleRatio)));
    j.set("sharpeningEnabled", Json(sharpeningEnabled));
    j.set("sharpness", Json(static_cast<double>(sharpness)));
    j.set("calibration", calibration.toJson());
    j.set("diagnostics", diagnostics.toJson());
    return j;
}

UpscaleSettings UpscaleSettings::fromJson(const Json& j) {
    UpscaleSettings s;
    if (!j.isObject()) return s;

    s.enabled = readBool(j, "enabled", s.enabled);
    s.safeMode = readBool(j, "safeMode", s.safeMode);
    s.implementation = upscalerSelectionFromString(
        readString(j, "implementation", toString(s.implementation)), s.implementation);
    s.versionId = readString(j, "versionId", s.versionId);
    s.quality = qualityModeFromString(readString(j, "quality", toString(s.quality)), s.quality);
    s.customUpscaleRatio = readFloat(j, "customUpscaleRatio", s.customUpscaleRatio);
    s.sharpeningEnabled = readBool(j, "sharpeningEnabled", s.sharpeningEnabled);
    s.sharpness = readFloat(j, "sharpness", s.sharpness);
    s.calibration = RenderCalibration::fromJson(j["calibration"]);
    s.diagnostics = DiagnosticsSettings::fromJson(j["diagnostics"]);
    s.sanitise();
    return s;
}

void UpscaleSettings::sanitise() {
    calibration.sanitise();
    diagnostics.sanitise();

    sharpness = clampValue(sharpness, 0.0f, 1.0f);
    // 1.0 is native, 3.0 matches AMD's ultra-performance ratio.
    customUpscaleRatio = clampValue(customUpscaleRatio, 1.0f, 3.0f);

    // Same rule as frame generation: safe mode switches the product off without
    // discarding the settings behind it.
    if (safeMode) enabled = false;

    if (versionId.empty()) versionId = "auto";
}

bool UpscaleSettings::operator==(const UpscaleSettings& o) const {
    return enabled == o.enabled && safeMode == o.safeMode &&
           implementation == o.implementation && versionId == o.versionId &&
           quality == o.quality && customUpscaleRatio == o.customUpscaleRatio &&
           sharpeningEnabled == o.sharpeningEnabled && sharpness == o.sharpness &&
           calibration == o.calibration && diagnostics == o.diagnostics;
}

// ---------------------------------------------------------------------------
// SettingsStore
// ---------------------------------------------------------------------------

template <typename T>
T SettingsStore<T>::load(std::string* diagnostic) const {
    if (diagnostic) diagnostic->clear();

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        if (diagnostic) *diagnostic = "no settings file at '" + path_ + "'; using defaults";
        return T{};
    }

    std::ostringstream ss;
    ss << in.rdbuf();

    std::string parseError;
    const Json j = Json::parse(ss.str(), &parseError);
    if (j.isNull()) {
        if (diagnostic)
            *diagnostic = "settings file is not valid JSON (" + parseError + "); using defaults";
        return T{};
    }
    return T::fromJson(j);
}

template <typename T>
bool SettingsStore<T>::save(const T& settings, std::string* diagnostic) const {
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

template class SettingsStore<FrameGenSettings>;
template class SettingsStore<UpscaleSettings>;

} // namespace bfsr
