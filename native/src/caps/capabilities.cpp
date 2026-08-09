// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// Requirement sources (see docs/FEASIBILITY.md §4.2 for quotes and links):
//   FSR Upscaling 4.1.1 (ML)   RX 9000 + RX 7000 discrete, SM 6.6, DX12, Win10/11
//   FSR 3.1.5 / 2.3.4          RX 500+ "or equivalent from other GPU vendors",
//                              SM 6.2, DX12, Win10/11
//   FSR FrameGen 4.0.x (ML)    RX 9000+, CS_6_6, DX12 + Agility SDK 1.4.9+,
//                              Windows 11 ONLY
//   FSR FrameGen 3.1.x         any GPU with SM 6.2+, DX12, Win10/11
//   Anti-Lag 2                 AMD driver extension, AMD GPUs only
#include "beamng_fsr/capabilities.hpp"

#include <algorithm>
#include <cctype>

namespace bfsr {
namespace {

/// Agility SDK version AMD documents as the minimum for ML frame generation.
constexpr uint32_t kMlFrameGenMinAgilitySdk = 1409; // "1.4.9"
constexpr uint32_t kShaderModel62 = 62;
constexpr uint32_t kShaderModel66 = 66;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string shaderModelText(uint32_t sm) {
    if (sm == 0) return "unknown (device not queried)";
    return "SM " + std::to_string(sm / 10) + "." + std::to_string(sm % 10);
}

/// Extracts the leading digits of a Radeon RX model number, e.g. "RX 9070 XT"
/// -> 9070. Returns 0 when the name does not contain a recognisable RX model.
uint32_t radeonRxModelNumber(const std::string& lowered) {
    const size_t pos = lowered.find("rx ");
    if (pos == std::string::npos) return 0;
    size_t i = pos + 3;
    while (i < lowered.size() && lowered[i] == ' ') ++i;
    uint32_t value = 0;
    size_t digits = 0;
    while (i < lowered.size() && std::isdigit(static_cast<unsigned char>(lowered[i]))) {
        value = value * 10 + static_cast<uint32_t>(lowered[i] - '0');
        ++i;
        ++digits;
        if (digits > 5) return 0;
    }
    return digits >= 3 ? value : 0;
}

FeatureAvailability yes(Feature f, std::string requirement) {
    FeatureAvailability a;
    a.feature = f;
    a.available = true;
    a.requirement = std::move(requirement);
    return a;
}

FeatureAvailability no(Feature f, std::string requirement, std::string reason) {
    FeatureAvailability a;
    a.feature = f;
    a.available = false;
    a.requirement = std::move(requirement);
    a.unavailableReason = std::move(reason);
    return a;
}

/// Shared precondition for every FSR feature: the bridge only ever runs on a
/// D3D12 device, because AMD ships no Vulkan backend (FEASIBILITY §4.3).
/// Returns an empty string when the API precondition holds.
std::string apiBlockReason(const SystemInfo& sys) {
    switch (sys.renderer) {
        case RendererApi::D3D12:
            return {};
        case RendererApi::Vulkan:
            return "BeamNG is running the Vulkan renderer. AMD's FSR SDK 2.3.0 ships "
                   "DirectX 12 backends only (there is no Vulkan loader DLL), so FSR "
                   "cannot be used. Restart BeamNG with the Direct3D 12 renderer.";
        case RendererApi::D3D11:
            return "BeamNG is running the Direct3D 11 renderer. FSR 3.1 and newer "
                   "require Direct3D 12. Restart BeamNG with the Direct3D 12 renderer.";
        case RendererApi::Unknown:
        default:
            return "The active graphics API could not be determined, so FSR was not "
                   "enabled. This is expected before the game finishes creating its "
                   "device.";
    }
}

} // namespace

// ---------------------------------------------------------------------------

const char* toString(GpuVendor v) noexcept {
    switch (v) {
        case GpuVendor::AMD: return "AMD";
        case GpuVendor::NVIDIA: return "NVIDIA";
        case GpuVendor::Intel: return "Intel";
        case GpuVendor::Microsoft: return "Microsoft";
        case GpuVendor::Unknown: break;
    }
    return "Unknown";
}

GpuVendor vendorFromPciId(uint32_t vendorId) noexcept {
    switch (vendorId) {
        case 0x1002: // ATI/AMD
        case 0x1022: // AMD (some APUs enumerate under the CPU vendor ID)
            return GpuVendor::AMD;
        case 0x10DE: return GpuVendor::NVIDIA;
        case 0x8086: return GpuVendor::Intel;
        case 0x1414: return GpuVendor::Microsoft; // WARP / Basic Render Driver
        default: return GpuVendor::Unknown;
    }
}

const char* toString(GpuArchitecture a) noexcept {
    switch (a) {
        case GpuArchitecture::AmdGcnOrOlder: return "GCN or older";
        case GpuArchitecture::AmdRdna1: return "RDNA 1";
        case GpuArchitecture::AmdRdna2: return "RDNA 2";
        case GpuArchitecture::AmdRdna3: return "RDNA 3";
        case GpuArchitecture::AmdRdna35: return "RDNA 3.5";
        case GpuArchitecture::AmdRdna4: return "RDNA 4";
        case GpuArchitecture::NvidiaGeneric: return "NVIDIA";
        case GpuArchitecture::IntelArc: return "Intel Arc";
        case GpuArchitecture::IntelIntegrated: return "Intel integrated";
        case GpuArchitecture::Unknown: break;
    }
    return "Unknown";
}

GpuArchitecture guessArchitectureFromName(GpuVendor vendor, const std::string& name) noexcept {
    const std::string n = lower(name);
    switch (vendor) {
        case GpuVendor::AMD: {
            const uint32_t model = radeonRxModelNumber(n);
            if (model >= 9000 && model < 10000) return GpuArchitecture::AmdRdna4;
            if (model >= 7000 && model < 8000) return GpuArchitecture::AmdRdna3;
            if (model >= 6000 && model < 7000) return GpuArchitecture::AmdRdna2;
            if (model >= 5000 && model < 6000) return GpuArchitecture::AmdRdna1;
            if (model >= 400 && model < 600) return GpuArchitecture::AmdGcnOrOlder;
            // Integrated parts advertise "Radeon 8xxxS/Graphics" rather than "RX".
            if (n.find("radeon") != std::string::npos &&
                (n.find("graphics") != std::string::npos || n.find("890m") != std::string::npos))
                return GpuArchitecture::AmdRdna35;
            return GpuArchitecture::Unknown;
        }
        case GpuVendor::NVIDIA:
            return GpuArchitecture::NvidiaGeneric;
        case GpuVendor::Intel:
            if (n.find("arc") != std::string::npos) return GpuArchitecture::IntelArc;
            if (n.find("iris") != std::string::npos || n.find("uhd") != std::string::npos ||
                n.find("hd graphics") != std::string::npos)
                return GpuArchitecture::IntelIntegrated;
            return GpuArchitecture::Unknown;
        default:
            return GpuArchitecture::Unknown;
    }
}

const char* toString(RendererApi a) noexcept {
    switch (a) {
        case RendererApi::D3D11: return "Direct3D 11";
        case RendererApi::D3D12: return "Direct3D 12";
        case RendererApi::Vulkan: return "Vulkan";
        case RendererApi::Unknown: break;
    }
    return "Unknown";
}

const char* toString(Feature f) noexcept {
    switch (f) {
        case Feature::UpscalingAnalytical: return "upscaling_analytical";
        case Feature::UpscalingMl: return "upscaling_ml";
        case Feature::FrameGenerationAnalytical: return "framegen_analytical";
        case Feature::FrameGenerationMl: return "framegen_ml";
        case Feature::AntiLag2: return "antilag2";
    }
    return "unknown";
}

const char* displayName(Feature f) noexcept {
    switch (f) {
        // AMD's current marketing name plus the implementation version, as the
        // brief requires: neither alone is honest.
        case Feature::UpscalingAnalytical: return "FSR Upscaling (analytical, FSR 3.1.x)";
        case Feature::UpscalingMl: return "FSR Upscaling (ML, FSR 4.x \"Redstone\")";
        case Feature::FrameGenerationAnalytical: return "FSR Frame Generation (analytical, 3.1.x)";
        case Feature::FrameGenerationMl: return "FSR Frame Generation (ML, 4.x \"Redstone\")";
        case Feature::AntiLag2: return "AMD Radeon Anti-Lag 2";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------

bool CapabilitySet::isAvailable(Feature f) const noexcept {
    const auto* a = find(f);
    return a && a->available;
}

const FeatureAvailability* CapabilitySet::find(Feature f) const noexcept {
    for (const auto& a : features) {
        if (a.feature == f) return &a;
    }
    return nullptr;
}

bool CapabilitySet::anyUpscaling() const noexcept {
    return isAvailable(Feature::UpscalingAnalytical) || isAvailable(Feature::UpscalingMl);
}

bool CapabilitySet::anyFrameGeneration() const noexcept {
    return isAvailable(Feature::FrameGenerationAnalytical) || isAvailable(Feature::FrameGenerationMl);
}

// ---------------------------------------------------------------------------

CapabilitySet evaluateCapabilities(const GpuInfo& gpu, const SystemInfo& sys) {
    CapabilitySet caps;

    const std::string apiBlock = apiBlockReason(sys);
    const bool amd = gpu.vendor == GpuVendor::AMD;

    // --- FSR 3.1.x / 2.3.x analytical upscaling ------------------------------
    {
        const char* req = "DirectX 12, Shader Model 6.2, Radeon RX 500 series or "
                          "newer (or an equivalent GPU from another vendor)";
        if (!apiBlock.empty()) {
            caps.features.push_back(no(Feature::UpscalingAnalytical, req, apiBlock));
        } else if (sys.highestShaderModel < kShaderModel62) {
            caps.features.push_back(no(
                Feature::UpscalingAnalytical, req,
                "Requires Shader Model 6.2; this device reports " +
                    shaderModelText(sys.highestShaderModel) + "."));
        } else if (gpu.isSoftwareAdapter) {
            caps.features.push_back(no(
                Feature::UpscalingAnalytical, req,
                "The active adapter is a software renderer (WARP). FSR requires a "
                "hardware GPU."));
        } else {
            caps.features.push_back(yes(Feature::UpscalingAnalytical, req));
        }
    }

    // --- FSR Upscaling 4.x ML ------------------------------------------------
    //
    // AMD lists "RX 9000 and 7000 Series discrete GPUs". Note this is *broader*
    // than the 2025 launch requirement of RDNA 4 only — see FEASIBILITY §4.2.
    // The definitive gate remains AMD's runtime version query; this check only
    // stops us advertising something the hardware plainly cannot do.
    {
        const char* req = "DirectX 12, Shader Model 6.6, AMD Radeon RX 9000 or "
                          "RX 7000 series discrete GPU";
        if (!apiBlock.empty()) {
            caps.features.push_back(no(Feature::UpscalingMl, req, apiBlock));
        } else if (!amd) {
            caps.features.push_back(no(
                Feature::UpscalingMl, req,
                std::string("ML FSR Upscaling is documented by AMD for Radeon RX 9000 and "
                            "RX 7000 series GPUs only. Detected vendor: ") +
                    toString(gpu.vendor) +
                    ". AMD publishes no support statement for this hardware, so the "
                    "option is hidden rather than offered speculatively."));
        } else if (sys.highestShaderModel < kShaderModel66) {
            caps.features.push_back(no(
                Feature::UpscalingMl, req,
                "Requires Shader Model 6.6; this device reports " +
                    shaderModelText(sys.highestShaderModel) +
                    ". Updating the graphics driver may raise this."));
        } else if (gpu.architecture != GpuArchitecture::AmdRdna4 &&
                   gpu.architecture != GpuArchitecture::AmdRdna3 &&
                   gpu.architecture != GpuArchitecture::Unknown) {
            caps.features.push_back(no(
                Feature::UpscalingMl, req,
                std::string("Requires a Radeon RX 9000 (RDNA 4) or RX 7000 (RDNA 3) "
                            "discrete GPU. Detected: ") + gpu.name + " (" +
                    toString(gpu.architecture) + ")."));
        } else {
            // Unknown architecture on an AMD part with SM 6.6 is allowed through
            // deliberately: AMD's runtime query is the real gate, and refusing
            // here would block genuinely capable future SKUs this build predates.
            caps.features.push_back(yes(Feature::UpscalingMl, req));
        }
    }

    // --- FSR Frame Generation 3.1.x (analytical) -----------------------------
    {
        const char* req = "DirectX 12, Shader Model 6.2, Windows 10 or 11";
        if (!apiBlock.empty()) {
            caps.features.push_back(no(Feature::FrameGenerationAnalytical, req, apiBlock));
        } else if (sys.highestShaderModel < kShaderModel62) {
            caps.features.push_back(no(
                Feature::FrameGenerationAnalytical, req,
                "Requires Shader Model 6.2; this device reports " +
                    shaderModelText(sys.highestShaderModel) + "."));
        } else if (gpu.isSoftwareAdapter) {
            caps.features.push_back(no(
                Feature::FrameGenerationAnalytical, req,
                "The active adapter is a software renderer (WARP)."));
        } else {
            caps.features.push_back(yes(Feature::FrameGenerationAnalytical, req));
        }
    }

    // --- FSR Frame Generation 4.x ML -----------------------------------------
    //
    // Strictly narrower than ML upscaling: RX 9000+ AND Windows 11 AND Agility
    // SDK 1.4.9+. Conflating this with ML upscaling would produce a wrong matrix,
    // which is precisely the mistake the brief calls out.
    {
        const char* req = "DirectX 12 with Agility SDK 1.4.9+, HLSL CS_6_6, "
                          "AMD Radeon RX 9000 series or newer, Windows 11";
        if (!apiBlock.empty()) {
            caps.features.push_back(no(Feature::FrameGenerationMl, req, apiBlock));
        } else if (!amd) {
            caps.features.push_back(no(
                Feature::FrameGenerationMl, req,
                std::string("ML FSR Frame Generation is documented by AMD for Radeon "
                            "RX 9000 series and newer only. Detected vendor: ") +
                    toString(gpu.vendor) + "."));
        } else if (!sys.isWindows11OrGreater) {
            caps.features.push_back(no(
                Feature::FrameGenerationMl, req,
                "AMD documents ML FSR Frame Generation as Windows 11 only. This "
                "system reports Windows build " + std::to_string(sys.windowsBuildNumber) +
                ". Analytical FSR Frame Generation is available instead and does "
                "support Windows 10."));
        } else if (sys.highestShaderModel < kShaderModel66) {
            caps.features.push_back(no(
                Feature::FrameGenerationMl, req,
                "Requires HLSL CS_6_6; this device reports " +
                    shaderModelText(sys.highestShaderModel) + "."));
        } else if (sys.agilitySdkVersion != 0 && sys.agilitySdkVersion < kMlFrameGenMinAgilitySdk) {
            caps.features.push_back(no(
                Feature::FrameGenerationMl, req,
                "Requires DirectX 12 Agility SDK 1.4.9 or later; the host "
                "application is using an older redistributable."));
        } else if (gpu.architecture != GpuArchitecture::AmdRdna4 &&
                   gpu.architecture != GpuArchitecture::Unknown) {
            caps.features.push_back(no(
                Feature::FrameGenerationMl, req,
                std::string("Requires a Radeon RX 9000 series (RDNA 4) GPU or newer. "
                            "Detected: ") + gpu.name + " (" + toString(gpu.architecture) +
                    "). Analytical FSR Frame Generation is available instead."));
        } else {
            caps.features.push_back(yes(Feature::FrameGenerationMl, req));
        }
    }

    // --- Anti-Lag 2 -----------------------------------------------------------
    {
        const char* req = "AMD Radeon GPU with AMD Software: Adrenalin Edition driver, "
                          "DirectX 12";
        if (!apiBlock.empty()) {
            caps.features.push_back(no(Feature::AntiLag2, req, apiBlock));
        } else if (!amd) {
            caps.features.push_back(no(
                Feature::AntiLag2, req,
                std::string("Anti-Lag 2 is an AMD driver extension and is only "
                            "available on AMD Radeon GPUs. Detected vendor: ") +
                    toString(gpu.vendor) + "."));
        } else {
            // Presence of the driver extension entry point is only knowable at
            // runtime; the bridge re-checks and downgrades on init failure.
            caps.features.push_back(yes(Feature::AntiLag2, req));
        }
    }

    return caps;
}

Json describeEnvironment(const GpuInfo& gpu, const SystemInfo& sys, const CapabilitySet& caps) {
    Json gpuJson = Json::object();
    gpuJson.set("name", Json(gpu.name));
    gpuJson.set("vendor", Json(std::string(toString(gpu.vendor))));
    gpuJson.set("architecture", Json(std::string(toString(gpu.architecture))));
    gpuJson.set("architectureIsAdvisory", Json(true));
    gpuJson.set("vendorId", Json(static_cast<unsigned long long>(gpu.vendorId)));
    gpuJson.set("deviceId", Json(static_cast<unsigned long long>(gpu.deviceId)));
    gpuJson.set("vramMB", Json(static_cast<unsigned long long>(gpu.vramMegabytes())));
    gpuJson.set("driverVersion", Json(gpu.driverVersion));
    gpuJson.set("softwareAdapter", Json(gpu.isSoftwareAdapter));

    Json sysJson = Json::object();
    sysJson.set("renderer", Json(std::string(toString(sys.renderer))));
    sysJson.set("windows11OrGreater", Json(sys.isWindows11OrGreater));
    sysJson.set("windowsBuild", Json(static_cast<unsigned long long>(sys.windowsBuildNumber)));
    sysJson.set("highestShaderModel", Json(shaderModelText(sys.highestShaderModel)));
    sysJson.set("agilitySdkVersion", Json(static_cast<unsigned long long>(sys.agilitySdkVersion)));
    sysJson.set("waveOps", Json(sys.waveOpsSupported));

    Json featureArray = Json::array();
    for (const auto& f : caps.features) {
        Json entry = Json::object();
        entry.set("id", Json(std::string(toString(f.feature))));
        entry.set("name", Json(std::string(displayName(f.feature))));
        entry.set("available", Json(f.available));
        entry.set("requirement", Json(f.requirement));
        entry.set("reason", Json(f.unavailableReason));
        featureArray.push(std::move(entry));
    }

    Json root = Json::object();
    root.set("gpu", std::move(gpuJson));
    root.set("system", std::move(sysJson));
    root.set("features", std::move(featureArray));
    return root;
}

} // namespace bfsr
