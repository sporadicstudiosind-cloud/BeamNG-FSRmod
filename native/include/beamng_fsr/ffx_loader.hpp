// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

#include "beamng_fsr/plan.hpp"
#include "beamng_fsr/result.hpp"

// AMD's public API. Fetched by tools/fetch_fsr_sdk.py rather than vendored —
// see docs/LEGAL.md for why.
#include <ffx_api.h>
#include <ffx_api_loader.h>

namespace bfsr {

/// Owns the AMD FSR runtime.
///
/// Since FSR SDK 2.0.0 the effects live behind a thin loader: the application
/// loads `amd_fidelityfx_loader_dx12.dll` and that DLL routes to
/// `amd_fidelityfx_upscaler_dx12.dll` / `amd_fidelityfx_framegeneration_dx12.dll`
/// as needed. Only the loader is named here; the effect DLLs must simply be
/// present alongside it.
///
/// The DLLs are loaded explicitly (never by implicit link) so that a missing or
/// unloadable runtime degrades into "FSR unavailable, game runs normally"
/// instead of a failure to start BeamNG at all.
class FfxLoader {
public:
    FfxLoader() = default;
    ~FfxLoader();

    FfxLoader(const FfxLoader&) = delete;
    FfxLoader& operator=(const FfxLoader&) = delete;

    /// `searchDirectory` is tried first, then the default OS search order.
    Result load(const std::string& searchDirectory);
    void unload();

    [[nodiscard]] bool isLoaded() const noexcept { return loaded_; }
    [[nodiscard]] const ffxFunctions& fn() const noexcept { return functions_; }
    [[nodiscard]] const std::string& loadedFrom() const noexcept { return loadedFrom_; }

    /// Which effect DLLs were found next to the loader. Reported in diagnostics
    /// so a user missing, say, the frame-generation DLL sees exactly that
    /// instead of an opaque failure.
    [[nodiscard]] bool upscalerDllPresent() const noexcept { return upscalerDllPresent_; }
    [[nodiscard]] bool frameGenDllPresent() const noexcept { return frameGenDllPresent_; }

    /// Routes AMD's runtime messages into our log. Safe to call once loaded.
    void installGlobalDebugHandler(bool verbose);

    /// Turns an FFX return code into a Result with AMD's own error semantics
    /// preserved (see FfxApiReturnCodes in ffx_api.h).
    static Result fromFfx(ffxReturnCode_t code, const char* what);

private:
    void* module_ = nullptr; ///< HMODULE
    ffxFunctions functions_{};
    bool loaded_ = false;
    bool upscalerDllPresent_ = false;
    bool frameGenDllPresent_ = false;
    std::string loadedFrom_;
};

/// Enumerates the providers AMD's runtime can supply *for a specific device*.
///
/// This is the mechanism the brief asks for: the version list is discovered,
/// not hardcoded, so newer AMD DLLs expose newer implementations with no code
/// change here — and the list is already filtered by AMD's driver against the
/// actual hardware, which makes it a far better authority than any device-ID
/// table this project could maintain.
///
/// `createDescType` is FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE or
/// FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION.
Result queryFfxVersions(const FfxLoader& loader,
                        uint64_t createDescType,
                        void* d3d12Device,
                        std::vector<FfxVersionEntry>& out);

} // namespace bfsr
