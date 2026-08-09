// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/capability_detector.hpp"

#include "beamng_fsr/logging.hpp"

#include <dxgi1_6.h>

#include <vector>

// Feature level 12_2 postdates some Windows SDK and mingw-w64 header versions.
// The value is fixed by Microsoft (D3D_FEATURE_LEVEL is a stable public enum),
// so define it when the headers are older rather than silently reporting a
// lower cap than the device actually supports.
#ifndef D3D_FEATURE_LEVEL_12_2
#  define D3D_FEATURE_LEVEL_12_2 static_cast<D3D_FEATURE_LEVEL>(0xc200)
#endif

namespace bfsr {
namespace {

std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

/// Highest shader model the device supports, as major*10+minor.
///
/// CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL) is an in/out query: it caps
/// the requested model to what the device supports, but returns E_INVALIDARG if
/// the requested model predates the runtime's knowledge. Walking down from the
/// highest is the documented way to get a definite answer.
uint32_t detectHighestShaderModel(ID3D12Device* device) {
    static const D3D_SHADER_MODEL kCandidates[] = {
        static_cast<D3D_SHADER_MODEL>(0x69), // 6.9
        static_cast<D3D_SHADER_MODEL>(0x68),
        static_cast<D3D_SHADER_MODEL>(0x67),
        static_cast<D3D_SHADER_MODEL>(0x66), // the FSR 4.x / ML frame-gen threshold
        static_cast<D3D_SHADER_MODEL>(0x65),
        static_cast<D3D_SHADER_MODEL>(0x64),
        static_cast<D3D_SHADER_MODEL>(0x63),
        static_cast<D3D_SHADER_MODEL>(0x62), // the FSR 3.1.x threshold
        static_cast<D3D_SHADER_MODEL>(0x61),
        static_cast<D3D_SHADER_MODEL>(0x60),
    };

    for (D3D_SHADER_MODEL candidate : kCandidates) {
        D3D12_FEATURE_DATA_SHADER_MODEL data{};
        data.HighestShaderModel = candidate;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data)))) {
            const uint32_t raw = static_cast<uint32_t>(data.HighestShaderModel);
            return ((raw >> 4) & 0xF) * 10 + (raw & 0xF);
        }
    }
    return 0;
}

bool detectWaveOps(ID3D12Device* device) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)))) {
        return false;
    }
    return options.WaveOps != FALSE;
}

/// Reads the real OS build from the kernel rather than GetVersionEx, which
/// version-lies unless the application ships a compatibility manifest — and we
/// are a library inside someone else's process, so we cannot rely on theirs.
void detectWindowsVersion(SystemInfo& sys) {
    sys.isWindows = true;

    using PfnRtlGetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;

    auto rtlGetVersion = reinterpret_cast<PfnRtlGetVersion>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (!rtlGetVersion) return;

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0) return;

    sys.windowsBuildNumber = info.dwBuildNumber;
    // Windows 11 still reports major version 10; build 22000 is the documented
    // dividing line. AMD's ML frame-generation requirement depends on this, so
    // getting it right is not cosmetic.
    sys.isWindows11OrGreater = info.dwMajorVersion > 10 ||
                               (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000);
}

} // namespace

Result detectGpu(ID3D12Device* device, GpuInfo& out) {
    out = GpuInfo{};
    if (!device) return Result::fail(Status::InvalidArgument, "no Direct3D 12 device");

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return Result::fail(Status::RuntimeError, "could not create a DXGI factory to enumerate adapters");
    }

    // Match by LUID rather than taking adapter 0: on a laptop with switchable
    // graphics, adapter 0 is frequently not the adapter the game is running on.
    const LUID target = device->GetAdapterLuid();
    Result result = Result::fail(Status::RuntimeError,
                                 "the adapter backing the Direct3D 12 device was not found");

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            desc.AdapterLuid.LowPart == target.LowPart &&
            desc.AdapterLuid.HighPart == target.HighPart) {
            out.name = wideToUtf8(desc.Description);
            out.vendorId = desc.VendorId;
            out.deviceId = desc.DeviceId;
            out.dedicatedVideoMemory = desc.DedicatedVideoMemory;
            out.sharedSystemMemory = desc.SharedSystemMemory;
            out.isSoftwareAdapter = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            out.vendor = vendorFromPciId(desc.VendorId);
            out.architecture = guessArchitectureFromName(out.vendor, out.name);
            result = Result::ok();
            adapter->Release();
            break;
        }
        adapter->Release();
    }

    factory->Release();
    return result;
}

Result detectSystem(ID3D12Device* device, SystemInfo& out) {
    out = SystemInfo{};
    detectWindowsVersion(out);

    if (!device) {
        return Result::fail(Status::InvalidArgument, "no Direct3D 12 device");
    }

    // Reaching this code at all means a D3D12 device exists, which is the only
    // renderer FSR can use.
    out.renderer = RendererApi::D3D12;
    out.highestShaderModel = detectHighestShaderModel(device);
    out.waveOpsSupported = detectWaveOps(device);

    D3D12_FEATURE_DATA_FEATURE_LEVELS levels{};
    static const D3D_FEATURE_LEVEL kLevels[] = {
        static_cast<D3D_FEATURE_LEVEL>(D3D_FEATURE_LEVEL_12_2), D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    levels.NumFeatureLevels = static_cast<UINT>(std::size(kLevels));
    levels.pFeatureLevelsRequested = kLevels;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)))) {
        out.d3d12FeatureLevel = static_cast<uint32_t>(levels.MaxSupportedFeatureLevel);
    }

    // The Agility SDK version in use is a property of the *host application*'s
    // redistributable, and D3D12 exposes no query for it. Leaving it at zero is
    // the honest answer; the capability policy treats zero as "unknown" and
    // does not block on it rather than inventing a number.
    out.agilitySdkVersion = 0;

    return Result::ok();
}

void logEnvironment(const GpuInfo& gpu, const SystemInfo& sys, const CapabilitySet& caps) {
    BFSR_LOG_INFO("GPU detected:      %s", gpu.name.c_str());
    BFSR_LOG_INFO("  vendor:          %s (0x%04X)", toString(gpu.vendor), gpu.vendorId);
    BFSR_LOG_INFO("  device id:       0x%04X", gpu.deviceId);
    BFSR_LOG_INFO("  architecture:    %s (advisory)", toString(gpu.architecture));
    BFSR_LOG_INFO("  dedicated VRAM:  %llu MB",
                  static_cast<unsigned long long>(gpu.vramMegabytes()));
    BFSR_LOG_INFO("API detected:      %s", toString(sys.renderer));
    BFSR_LOG_INFO("  shader model:    %u.%u", sys.highestShaderModel / 10, sys.highestShaderModel % 10);
    BFSR_LOG_INFO("  wave ops:        %s", sys.waveOpsSupported ? "yes" : "no");
    BFSR_LOG_INFO("OS:                Windows build %u%s", sys.windowsBuildNumber,
                  sys.isWindows11OrGreater ? " (Windows 11 or newer)" : "");

    BFSR_LOG_INFO("Feature availability:");
    for (const auto& f : caps.features) {
        if (f.available) {
            BFSR_LOG_INFO("  [available]   %s", displayName(f.feature));
        } else {
            BFSR_LOG_INFO("  [unavailable] %s", displayName(f.feature));
            BFSR_LOG_INFO("                %s", f.unavailableReason.c_str());
        }
    }
}

} // namespace bfsr
