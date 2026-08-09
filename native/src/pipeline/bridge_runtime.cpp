// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// The shared runtime hosting one or both products.
//
// Both products ship the *same* binaries, so installing either or both never
// conflicts over a file. What differs is the manifest each drops into
// `beamng_fsr/modules/`. A feature is only started if its manifest is present,
// which is what makes "separately installed and functional on its own" real:
// with only the frame-generation manifest, no upscaling context is created, no
// upscaling settings file is read, and no upscaling status is published.
//
// Failure policy, applied without exception: BeamNG must keep running. Every
// exported entry point catches everything, and every path that cannot produce a
// correct result returns the unmodified object the game gave us. A feature
// turning itself off is always preferable to a crash or a wrong image.
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
#include "beamng_fsr/module_registry.hpp"
#include "beamng_fsr/paths.hpp"
#include "beamng_fsr/plan.hpp"
#include "beamng_fsr/resource_registry.hpp"

#include <dxgi1_6.h>

// For the create-descriptor type constants the version query selects on.
#include <ffx_framegeneration.h>
#include <ffx_upscale.h>

#include <memory>
#include <mutex>

#ifndef BFSR_VERSION_STRING
#  define BFSR_VERSION_STRING "0.0.0-dev"
#endif

namespace bfsr {
namespace {

/// State for the frame-generation product. Constructed only when its manifest
/// is present.
struct FrameGenModule {
    FrameGenSettings settings;
    FrameGenPlan plan;
    std::vector<FfxVersionEntry> versions;
    std::unique_ptr<SettingsStore<FrameGenSettings>> store;
    std::unique_ptr<FrameGenIpc> ipc;
    FsrFrameGenerator generator;
    AntiLag2 antiLag;
    FrameGenerationGate gate;
    uint64_t vramBytes = 0;
};

/// State for the upscaling product. Same contract.
struct UpscaleModule {
    UpscaleSettings settings;
    UpscalePlan plan;
    std::vector<FfxVersionEntry> versions;
    std::unique_ptr<SettingsStore<UpscaleSettings>> store;
    std::unique_ptr<UpscaleIpc> ipc;
    FsrUpscaler upscaler;
    uint64_t vramBytes = 0;
};

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

        modules_.scan(paths_.modulesDirectory);
        BFSR_LOG_INFO("Installed products:");
        for (const auto& line : modules_.describe()) {
            BFSR_LOG_INFO("  %s", line.c_str());
        }

        if (!modules_.anyPresent()) {
            // The runtime binaries are present but neither product declared
            // itself. Stay completely inert rather than guessing.
            initFailure_ =
                "Neither the frame generation nor the upscaling product is installed. "
                "The bridge binaries are present but no module manifest was found in "
                "beamng_fsr/modules/, so nothing was started. Reinstall whichever "
                "product you want.";
            BFSR_LOG_WARN("%s", initFailure_.c_str());
        }

        if (modules_.isPresent(ModuleId::FrameGeneration)) {
            frameGen_ = std::make_unique<FrameGenModule>();
            frameGen_->store = std::make_unique<SettingsStore<FrameGenSettings>>(
                paths_.frameGenSettingsFile);
            frameGen_->ipc = std::make_unique<FrameGenIpc>(paths_.frameGenRequestFile,
                                                           paths_.frameGenStatusFile);
            std::string diagnostic;
            frameGen_->settings = frameGen_->store->load(&diagnostic);
            if (!diagnostic.empty()) BFSR_LOG_INFO("frame generation settings: %s", diagnostic.c_str());
        }

        if (modules_.isPresent(ModuleId::Upscaling)) {
            upscale_ = std::make_unique<UpscaleModule>();
            upscale_->store = std::make_unique<SettingsStore<UpscaleSettings>>(
                paths_.upscaleSettingsFile);
            upscale_->ipc = std::make_unique<UpscaleIpc>(paths_.upscaleRequestFile,
                                                          paths_.upscaleStatusFile);
            std::string diagnostic;
            upscale_->settings = upscale_->store->load(&diagnostic);
            if (!diagnostic.empty()) BFSR_LOG_INFO("upscaling settings: %s", diagnostic.c_str());
        }

        applyLogLevel();

        // The crash sentinel: written at startup, deleted on clean shutdown.
        // Finding one means the previous session died with the bridge loaded,
        // so start disabled rather than repeat whatever killed it. Safe mode is
        // set on every installed product, because either could have been the
        // cause and we cannot tell which.
        if (sentinelExists(paths_.crashSentinelFile)) {
            BFSR_LOG_WARN("the previous session did not shut down cleanly; entering safe "
                          "mode. Clear it from the Advanced panel once you have checked "
                          "this log.");
            if (frameGen_) {
                frameGen_->settings.safeMode = true;
                frameGen_->settings.sanitise();
                frameGen_->store->save(frameGen_->settings);
            }
            if (upscale_) {
                upscale_->settings.safeMode = true;
                upscale_->settings.sanitise();
                upscale_->store->save(upscale_->settings);
            }
        }
        writeSentinel(paths_.crashSentinelFile);

        publishStatus();
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialised_) return;
        BFSR_LOG_INFO("BeamNG FSR Bridge shutting down");

        // Order matters: interpolation must stop before the swapchain that
        // drives it is released, and both before the runtime unloads.
        if (frameGen_) {
            frameGen_->generator.destroyContext();
            frameGen_->generator.unwrapSwapChain();
            frameGen_->antiLag.shutdown();
        }
        if (upscale_) upscale_->upscaler.destroy();
        ffx_.unload();

        removeSentinel(paths_.crashSentinelFile);
        Logger::instance().shutdown();
        initialised_ = false;
    }

    void* wrapDevice(void* device, const IID* riid) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!device || !riid) return device;
        if (!modules_.anyPresent()) return device; // nothing installed: stay out of the way

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

        // The swapchain is only worth intercepting for frame generation.
        // Upscaling alone has no reason to replace the game's swapchain, so
        // installing only that product leaves presentation completely untouched.
        if (!modules_.isPresent(ModuleId::FrameGeneration)) return factory;

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
        if (frameGen_ && frameGen_->generator.hasContext()) {
            BFSR_LOG_INFO("swapchain resize: releasing frame generation for rebuild");
            frameGen_->generator.destroyContext();
            frameGen_->gate.reset();
        }
        if (upscale_ && upscale_->upscaler.isValid()) {
            BFSR_LOG_INFO("swapchain resize: releasing the upscaler for rebuild");
            upscale_->upscaler.destroy();
        }
    }

private:
    BridgeRuntime() = default;

    void applyLogLevel() {
        // Whichever product asks for the most detail wins: a user debugging one
        // of them should not have to change the other's setting too.
        LogLevel level = LogLevel::Info;
        if (frameGen_) {
            level = std::min(level, logLevelFromString(frameGen_->settings.diagnostics.logLevel));
        }
        if (upscale_) {
            level = std::min(level, logLevelFromString(upscale_->settings.diagnostics.logLevel));
        }
        Logger::instance().setLevel(level);
    }

    static IUnknown* onSwapChainCreated(IUnknown* swapChain, IUnknown* deviceOrQueue, void* ctx) {
        auto* self = static_cast<BridgeRuntime*>(ctx);
        return self ? self->handleSwapChainCreated(swapChain, deviceOrQueue) : nullptr;
    }

    IUnknown* handleSwapChainCreated(IUnknown* swapChain, IUnknown* deviceOrQueue) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!swapChain || !frameGen_) return nullptr;

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
        gameQueue_ = queue; // reference retained for the swapchain's lifetime
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

        // Only load the AMD runtime if an installed product could use it.
        const bool needFrameGen = frameGen_ && capabilities_.anyFrameGeneration();
        const bool needUpscale = upscale_ && capabilities_.anyUpscaling();
        if (!needFrameGen && !needUpscale) {
            if (initFailure_.empty()) {
                initFailure_ =
                    "No installed FSR product is supported on this hardware and API "
                    "combination; the bridge is inert. See the per-feature reasons in "
                    "the diagnostics panel.";
                BFSR_LOG_INFO("%s", initFailure_.c_str());
            }
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

        queryVersions(device);
        publishStatus();
    }

    /// Version discovery. AMD's query takes the device, so the driver has
    /// already filtered the list against the real hardware — a far better
    /// authority than any table this project could keep current.
    void queryVersions(ID3D12Device* device) {
        if (upscale_) {
            const Result r = queryFfxVersions(ffx_, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                              device, upscale_->versions);
            if (!r) {
                BFSR_LOG_WARN("upscaler version enumeration: %s", r.reason().c_str());
            } else {
                BFSR_LOG_INFO("FSR upscaler providers for this device: %zu",
                              upscale_->versions.size());
                for (const auto& v : upscale_->versions) {
                    BFSR_LOG_INFO("  %s%s", v.name.c_str(), v.isMachineLearning ? "  [ML]" : "");
                }
            }
            // An empty list is decisive: the runtime is telling us this device
            // has no provider, which overrides any optimism in our table.
            if (upscale_->versions.empty()) {
                markUnavailable({Feature::UpscalingMl, Feature::UpscalingAnalytical},
                                "The installed AMD FSR runtime reports no upscaling provider "
                                "for this GPU and driver.");
            }
        }

        if (frameGen_) {
            const Result r = queryFfxVersions(ffx_, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
                                              device, frameGen_->versions);
            if (!r) {
                BFSR_LOG_WARN("frame-generation version enumeration: %s", r.reason().c_str());
            } else {
                BFSR_LOG_INFO("FSR frame-generation providers for this device: %zu",
                              frameGen_->versions.size());
                for (const auto& v : frameGen_->versions) {
                    BFSR_LOG_INFO("  %s%s", v.name.c_str(), v.isMachineLearning ? "  [ML]" : "");
                }
            }
            if (frameGen_->versions.empty()) {
                markUnavailable({Feature::FrameGenerationMl, Feature::FrameGenerationAnalytical},
                                "The installed AMD FSR runtime reports no frame-generation "
                                "provider for this GPU and driver.");
            }
        }
    }

    void markUnavailable(std::initializer_list<Feature> features, const char* reason) {
        for (auto& f : capabilities_.features) {
            for (Feature target : features) {
                if (f.feature != target) continue;
                f.available = false;
                if (f.unavailableReason.empty()) f.unavailableReason = reason;
            }
        }
    }

    StatusCommon buildCommon() const {
        StatusCommon c;
        c.bridgeVersion = BFSR_VERSION_STRING;
        c.bridgeLoaded = true;
        c.initialised = initFailure_.empty();
        c.initFailureReason = initFailure_;
        c.gpu = gpu_;
        c.system = system_;
        c.capabilities = capabilities_;
        c.installedModules = modules_.toJson();
        c.renderedFps = renderedFrameTimes_.averageFps();
        c.onePercentLowFps = renderedFrameTimes_.percentileLowFps(0.01);
        c.pointOnePercentLowFps = renderedFrameTimes_.percentileLowFps(0.001);
        c.frameTimeMs = renderedFrameTimes_.averageFrameTimeMs();
        c.framesRendered = framesRendered_;
        c.depthSource = depthSource_;
        c.motionVectorSource = motionVectorSource_;
        const auto recent = Logger::instance().recent(64);
        c.recentLog.assign(recent.begin(), recent.end());
        return c;
    }

    void publishStatus() {
        const StatusCommon common = buildCommon();

        if (frameGen_ && frameGen_->ipc) {
            FrameGenStatus status;
            status.moduleInstalled = true;
            status.common = common;
            status.plan = frameGen_->plan;
            status.versions = frameGen_->versions;
            status.vramBytes = frameGen_->vramBytes;
            status.framesGenerated = frameGen_->generator.generatedFrameCount();
            status.observedGeneratedPerFrame = frameGen_->generator.observedGeneratedPerFrame();
            status.suppressReason = describeSuppressMask(frameGen_->gate.lastSuppressMask());
            status.antiLagActive = frameGen_->antiLag.isActive();
            status.safeModeActive = frameGen_->settings.safeMode;
            // Displayed frame rate only means something when interpolation is
            // running, so it is reported by this product and not the other.
            status.common.displayedFps = displayedFps_;
            frameGen_->ipc->writeStatus(status);
        }

        if (upscale_ && upscale_->ipc) {
            UpscaleStatus status;
            status.moduleInstalled = true;
            status.common = common;
            status.plan = upscale_->plan;
            status.versions = upscale_->versions;
            status.vramBytes = upscale_->vramBytes;
            status.jitterAvailable = false; // BeamNG exposes no jitter hook
            status.safeModeActive = upscale_->settings.safeMode;
            upscale_->ipc->writeStatus(status);
        }
    }

    std::mutex mutex_;
    bool initialised_ = false;
    BridgePaths paths_;
    ModuleRegistry modules_;
    std::string initFailure_;

    std::unique_ptr<FrameGenModule> frameGen_;
    std::unique_ptr<UpscaleModule> upscale_;

    GpuInfo gpu_;
    SystemInfo system_;
    CapabilitySet capabilities_;

    FfxLoader ffx_;
    ResourceRegistry registry_;
    FrameTimeTracker renderedFrameTimes_;

    ID3D12Device* realDevice_ = nullptr;
    D3D12DeviceProxy* deviceProxy_ = nullptr;
    ID3D12CommandQueue* gameQueue_ = nullptr;
    IUnknown* gameSwapChain_ = nullptr;

    std::string depthSource_ = "not identified";
    std::string motionVectorSource_ = "not identified";
    uint64_t framesRendered_ = 0;
    float displayedFps_ = 0.0f;
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
