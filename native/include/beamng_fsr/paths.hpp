// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <string>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace bfsr {

/// Where the bridge keeps its own files.
///
/// Everything lives beside the bridge DLL, not in BeamNG's install tree and not
/// in BeamNG's user folder: the project never writes into anything BeamNG owns.
/// The mod half reads and writes the same IPC files through Lua's file API.
struct BridgePaths {
    std::string bridgeDirectory;
    std::string baseDirectory;      ///< bridgeDirectory + "beamng_fsr\\"
    std::string modulesDirectory;   ///< where each product drops its manifest
    std::string logFile;
    std::string crashSentinelFile;
    std::string benchmarkDirectory;

    // One settings document and one IPC channel per product, so either can be
    // installed, configured and removed without touching the other's state.
    std::string frameGenSettingsFile;
    std::string frameGenRequestFile;
    std::string frameGenStatusFile;

    std::string upscaleSettingsFile;
    std::string upscaleRequestFile;
    std::string upscaleStatusFile;
};

#if defined(_WIN32)
/// Derives all paths from the location of the calling module.
BridgePaths resolvePaths(HMODULE module);
#endif

/// Keeps one previous log as "<name>.1" so a crash report can include the run
/// before the one the user is currently in.
void rotateLog(const std::string& logFile);

bool sentinelExists(const std::string& path);
void writeSentinel(const std::string& path);
void removeSentinel(const std::string& path);

} // namespace bfsr
