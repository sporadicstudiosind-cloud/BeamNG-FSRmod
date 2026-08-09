// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/paths.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

#include <windows.h>

namespace bfsr {
namespace {

std::string moduleDirectory(HMODULE module) {
    char path[MAX_PATH]{};
    if (GetModuleFileNameA(module, path, MAX_PATH) == 0) return {};
    char* lastSlash = std::strrchr(path, '\\');
    if (!lastSlash) return {};
    lastSlash[1] = '\0';
    return path;
}

} // namespace

BridgePaths resolvePaths(HMODULE module) {
    BridgePaths p;
    p.bridgeDirectory = moduleDirectory(module);
    if (p.bridgeDirectory.empty()) p.bridgeDirectory = ".\\";

    p.baseDirectory = p.bridgeDirectory + "beamng_fsr\\";
    CreateDirectoryA(p.baseDirectory.c_str(), nullptr); // harmless if it exists

    p.modulesDirectory = p.baseDirectory + "modules\\";
    CreateDirectoryA(p.modulesDirectory.c_str(), nullptr);

    p.logFile = p.baseDirectory + "bridge.log";
    p.crashSentinelFile = p.baseDirectory + "session.lock";
    p.benchmarkDirectory = p.baseDirectory + "benchmarks\\";
    CreateDirectoryA(p.benchmarkDirectory.c_str(), nullptr);

    p.frameGenSettingsFile = p.baseDirectory + "framegen.settings.json";
    p.frameGenRequestFile  = p.baseDirectory + "framegen.request.json";
    p.frameGenStatusFile   = p.baseDirectory + "framegen.status.json";

    p.upscaleSettingsFile  = p.baseDirectory + "upscaling.settings.json";
    p.upscaleRequestFile   = p.baseDirectory + "upscaling.request.json";
    p.upscaleStatusFile    = p.baseDirectory + "upscaling.status.json";
    return p;
}

void rotateLog(const std::string& logFile) {
    if (logFile.empty()) return;
    const std::string previous = logFile + ".1";
    std::remove(previous.c_str());
    std::rename(logFile.c_str(), previous.c_str()); // fails harmlessly on first run
}

bool sentinelExists(const std::string& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

void writeSentinel(const std::string& path) {
    if (path.empty()) return;
    std::ofstream out(path, std::ios::trunc);
    out << "This file marks an in-progress BeamNG FSR Bridge session.\n"
           "It is deleted on a clean shutdown. If it is still here at startup,\n"
           "the previous session crashed and the bridge starts in safe mode.\n";
}

void removeSentinel(const std::string& path) {
    if (!path.empty()) std::remove(path.c_str());
}

} // namespace bfsr
