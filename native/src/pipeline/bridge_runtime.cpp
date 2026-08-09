// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// The bridge core: owns every subsystem and implements the C API the two proxy
// stubs call into.
//
// Failure policy, applied without exception throughout this file: BeamNG must
// keep running. Every entry point catches everything, and every path that
// cannot produce a correct result returns the unmodified object the game gave
// us. FSR turning itself off is always preferable to a crash or a wrong image.
#include "beamng_fsr/bridge_api.h"  // BFSR_CORE_EXPORTS comes from the build

#include "beamng_fsr/antilag2.hpp"
#include "beamng_fsr/capability_detector.hpp"
#include "beamng_fsr/device_proxy.hpp"
#include "beamng_fsr/factory_proxy.hpp"
#include "beamng_fsr/ffx_loader.hpp"
#include "beamng_fsr/file_ipc.hpp"
#include "beamng_fsr/frame_pacing.hpp"
#include "beamng_fsr/fsr_framegen.hpp"
#include "beamng_fsr/fsr_upscaler.hpp"
#include "beamng_fsr/logging.hpp"
#include "beamng_fsr/paths.hpp"
#include "beamng_fsr/plan.hpp"
#include "beamng_fsr/resource_registry.hpp"

#include <dxgi1_6.h>

// For the create-descriptor type constants the version query selects on.
#include <ffx_framegeneration.h>
#include <ffx_upscale.h>

#include <chrono>
#include <mutex>

#ifndef BFSR_VERSION_STRING
#  define BFSR_VERSION_STRING "0.0.0-dev"
#endif

namespace bfsr {
namespace {

/// Everything the bridge owns. One instance, created on first stub call.
class BridgeRuntime {
public:
    static BridgeRuntime& instance() {
        static BridgeRuntime inst;
        return inst;
    }

    void initialise(HMODULE stubModule) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialised_) return;
        initialised_ = true;

        paths_ = resolvePaths(stubModule);
        rotateLog(paths_.logFile);
        Logger::instance().init(paths_.logFile, LogLevel::Info);

        BFSR_LOG_INFO("BeamNG FSR Bridge %s starting", BFSR_VERSION_STRING);
        BFSR_LOG_INFO("  bridge directory: %s", paths_.bridgeDirectory.c_str());
        BFSR_LOG_INFO("  settings file:    %s", paths_.settingsFile.c_str());

        settingsStore_ = std::make_unique<SettingsStore>(paths_.settingsFile);
        std::string diagnostic;
        settings_ = settingsStore_->load(&diagnostic);
        if (!diagnostic.empty()) BFSR_LOG_INFO("settings: %s", diagnostic.c_str());
        Logger::instance().setLevel(logLevelFromString(settings_.logLevel));

        // The crash sentinel: a file written at startup and deleted on a clean
        // shutdown. Finding one means the previous session died with the bridge
        // loaded, so start disabled rather than repeat whatever killed it.
        if (sentinelExists(paths_.crashSentinelFile)) {
            BFSR_LOG_WARN("the previous session did not shut down cleanly; entering safe "
                          "mode. Clear it from the FSR settings panel once you have "
                          "checked this log.");
            settings_.safeMode = true;
            settings_.sanitise();
            settingsStore_->save(settings_);
        }
        writeSentinel(paths_.crashSentinelFile);

        ipc_ = std::make_unique<FileIpc>(paths_.ipcRequestFile, paths_.ipcStatusFile);
        publishStatus();
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialised_) return;
        BFSR_LOG_INFO("BeamNG FSR Bridge shutting down");

        // Order matters: interpolation must stop before the swapchain that
        // drives it is released, and both before the runtime unloads.
        frameGen_.destroyContext();
        frameGen_.unwrapSwapChain();
        upscaler_.destroy();
        antiLag_.shutdown();
        ffx_.unload();

        removeSentinel(paths_.crashSentinelFile);
        Logger::instance().shutdown();
        initialised_ = false;
    }

    void* wrapDevice(void* device, const IID* riid) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!device || !riid) return device;
        // Only the base interface is wrapped; see device_proxy.cpp.
        if (*riid != __uuidof(ID3D12Device)) {
            BFSR_LOG_INFO("device created through an interface this build does not wrap; "
                          "resource observation is unavailable for it");
            return device;
        }
        if (deviceProxy_) {
            BFSR_LOG_DEBUG("a second D3D12 device was created; only the first is tracked");
            return device;
        }

        auto* real = static_cast<ID3D12Device*>(device);
        realDevice_ = real;
        deviceProxy_ = new D3D12DeviceProxy(real, &registry_);
        // The proxy holds its own reference; release the caller's so the game's
        // Release on the proxy balances correctly.
        real->Release();

        detectEnvironment(real);
        return static_cast<ID3D12Device*>(deviceProxy_);
    }

    void* wrapFactory(void* factory, const IID* riid) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!factory || !riid) return factory;

        // Only a factory new enough to expose CreateSwapChainForHwnd is worth
        // wrapping; a D3D12 renderer always has one.
        IDXGIFactory7* real7 = nullptr;
        auto* unknown = static_cast<IUnknown*>(factory);
        if (FAILED(unknown->QueryInterface(__uuidof(IDXGIFactory7),
                                           reinterpret_cast<void**>(&real7))) || !real7) {
            BFSR_LOG_INFO("the DXGI factory does not expose IDXGIFactory7; frame "
                          "generation cannot attach to it");
            return factory;
        }

        auto* proxy = new DxgiFactoryProxy(real7, &BridgeRuntime::onSwapChainCreated, this);
        real7->Release(); // the proxy took its own reference
        unknown->Release();

        void* out = nullptr;
        if (FAILED(proxy->QueryInterface(*riid, &out)) || !out) {
            // The caller asked for an interface the proxy cannot serve. Give
            // the original back rather than handing over something wrong.
            proxy->Release();
            unknown->AddRef();
            return factory;
        }
        proxy->Release(); // QueryInterface added the reference the game keeps
        return out;
    }

    void onSwapChainResize(void* swapChain) {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)swapChain;
        // Buffers the frame-generation context holds refer to the swapchain's
        // old surfaces. Tear down now; the next frame rebuilds against the new
        // size via the planner.
        if (frameGen_.hasContext()) {
            BFSR_LOG_INFO("swapchain resize: releasing frame generation for rebuild");
            frameGen_.destroyContext();
        }
        gate_.reset();
        resolutionChanged_ = true;
    }

private:
    BridgeRuntime() = default;

    static IUnknown* onSwapChainCreated(IUnknown* swapChain, IUnknown* deviceOrQueue, void* ctx) {
        auto* self = static_cast<BridgeRuntime*>(ctx);
        return self ? self->handleSwapChainCreated(swapChain, deviceOrQueue) : nullptr;
    }

    IUnknown* handleSwapChainCreated(IUnknown* swapChain, IUnknown* deviceOrQueue) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!swapChain) return nullptr;

        // For D3D12 the "device" argument to CreateSwapChainForHwnd is actually
        // the command queue, which is exactly what AMD's wrap descriptor wants.
        ID3D12CommandQueue* queue = nullptr;
        if (deviceOrQueue) {
            deviceOrQueue->QueryInterface(__uuidof(ID3D12CommandQueue),
                                          reinterpret_cast<void**>(&queue));
        }
        if (!queue) {
            BFSR_LOG_INFO("swapchain was not created against a D3D12 command queue; frame "
                          "generation is unavailable for it");
            return nullptr;
        }
        gameQueue_ = queue; // reference retained for the lifetime of the swapchain
        gameSwapChain_ = swapChain;
        return nullptr;     // wrapping happens lazily, once a plan asks for it
    }

    void detectEnvironment(ID3D12Device* device) {
        const Result gpuResult = detectGpu(device, gpu_);
        if (!gpuResult) BFSR_LOG_WARN("GPU detection: %s", gpuResult.reason().c_str());
        const Result sysResult = detectSystem(device, system_);
        if (!sysResult) BFSR_LOG_WARN("system detection: %s", sysResult.reason().c_str());

        capabilities_ = evaluateCapabilities(gpu_, system_);
        logEnvironment(gpu_, system_, capabilities_);

        // Load the AMD runtime only once something might actually use it.
        if (!capabilities_.anyUpscaling() && !capabilities_.anyFrameGeneration()) {
            initFailure_ = "no FSR feature is supported on this hardware and API "
                           "combination; the bridge is inert.";
            BFSR_LOG_INFO("%s", initFailure_.c_str());
            publishStatus();
            return;
        }

        const Result loadResult = ffx_.load(paths_.bridgeDirectory);
        if (!loadResult) {
            initFailure_ = loadResult.reason();
            Logger::instance().failureBlock("AMD FSR runtime load", loadResult.reason(),
                                            "amd_fidelityfx_loader_dx12.dll and its effect DLLs",
                                            "not found or not loadable",
                                            "FSR disabled; BeamNG renders normally");
            publishStatus();
            return;
        }
        ffx_.installGlobalDebugHandler(Logger::instance().level() <= LogLevel::Debug);

        // Version discovery: AMD's own query, filtered by AMD's driver against
        // this device. This is the authority on what can run, not our tables.
        queryVersions(device);
        publishStatus();
    }

    void queryVersions(ID3D12Device* device) {
        upscalerVersions_.clear();
        frameGenVersions_.clear();

        Result r = queryFfxVersions(ffx_, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, device,
                                    upscalerVersions_);
        if (!r) {
            BFSR_LOG_WARN("upscaler version enumeration: %s", r.reason().c_str());
        } else {
            BFSR_LOG_INFO("FSR upscaler providers available for this device: %zu",
                          upscalerVersions_.size());
            for (const auto& v : upscalerVersions_) {
                BFSR_LOG_INFO("  %s%s", v.name.c_str(), v.isMachineLearning ? "  [ML]" : "");
            }
        }

        r = queryFfxVersions(ffx_, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, device,
                             frameGenVersions_);
        if (!r) {
            BFSR_LOG_WARN("frame-generation version enumeration: %s", r.reason().c_str());
        } else {
            BFSR_LOG_INFO("FSR frame-generation providers available for this device: %zu",
                          frameGenVersions_.size());
            for (const auto& v : frameGenVersions_) {
                BFSR_LOG_INFO("  %s%s", v.name.c_str(), v.isMachineLearning ? "  [ML]" : "");
            }
        }

        // An empty list here is decisive: the runtime is telling us this device
        // has no provider, which overrides any optimism in our capability table.
        if (upscalerVersions_.empty()) {
            for (auto& f : capabilities_.features) {
                if (f.feature == Feature::UpscalingMl || f.feature == Feature::UpscalingAnalytical) {
                    f.available = false;
                    if (f.unavailableReason.empty()) {
                        f.unavailableReason =
                            "The installed AMD FSR runtime reports no upscaling provider "
                            "for this GPU and driver.";
                    }
                }
            }
        }
        if (frameGenVersions_.empty()) {
            for (auto& f : capabilities_.features) {
                if (f.feature == Feature::FrameGenerationMl ||
                    f.feature == Feature::FrameGenerationAnalytical) {
                    f.available = false;
                    if (f.unavailableReason.empty()) {
                        f.unavailableReason =
                            "The installed AMD FSR runtime reports no frame-generation "
                            "provider for this GPU and driver.";
                    }
                }
            }
        }
    }

    void publishStatus() {
        if (!ipc_) return;
        IpcStatus status;
        status.bridgeVersion = BFSR_VERSION_STRING;
        status.bridgeLoaded = true;
        status.initialised = initFailure_.empty();
        status.initFailureReason = initFailure_;
        status.gpu = gpu_;
        status.system = system_;
        status.capabilities = capabilities_;
        status.plan = plan_;
        status.upscalerVersions = upscalerVersions_;
        status.frameGenVersions = frameGenVersions_;
        status.safeModeActive = settings_.safeMode;
        status.frameGenSuppressReason = describeSuppressMask(gate_.lastSuppressMask());
        status.depthSource = depthSource_;
        status.motionVectorSource = motionVectorSource_;
        status.renderedFps = renderedFrameTimes_.averageFps();
        status.onePercentLowFps = renderedFrameTimes_.percentileLowFps(0.01);
        status.pointOnePercentLowFps = renderedFrameTimes_.percentileLowFps(0.001);
        status.frameTimeMs = renderedFrameTimes_.averageFrameTimeMs();
        status.framesRendered = framesRendered_;
        status.framesGenerated = frameGen_.generatedFrameCount();
        status.upscalerVramBytes = upscalerVramBytes_;
        status.frameGenVramBytes = frameGenVramBytes_;

        const auto recent = Logger::instance().recent(64);
        status.recentLog.assign(recent.begin(), recent.end());

        ipc_->writeStatus(status);
    }

    std::mutex mutex_;
    bool initialised_ = false;
    BridgePaths paths_;

    std::unique_ptr<SettingsStore> settingsStore_;
    std::unique_ptr<FileIpc> ipc_;
    Settings settings_;
    std::string initFailure_;

    GpuInfo gpu_;
    SystemInfo system_;
    CapabilitySet capabilities_;
    ResolvedPlan plan_;
    std::vector<FfxVersionEntry> upscalerVersions_;
    std::vector<FfxVersionEntry> frameGenVersions_;

    FfxLoader ffx_;
    FsrUpscaler upscaler_;
    FsrFrameGenerator frameGen_;
    AntiLag2 antiLag_;
    ResourceRegistry registry_;
    FrameGenerationGate gate_;
    FrameTimeTracker renderedFrameTimes_;

    ID3D12Device* realDevice_ = nullptr;
    D3D12DeviceProxy* deviceProxy_ = nullptr;
    ID3D12CommandQueue* gameQueue_ = nullptr;
    IUnknown* gameSwapChain_ = nullptr;

    std::string depthSource_ = "not identified";
    std::string motionVectorSource_ = "not identified";
    uint64_t framesRendered_ = 0;
    uint64_t upscalerVramBytes_ = 0;
    uint64_t frameGenVramBytes_ = 0;
    bool resolutionChanged_ = false;
};

} // namespace
} // namespace bfsr

// ---------------------------------------------------------------------------
// Exported C API. Every function is noexcept in effect: the stubs cannot handle
// an exception crossing the DLL boundary, and neither can BeamNG.
// ---------------------------------------------------------------------------

extern "C" {

BFSR_API void bfsrCoreInitialise(HMODULE stubModule) {
    try {
        bfsr::BridgeRuntime::instance().initialise(stubModule);
    } catch (...) {
        // Nothing is safe to log yet if initialisation itself threw.
    }
}

BFSR_API void bfsrCoreShutdown(void) {
    try {
        bfsr::BridgeRuntime::instance().shutdown();
    } catch (...) {
    }
}

BFSR_API void* bfsrCoreWrapDevice(void* device, const IID* riid) {
    try {
        return bfsr::BridgeRuntime::instance().wrapDevice(device, riid);
    } catch (...) {
        return device;
    }
}

BFSR_API void* bfsrCoreWrapFactory(void* factory, const IID* riid) {
    try {
        return bfsr::BridgeRuntime::instance().wrapFactory(factory, riid);
    } catch (...) {
        return factory;
    }
}

BFSR_API void* bfsrCoreWrapSwapChain(void* swapChain, void* queue, const IID* riid) {
    (void)queue;
    (void)riid;
    // Swapchain substitution happens inside the wrapped factory, where the
    // creation parameters are still in scope. This entry point exists for the
    // stub's symmetry and deliberately does nothing.
    return swapChain;
}

BFSR_API void bfsrCoreOnSwapChainResize(void* swapChain) {
    try {
        bfsr::BridgeRuntime::instance().onSwapChainResize(swapChain);
    } catch (...) {
    }
}

} // extern "C"
