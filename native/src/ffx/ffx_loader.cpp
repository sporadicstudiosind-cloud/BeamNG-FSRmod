// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/ffx_loader.hpp"

#include "beamng_fsr/logging.hpp"

#include <windows.h>

namespace bfsr {
namespace {

/// AMD's loader DLL. Since SDK 2.0.0 this is the only module an application
/// links or loads; it routes to the per-effect DLLs itself.
constexpr const char* kLoaderDll = "amd_fidelityfx_loader_dx12.dll";
constexpr const char* kUpscalerDll = "amd_fidelityfx_upscaler_dx12.dll";
constexpr const char* kFrameGenDll = "amd_fidelityfx_framegeneration_dx12.dll";

bool fileExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string joinPath(const std::string& dir, const char* file) {
    if (dir.empty()) return file;
    std::string out = dir;
    if (out.back() != '\\' && out.back() != '/') out += '\\';
    out += file;
    return out;
}

/// AMD's runtime reports through a wide-string callback. Levels map onto ours.
void FFX_MESSAGE_CALLBACK(uint32_t type, const wchar_t* message) {
    if (!message) return;
    const int needed = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return;
    std::string text(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text.data(), needed, nullptr, nullptr);

    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        BFSR_LOG_ERROR("AMD FSR runtime: %s", text.c_str());
    } else {
        BFSR_LOG_WARN("AMD FSR runtime: %s", text.c_str());
    }
}

} // namespace

FfxLoader::~FfxLoader() { unload(); }

Result FfxLoader::load(const std::string& searchDirectory) {
    if (loaded_) return Result::ok();

    // Prefer the copy shipped beside the bridge. Loading by absolute path also
    // avoids picking up an unrelated copy that happens to be earlier in the
    // search order, which is a real hazard given how widely these DLLs are
    // hand-copied between game folders.
    std::string path = joinPath(searchDirectory, kLoaderDll);
    HMODULE module = nullptr;
    if (!searchDirectory.empty() && fileExists(path)) {
        module = LoadLibraryA(path.c_str());
        if (!module) {
            BFSR_LOG_WARN("found %s but LoadLibrary failed (error %lu); "
                          "falling back to the default search order",
                          path.c_str(), GetLastError());
        }
    }
    if (!module) {
        path = kLoaderDll;
        module = LoadLibraryA(kLoaderDll);
    }

    if (!module) {
        return Result::fail(
            Status::MissingDependency,
            std::string("could not load ") + kLoaderDll +
                ". The AMD FSR runtime is not installed next to the bridge. Run the "
                "installer again, or copy the signed DLLs from the AMD FSR SDK's "
                "Kits/FidelityFX/signedbin folder into the bridge directory.");
    }

    ffxLoadFunctions(&functions_, module);
    if (!functions_.CreateContext || !functions_.DestroyContext || !functions_.Configure ||
        !functions_.Query || !functions_.Dispatch) {
        FreeLibrary(module);
        functions_ = {};
        return Result::fail(
            Status::MissingDependency,
            std::string(kLoaderDll) +
                " loaded but does not export the FidelityFX API entry points. The file "
                "is probably from a pre-2.0 SDK or is corrupt; replace it with the "
                "loader from AMD FSR SDK 2.x.");
    }

    module_ = module;
    loaded_ = true;
    loadedFrom_ = path;

    // The loader routes to these; report their absence explicitly rather than
    // letting context creation fail with a generic code later on.
    upscalerDllPresent_ = fileExists(joinPath(searchDirectory, kUpscalerDll));
    frameGenDllPresent_ = fileExists(joinPath(searchDirectory, kFrameGenDll));

    BFSR_LOG_INFO("AMD FSR runtime loaded from %s", loadedFrom_.c_str());
    BFSR_LOG_INFO("  upscaler DLL present:        %s", upscalerDllPresent_ ? "yes" : "no");
    BFSR_LOG_INFO("  frame generation DLL present: %s", frameGenDllPresent_ ? "yes" : "no");
    return Result::ok();
}

void FfxLoader::unload() {
    if (module_) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
    functions_ = {};
    loaded_ = false;
    upscalerDllPresent_ = false;
    frameGenDllPresent_ = false;
    loadedFrom_.clear();
}

void FfxLoader::installGlobalDebugHandler(bool verbose) {
    if (!loaded_) return;
    ffxConfigureDescGlobalDebug desc{};
    desc.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    desc.header.pNext = nullptr;
    desc.effectId = 0; // all effects
    desc.fpMessage = &FFX_MESSAGE_CALLBACK;
    desc.debugLevel = verbose ? FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE
                              : FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    const ffxReturnCode_t rc = functions_.Configure(nullptr, &desc.header);
    if (rc != FFX_API_RETURN_OK) {
        BFSR_LOG_DEBUG("global debug callback not installed (ffx code %u)", rc);
    }
}

Result FfxLoader::fromFfx(ffxReturnCode_t code, const char* what) {
    if (code == FFX_API_RETURN_OK) return Result::ok();

    const char* detail = "unspecified error";
    Status status = Status::RuntimeError;
    switch (code) {
        case FFX_API_RETURN_ERROR:
            detail = "the runtime reported an unspecified error";
            break;
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
            detail = "the runtime did not recognise a descriptor type. The installed "
                     "AMD FSR DLLs are older than this build of the bridge expects";
            status = Status::Unsupported;
            break;
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR:
            detail = "Direct3D 12 or the effect itself returned an error";
            break;
        case FFX_API_RETURN_NO_PROVIDER:
            detail = "no provider is available for this effect. The corresponding AMD "
                     "effect DLL is missing, or this GPU cannot run the requested "
                     "implementation";
            status = Status::Unsupported;
            break;
        case FFX_API_RETURN_ERROR_MEMORY:
            detail = "a memory allocation failed";
            break;
        case FFX_API_RETURN_ERROR_PARAMETER:
            detail = "a parameter was rejected as invalid";
            status = Status::InvalidArgument;
            break;
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
            detail = "the installed provider is too old for a descriptor this bridge "
                     "uses. Update the graphics driver or the AMD FSR DLLs";
            status = Status::Unsupported;
            break;
        default:
            break;
    }
    return Result::fail(status, std::string(what) + ": " + detail + " (ffx code " +
                                    std::to_string(code) + ")");
}

Result queryFfxVersions(const FfxLoader& loader,
                        uint64_t createDescType,
                        void* d3d12Device,
                        std::vector<FfxVersionEntry>& out) {
    out.clear();
    if (!loader.isLoaded()) {
        return Result::fail(Status::NotInitialised, "the AMD FSR runtime is not loaded");
    }
    if (!d3d12Device) {
        return Result::fail(Status::InvalidArgument,
                            "a Direct3D 12 device is required to enumerate FSR versions");
    }

    // Two-pass query: first for the count, then for the data. This is AMD's
    // documented pattern (ffx_api.h, ffxQueryDescGetVersions).
    uint64_t count = 0;
    ffxQueryDescGetVersions desc{};
    desc.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    desc.header.pNext = nullptr;
    desc.createDescType = createDescType;
    desc.device = d3d12Device;
    desc.outputCount = &count;
    desc.versionIds = nullptr;
    desc.versionNames = nullptr;

    ffxReturnCode_t rc = loader.fn().Query(nullptr, &desc.header);
    if (rc != FFX_API_RETURN_OK) {
        return FfxLoader::fromFfx(rc, "enumerating FSR versions");
    }
    if (count == 0) {
        // Not an error: it means this device has no provider for this effect,
        // which is exactly what we want to know before offering the option.
        return Result::ok();
    }

    std::vector<uint64_t> ids(static_cast<size_t>(count), 0);
    std::vector<const char*> names(static_cast<size_t>(count), nullptr);
    desc.versionIds = ids.data();
    desc.versionNames = names.data();

    rc = loader.fn().Query(nullptr, &desc.header);
    if (rc != FFX_API_RETURN_OK) {
        return FfxLoader::fromFfx(rc, "reading FSR version list");
    }

    // `count` is an in/out parameter: the runtime may report fewer than the
    // capacity we offered.
    const size_t returned = static_cast<size_t>(count) <= ids.size()
                                ? static_cast<size_t>(count)
                                : ids.size();
    out.reserve(returned);
    for (size_t i = 0; i < returned; ++i) {
        FfxVersionEntry entry;
        entry.id = ids[i];
        entry.name = names[i] ? names[i] : "unnamed provider";
        entry.isMachineLearning = classifyFfxVersionAsMl(entry.name);
        out.push_back(std::move(entry));
    }
    return Result::ok();
}

} // namespace bfsr
