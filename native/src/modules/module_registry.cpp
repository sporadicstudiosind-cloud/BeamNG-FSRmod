// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/module_registry.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace bfsr {

const char* toString(ModuleId id) noexcept {
    switch (id) {
        case ModuleId::FrameGeneration: return "framegen";
        case ModuleId::Upscaling: return "upscaling";
    }
    return "unknown";
}

const char* displayName(ModuleId id) noexcept {
    switch (id) {
        case ModuleId::FrameGeneration: return "AMD FSR Frame Generation";
        case ModuleId::Upscaling: return "AMD FSR Upscaling";
    }
    return "Unknown module";
}

std::string moduleManifestFileName(ModuleId id) {
    return std::string(toString(id)) + ".module.json";
}

bool parseModuleManifest(const Json& document, ModuleManifest& out) {
    if (!document.isObject()) return false;

    const std::string module = document["module"].asString();
    if (module == "framegen") out.id = ModuleId::FrameGeneration;
    else if (module == "upscaling") out.id = ModuleId::Upscaling;
    else return false;

    out.present = true;
    out.version = document["version"].asString("unknown");
    return true;
}

void ModuleRegistry::scan(const std::string& modulesDirectory) {
    modules_.clear();
    if (modulesDirectory.empty()) return;

    // Only the two known names are probed rather than enumerating the
    // directory: an unknown manifest cannot enable anything, so discovering it
    // would only produce a log line, and probing keeps this free of platform
    // directory-iteration differences.
    for (const ModuleId id : {ModuleId::FrameGeneration, ModuleId::Upscaling}) {
        const std::string path = modulesDirectory + moduleManifestFileName(id);

        std::ifstream in(path, std::ios::binary);
        if (!in) continue;

        std::ostringstream ss;
        ss << in.rdbuf();
        const Json document = Json::parse(ss.str());

        ModuleManifest manifest;
        if (!parseModuleManifest(document, manifest)) continue;
        // Trust the filename over the document's own claim: a manifest naming a
        // different module than its filename is malformed, and honouring the
        // body would let one product silently enable the other.
        if (manifest.id != id) continue;

        manifest.manifestPath = path;
        modules_.push_back(std::move(manifest));
    }
}

void ModuleRegistry::forcePresent(ModuleId id, std::string version) {
    if (isPresent(id)) return;
    ModuleManifest manifest;
    manifest.id = id;
    manifest.present = true;
    manifest.version = std::move(version);
    manifest.manifestPath = "(forced)";
    modules_.push_back(std::move(manifest));
}

bool ModuleRegistry::isPresent(ModuleId id) const noexcept {
    const auto* m = find(id);
    return m != nullptr && m->present;
}

const ModuleManifest* ModuleRegistry::find(ModuleId id) const noexcept {
    for (const auto& m : modules_) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

bool ModuleRegistry::anyPresent() const noexcept {
    return std::any_of(modules_.begin(), modules_.end(),
                       [](const ModuleManifest& m) { return m.present; });
}

std::vector<std::string> ModuleRegistry::describe() const {
    std::vector<std::string> lines;
    for (const ModuleId id : {ModuleId::FrameGeneration, ModuleId::Upscaling}) {
        const auto* m = find(id);
        if (m && m->present) {
            lines.push_back(std::string(displayName(id)) + " " + m->version + "  [installed]");
        } else {
            lines.push_back(std::string(displayName(id)) + "  [not installed]");
        }
    }
    return lines;
}

Json ModuleRegistry::toJson() const {
    Json arr = Json::array();
    for (const ModuleId id : {ModuleId::FrameGeneration, ModuleId::Upscaling}) {
        const auto* m = find(id);
        Json e = Json::object();
        e.set("id", Json(std::string(toString(id))));
        e.set("name", Json(std::string(displayName(id))));
        e.set("installed", Json(m != nullptr && m->present));
        e.set("version", Json(m ? m->version : std::string{}));
        arr.push(std::move(e));
    }
    return arr;
}

} // namespace bfsr
