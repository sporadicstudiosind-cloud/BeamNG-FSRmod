// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

#include "beamng_fsr/json.hpp"

namespace bfsr {

/// The two products this runtime can host.
enum class ModuleId { FrameGeneration, Upscaling };

const char* toString(ModuleId id) noexcept;
const char* displayName(ModuleId id) noexcept;

/// One installed product, as declared by its manifest.
struct ModuleManifest {
    ModuleId id{};
    bool present = false;
    std::string version;      ///< The product version that dropped the manifest.
    std::string manifestPath;
};

/// Decides which products are installed, and therefore which features the
/// shared runtime is allowed to start.
///
/// Both products ship the *same* runtime binaries, so installing one, the
/// other, or both never conflicts over a file. What differs is the manifest
/// each drops into `beamng_fsr/modules/`. The runtime enables a feature only if
/// its manifest is present, which is what makes "installed separately and
/// functional on its own" true rather than aspirational: with only the frame
/// generation manifest present, no upscaling context is ever created, no
/// upscaling settings file is read, and no upscaling status is published.
class ModuleRegistry {
public:
    /// Scans `modulesDirectory` for `*.module.json`. Missing directory is not an
    /// error — it means neither product is installed, and the runtime stays
    /// inert rather than failing.
    void scan(const std::string& modulesDirectory);

    /// Declares a module present without a file on disk. Used by tests and by
    /// the `BFSR_FORCE_MODULES` escape hatch.
    void forcePresent(ModuleId id, std::string version);

    [[nodiscard]] bool isPresent(ModuleId id) const noexcept;
    [[nodiscard]] const ModuleManifest* find(ModuleId id) const noexcept;
    [[nodiscard]] bool anyPresent() const noexcept;
    [[nodiscard]] const std::vector<ModuleManifest>& modules() const noexcept { return modules_; }

    /// One line per module for the startup log.
    [[nodiscard]] std::vector<std::string> describe() const;
    [[nodiscard]] Json toJson() const;

private:
    std::vector<ModuleManifest> modules_;
};

/// Parses a manifest document. Returns false when it is not a manifest this
/// runtime recognises.
bool parseModuleManifest(const Json& document, ModuleManifest& out);

/// The filename a product drops to declare itself.
std::string moduleManifestFileName(ModuleId id);

} // namespace bfsr
