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

    const std::string base = p.bridgeDirectory + "beamng_fsr\\";
    CreateDirectoryA(base.c_str(), nullptr); // harmless if it already exists

    p.settingsFile = base + "settings.json";
    p.logFile = base + "bridge.log";
    p.ipcRequestFile = base + "request.json";
    p.ipcStatusFile = base + "status.json";
    p.crashSentinelFile = base + "session.lock";
    p.benchmarkDirectory = base + "benchmarks\\";
    CreateDirectoryA(p.benchmarkDirectory.c_str(), nullptr);
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
