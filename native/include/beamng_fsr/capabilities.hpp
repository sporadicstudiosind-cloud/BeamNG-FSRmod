// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "beamng_fsr/json.hpp"

namespace bfsr {

// ---------------------------------------------------------------------------
// Hardware / environment description
// ---------------------------------------------------------------------------

enum class GpuVendor { Unknown, AMD, NVIDIA, Intel, Microsoft };

const char* toString(GpuVendor v) noexcept;
GpuVendor vendorFromPciId(uint32_t vendorId) noexcept;

/// Advisory only.
///
/// Deliberately *not* driven by a hardcoded PCI device-ID table: such tables go
/// stale with every new SKU and inventing entries would be exactly the kind of
/// fabricated compatibility this project refuses to ship. The authoritative
/// answer to "can this GPU run FSR 4?" comes from AMD's own
/// `ffxQuery(FFX_API_QUERY_DESC_TYPE_GET_VERSIONS)`, which takes the
/// ID3D12Device and is filtered by AMD's driver. This enum exists so the
/// diagnostics panel can print something meaningful, and so Auto mode has a
/// tie-breaker hint. It is never the sole basis for enabling a feature.
enum class GpuArchitecture {
    Unknown,
    AmdGcnOrOlder,
    AmdRdna1,      ///< RX 5000
    AmdRdna2,      ///< RX 6000
    AmdRdna3,      ///< RX 7000
    AmdRdna35,     ///< integrated / mobile refresh
    AmdRdna4,      ///< RX 9000
    NvidiaGeneric,
    IntelArc,
    IntelIntegrated,
};

const char* toString(GpuArchitecture a) noexcept;

/// Best-effort architecture guess from the adapter description string.
/// Returns Unknown when the name does not clearly identify a family — which is
/// the correct answer far more often than a guess would be.
GpuArchitecture guessArchitectureFromName(GpuVendor vendor, const std::string& name) noexcept;

struct GpuInfo {
    GpuVendor vendor = GpuVendor::Unknown;
    GpuArchitecture architecture = GpuArchitecture::Unknown;
    std::string name;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t dedicatedVideoMemory = 0; ///< bytes
    uint64_t sharedSystemMemory = 0;   ///< bytes
    std::string driverVersion;         ///< as reported by the OS; may be empty
    bool isSoftwareAdapter = false;

    [[nodiscard]] uint64_t vramMegabytes() const noexcept {
        return dedicatedVideoMemory / (1024ull * 1024ull);
    }
};

/// Which renderer BeamNG is actually running. Determined by which proxy entry
/// point the game exercised, never assumed.
enum class RendererApi { Unknown, D3D11, D3D12, Vulkan };
const char* toString(RendererApi a) noexcept;

/// Highest shader model the D3D12 device reports, encoded as major*10+minor
/// (so 66 == SM 6.6). Zero means "not queried / not applicable".
struct SystemInfo {
    bool isWindows = false;
    bool isWindows11OrGreater = false;
    uint32_t windowsBuildNumber = 0;
    uint32_t highestShaderModel = 0;     ///< 62 == SM 6.2, 66 == SM 6.6
    uint32_t d3d12FeatureLevel = 0;      ///< e.g. 0xc000 for 12_0
    uint32_t agilitySdkVersion = 0;      ///< D3D12SDKVersion actually in use, 0 if unknown
    bool waveOpsSupported = false;
    RendererApi renderer = RendererApi::Unknown;
};

// ---------------------------------------------------------------------------
// Feature model
// ---------------------------------------------------------------------------

/// The three technologies the brief insists must never be conflated, plus the
/// latency helper. Each has genuinely different requirements.
enum class Feature {
    UpscalingAnalytical,   ///< FSR 3.1.x / 2.3.x temporal upscaler
    UpscalingMl,           ///< FSR Upscaling 4.x (ML)
    FrameGenerationAnalytical, ///< FSR Frame Generation 3.1.x
    FrameGenerationMl,     ///< FSR Frame Generation 4.x (ML)
    AntiLag2,
};

const char* toString(Feature f) noexcept;
const char* displayName(Feature f) noexcept;

struct FeatureAvailability {
    Feature feature{};
    bool available = false;
    /// Empty when available. Otherwise a complete, user-facing sentence — this
    /// string is shown directly in the UI next to the disabled control, which is
    /// why it names both the requirement and what was actually found.
    std::string unavailableReason;
    /// Machine-readable requirement summary for the diagnostics report.
    std::string requirement;
};

struct CapabilitySet {
    std::vector<FeatureAvailability> features;

    [[nodiscard]] bool isAvailable(Feature f) const noexcept;
    [[nodiscard]] const FeatureAvailability* find(Feature f) const noexcept;
    [[nodiscard]] bool anyUpscaling() const noexcept;
    [[nodiscard]] bool anyFrameGeneration() const noexcept;
};

/// Pure, deterministic capability evaluation.
///
/// Encodes the published AMD requirements verified in docs/FEASIBILITY.md §4.2.
/// Split out from all D3D12 code specifically so it can be unit-tested on any
/// platform — the correctness of this table is the difference between an honest
/// mod and one that offers options that cannot work.
CapabilitySet evaluateCapabilities(const GpuInfo& gpu, const SystemInfo& sys);

/// Serialises the environment plus the capability verdicts for the IPC status
/// document and the exported diagnostic report.
Json describeEnvironment(const GpuInfo& gpu, const SystemInfo& sys, const CapabilitySet& caps);

} // namespace bfsr
