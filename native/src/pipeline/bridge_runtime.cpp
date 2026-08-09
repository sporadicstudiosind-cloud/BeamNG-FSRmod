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
#include "beamng_fsr/frame_command_ring.hpp"
#include "beamng_fsr/frame_pacing.hpp"
#include "beamng_fsr/fsr_framegen.hpp"
#include "beamng_fsr/fsr_upscaler.hpp"
#include "beamng_fsr/logging.hpp"
#include "beamng_fsr/module_registry.hpp"
#include "beamng_fsr/paths.hpp"
#include "beamng_fsr/plan.hpp"
#include "beamng_fsr/present_blit.hpp"
#include "beamng_fsr/resource_registry.hpp"
#include "beamng_fsr/swapchain_proxy.hpp"

#include <dxgi1_6.h>

// For the create-descriptor type constants the version query selects on.
#include <ffx_framegeneration.h>
#include <ffx_upscale.h>

#include <chrono>
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

        // Order matters, and this is the order.
        //
        // The game may still present while the process tears down, so the hooks
        // come off first — after that a Present is a plain forward and cannot
        // reach anything being destroyed. Then the GPU is drained, because the
        // command ring's submissions reference FSR's resources. Only then may
        // interpolation stop, the swapchain be released, and the runtime unload.
        if (swapChainProxy_) swapChainProxy_->detachHooks();
        ring_.destroy();
        upscaleRing_.destroy();
        blit_.destroy();
        if (frameGen_) {
            frameGen_->generator.destroyContext();
            frameGen_->generator.unwrapSwapChain();
            frameGen_->antiLag.shutdown();
        }
        if (upscale_) upscale_->upscaler.destroy();
        ffx_.unload();

        // The references taken when the swapchain was wrapped. The game may
        // still hold its own on the proxy, which is why this is a release and
        // not a delete.
        if (swapChainProxy_) {
            swapChainProxy_->Release();
            swapChainProxy_ = nullptr;
        }
        if (gameQueue_) {
            gameQueue_->Release();
            gameQueue_ = nullptr;
        }

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

        // Both products need the swapchain now: frame generation to install
        // AMD's interpolation swapchain, upscaling to substitute the
        // backbuffers. With neither installed the factory is left alone.
        if (!modules_.anyPresent()) return factory;

        IDXGIFactory7* real7 = nullptr;
        auto* unknown = static_cast<IUnknown*>(factory);
        if (FAILED(unknown->QueryInterface(__uuidof(IDXGIFactory7),
                                           reinterpret_cast<void**>(&real7))) || !real7) {
            BFSR_LOG_INFO("the DXGI factory does not expose IDXGIFactory7; neither product "
                          "can attach to it");
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
        //
        // The command ring is drained first because its in-flight submissions
        // reference those same resources, and the beacon cache is dropped
        // because the engine will re-create its render targets at the new size.
        ring_.waitForIdle();
        upscaleRing_.waitForIdle();
        upscaleNeedsReset_ = true;
        beaconCacheValid_ = false;
        depthResource_ = nullptr;
        motionResource_ = nullptr;
        displaySize_ = {};

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
        if (!swapChain || !modules_.anyPresent()) return nullptr;
        if (swapChainProxy_) {
            BFSR_LOG_DEBUG("a second swapchain was created; only the first is driven");
            return nullptr;
        }

        // For D3D12 the "device" argument to CreateSwapChainForHwnd is actually
        // the command queue, which is exactly what AMD's wrap descriptor wants.
        ID3D12CommandQueue* queue = nullptr;
        if (deviceOrQueue) {
            deviceOrQueue->QueryInterface(__uuidof(ID3D12CommandQueue),
                                          reinterpret_cast<void**>(&queue));
        }
        if (!queue) {
            BFSR_LOG_INFO("swapchain was not created against a D3D12 command queue; neither "
                          "product can attach to it");
            return nullptr;
        }

        // AMD's wrap descriptor and this bridge's own proxy both need
        // IDXGISwapChain4.
        IDXGISwapChain4* chain4 = nullptr;
        if (FAILED(swapChain->QueryInterface(__uuidof(IDXGISwapChain4),
                                             reinterpret_cast<void**>(&chain4))) ||
            !chain4) {
            queue->Release();
            BFSR_LOG_INFO("the swapchain does not expose IDXGISwapChain4; frame generation "
                          "cannot attach to it");
            return nullptr;
        }

        gameQueue_ = queue; // reference retained for the swapchain's lifetime

        // Composition order matters. AMD's frame-interpolation swapchain
        // replaces the DXGI one and schedules the generated frame; this
        // bridge's proxy then wraps *that*, so the per-frame hook runs before
        // AMD's swapchain sees the Present. Wrapping the other way round would
        // prepare a frame that had already been presented.
        if (frameGen_ && ffx_.isLoaded() && capabilities_.anyFrameGeneration()) {
            const Result wrapped = frameGen_->generator.wrapSwapChain(ffx_, &chain4, gameQueue_);
            if (!wrapped) {
                Logger::instance().failureBlock(
                    "frame-generation swapchain", wrapped.reason(),
                    "AMD's frame-interpolation swapchain", "could not be created",
                    "frame generation stays off; BeamNG presents normally");
                frameGenSwapChainFailure_ = wrapped.reason();
            }
        } else if (frameGen_) {
            frameGenSwapChainFailure_ =
                ffx_.isLoaded()
                    ? std::string("This GPU, driver and API combination supports no FSR "
                                  "frame-generation provider.")
                    : std::string("The AMD FSR runtime is not loaded, so no frame-generation "
                                  "swapchain could be created.");
        }

        // The proxy is installed even when AMD's swapchain could not be
        // created. It costs one forwarded call per Present and it is what keeps
        // the frame timer, the IPC poll and the status document live, so the
        // panel can report *why* frame generation is off instead of going
        // silent.
        swapChainProxy_ = new SwapChainProxy(chain4, &BridgeRuntime::onPresentThunk,
                                             &BridgeRuntime::onResizeThunk, this);
        chain4->Release(); // the proxy took its own reference

        // Backbuffer substitution has to be decided now, before the game calls
        // GetBuffer and builds render-target views it will keep for the
        // session. There is no plan yet, so the ratio comes straight from the
        // persisted settings — deterministic, and the planner still gets the
        // final say each frame over whether a dispatch happens.
        enableSubstitutionIfWanted();

        // Two reference-count details, both of which are bugs if omitted.
        //
        // First: the factory proxy hands the game this object *in place of* the
        // one DXGI created, so the reference the game was going to hold on that
        // original is now held by nobody. Releasing it here is what stops the
        // real swapchain outliving the process.
        swapChain->Release();

        // Second: the proxy is constructed with the single reference the game
        // takes, and this raw pointer would dangle the moment the game released
        // it — while shutdown() still expects to call detachHooks() on it. So
        // the runtime takes a reference of its own, released in shutdown().
        swapChainProxy_->AddRef();

        BFSR_LOG_INFO("per-frame driver attached to the swapchain");
        publishStatus();
        return static_cast<IDXGISwapChain4*>(swapChainProxy_);
    }

    /// Decides whether to substitute the backbuffers, and does it.
    ///
    /// Deliberately conservative: substitution changes what the game believes
    /// its swapchain is, and the failure mode is a black screen rather than a
    /// soft image. It is therefore only installed when upscaling is installed,
    /// switched on, out of safe mode, supported by the hardware, and asking for
    /// a ratio above 1. Anything else leaves the proxy a pure pass-through.
    void enableSubstitutionIfWanted() {
        if (!upscale_ || !swapChainProxy_ || !realDevice_) return;
        if (!upscale_->settings.enabled || upscale_->settings.safeMode) return;
        if (!capabilities_.anyUpscaling() || !ffx_.isLoaded()) return;

        const float ratio = upscale_->settings.quality == QualityMode::Custom
                                ? upscale_->settings.customUpscaleRatio
                                : qualityModeRatio(upscale_->settings.quality);
        if (ratio <= 1.0f) {
            // Native AA renders at display resolution, so there is nothing to
            // substitute. FSR would still sharpen, but not through this path.
            BFSR_LOG_INFO("upscaling is enabled at a 1.0x ratio; no backbuffer substitution "
                          "is needed");
            return;
        }

        // The swapchain's own description, read directly: the proxy only learns
        // its format as part of enabling substitution, and the blit has to be
        // built before that so a failure leaves nothing substituted.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(swapChainProxy_->real()->GetDesc1(&desc)) ||
            desc.Format == DXGI_FORMAT_UNKNOWN) {
            BFSR_LOG_WARN("the swapchain description could not be read; upscaling stays off");
            return;
        }

        // The fallback blit comes first, and substitution is refused without
        // it. Once the game is rendering into our textures, something must
        // write the real backbuffer every single frame; if the only thing that
        // can do that is an FSR dispatch which has not started yet, the first
        // frames are a black screen. Creating the blit up front makes "we can
        // always produce a frame" a precondition rather than a hope.
        const Result blit = blit_.create(realDevice_, desc.Format);
        if (!blit) {
            upscaleFailure_ = blit.reason();
            Logger::instance().failureBlock("backbuffer substitution", blit.reason(),
                                            "the fallback present blit",
                                            "could not be created",
                                            "upscaling disabled; BeamNG renders at native");
            return;
        }

        const Result substituted = swapChainProxy_->enableSubstitution(realDevice_, ratio);
        if (!substituted) {
            blit_.destroy();
            upscaleFailure_ = substituted.reason();
            Logger::instance().failureBlock("backbuffer substitution", substituted.reason(),
                                            "substitute backbuffers at render resolution",
                                            "could not be created",
                                            "upscaling disabled; BeamNG renders at native");
            return;
        }
        upscaleFailure_.clear();
    }

    // -----------------------------------------------------------------------
    // The per-frame driver.
    //
    // Everything above this line runs once. This is the part that runs every
    // frame, and it is what turns a set of configured FSR objects into a
    // working integration: without it nothing resolves a plan, creates a
    // context, locates the beacon or dispatches anything.
    // -----------------------------------------------------------------------

    static void onPresentThunk(SwapChainProxy* proxy, UINT syncInterval, UINT flags, void* ctx) {
        auto* self = static_cast<BridgeRuntime*>(ctx);
        if (!self) return;
        try {
            self->onPresent(proxy, syncInterval, flags);
        } catch (...) {
            // A throw here would cross into BeamNG's render loop. Losing a
            // frame of FSR is always preferable.
        }
    }

    static void onResizeThunk(SwapChainProxy* proxy, void* ctx) {
        auto* self = static_cast<BridgeRuntime*>(ctx);
        if (!self) return;
        try {
            self->onSwapChainResize(proxy);
        } catch (...) {
        }
    }

    void onPresent(SwapChainProxy* proxy, UINT syncInterval, UINT flags) {
        (void)syncInterval;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialised_ || !proxy) return;

        // A test present is how an application probes an occluded window. It
        // presents nothing, so there is no frame to work on — and it is the
        // one occlusion signal available before Present returns.
        if ((flags & DXGI_PRESENT_TEST) != 0) {
            windowOccluded_ = true;
            return;
        }
        windowOccluded_ = false;

        const auto now = std::chrono::steady_clock::now();
        float frameTimeMs = 0.0f;
        if (haveLastPresent_) {
            frameTimeMs = std::chrono::duration<float, std::milli>(now - lastPresent_).count();
            if (frameTimeMs > 0.0f) renderedFrameTimes_.addFrame(frameTimeMs);
        }
        lastPresent_ = now;
        haveLastPresent_ = true;
        ++framesRendered_;
        ++frameId_;

        // File I/O on the render thread is paced rather than per-frame: nothing
        // in the request document changes faster than a user can click, and the
        // cost of a stat() every frame is not worth paying for that.
        if (now - lastRequestPoll_ >= std::chrono::milliseconds(150)) {
            lastRequestPoll_ = now;
            pollRequests();
        }

        // Both products consume the beacons, so they are resolved once, before
        // either drive runs. Doing it inside one of them would leave the other
        // reading whatever the previous frame left behind — and on a
        // single-product install, nothing at all.
        resolveBeacons();

        // Upscaling runs first and frame generation second, which is the order
        // AMD specifies and the only one that makes sense here: interpolation
        // works on the frames that are actually presented, and under
        // substitution those are the upscaled ones.
        if (upscale_) driveUpscaling(proxy, frameTimeMs);
        if (frameGen_) driveFrameGeneration(proxy, frameTimeMs);

        if (now - lastStatusPublish_ >= std::chrono::milliseconds(400)) {
            lastStatusPublish_ = now;
            displayedFps_ = computeDisplayedFps();
            publishStatus();
        }
    }

    /// Reads whatever each installed product's mod half last wrote.
    void pollRequests() {
        if (frameGen_ && frameGen_->ipc) {
            FrameGenRequest request;
            std::string diagnostic;
            if (frameGen_->ipc->readRequest(request, &diagnostic)) {
                applyFrameGenRequest(request);
            } else if (!diagnostic.empty() && diagnostic != lastFrameGenIpcDiagnostic_) {
                lastFrameGenIpcDiagnostic_ = diagnostic;
                BFSR_LOG_WARN("frame generation request: %s", diagnostic.c_str());
            }
        }

        if (upscale_ && upscale_->ipc) {
            UpscaleRequest request;
            std::string diagnostic;
            if (upscale_->ipc->readRequest(request, &diagnostic)) {
                applyUpscaleRequest(request);
            } else if (!diagnostic.empty() && diagnostic != lastUpscaleIpcDiagnostic_) {
                lastUpscaleIpcDiagnostic_ = diagnostic;
                BFSR_LOG_WARN("upscaling request: %s", diagnostic.c_str());
            }
        }
    }

    void applyFrameGenRequest(const FrameGenRequest& request) {
        shared_ = request.shared;
        haveSharedState_ = true;

        FrameGenSettings incoming = request.settings;
        // Safe mode is the bridge's state, not the mod's: the mod half has no
        // way to know the previous session crashed. It survives a settings
        // push and is cleared only by the explicit request below.
        incoming.safeMode = frameGen_->settings.safeMode;
        if (request.clearSafeMode && incoming.safeMode) {
            incoming.safeMode = false;
            BFSR_LOG_INFO("safe mode cleared for frame generation at the user's request");
        }
        incoming.sanitise();

        if (incoming != frameGen_->settings) {
            frameGen_->settings = incoming;
            frameGen_->store->save(frameGen_->settings);
            applyLogLevel();
            BFSR_LOG_INFO("frame generation settings updated from the mod half");
        }
    }

    void applyUpscaleRequest(const UpscaleRequest& request) {
        shared_ = request.shared;
        haveSharedState_ = true;

        UpscaleSettings incoming = request.settings;
        incoming.safeMode = upscale_->settings.safeMode;
        if (request.clearSafeMode && incoming.safeMode) {
            incoming.safeMode = false;
            BFSR_LOG_INFO("safe mode cleared for upscaling at the user's request");
        }
        incoming.sanitise();

        if (incoming != upscale_->settings) {
            upscale_->settings = incoming;
            upscale_->store->save(upscale_->settings);
            applyLogLevel();
            BFSR_LOG_INFO("upscaling settings updated from the mod half");
        }
    }

    /// Resolves the two beacon render targets, caching while the registry is
    /// unchanged.
    ///
    /// Caching is not an optimisation here so much as a correctness boundary: a
    /// cached ID3D12Resource* is only safe while nothing has been destroyed,
    /// and the registry's generation counter is exactly that condition.
    void resolveBeacons() {
        if (!haveSharedState_) {
            depthSource_ = "the BeamNG mod half has not reported in yet";
            motionVectorSource_ = depthSource_;
            depthResource_ = nullptr;
            motionResource_ = nullptr;
            return;
        }

        const uint64_t generation = registry_.generation();
        if (beaconCacheValid_ && generation == beaconCacheGeneration_ &&
            shared_.depthBeacon.width == cachedWidth_ &&
            shared_.depthBeacon.height == cachedHeight_ &&
            shared_.depthBeacon.valid == cachedSignatureValid_) {
            return;
        }
        beaconCacheGeneration_ = generation;
        cachedWidth_ = shared_.depthBeacon.width;
        cachedHeight_ = shared_.depthBeacon.height;
        cachedSignatureValid_ = shared_.depthBeacon.valid;
        beaconCacheValid_ = true;

        const auto resources = registry_.snapshot();
        const MatchResult depth = matchBeacon(shared_.depthBeacon, resources);
        const MatchResult motion = matchBeacon(shared_.motionBeacon, resources);

        // Ambiguous is accepted with the reason recorded; None is not. The
        // difference matters: an ambiguous match picked the newest of several
        // identically-described targets, which is nearly always the live one
        // after a resolution change, while no match at all means the bridge
        // does not know where the data is.
        depthResource_ = depth.confidence == MatchConfidence::None
                             ? nullptr
                             : reinterpret_cast<ID3D12Resource*>(depth.handle);
        motionResource_ = motion.confidence == MatchConfidence::None
                              ? nullptr
                              : reinterpret_cast<ID3D12Resource*>(motion.handle);

        depthSource_ = std::string(toString(depth.confidence)) + ": " + depth.explanation;
        motionVectorSource_ = std::string(toString(motion.confidence)) + ": " + motion.explanation;

        if (depthSource_ != lastLoggedDepthSource_) {
            lastLoggedDepthSource_ = depthSource_;
            BFSR_LOG_INFO("depth beacon: %s", depthSource_.c_str());
            BFSR_LOG_INFO("motion beacon: %s", motionVectorSource_.c_str());
        }
    }

    RuntimeContext buildRuntimeContext(Extent2D displaySize) {
        RuntimeContext ctx;
        ctx.displaySize = displaySize;
        // The bridge cannot make BeamNG render its scene at a resolution other
        // than the one it presents at (docs/LIMITATIONS.md §2), so these are
        // the same number. It stays a separate field because the planners are
        // written for the day that changes.
        ctx.sceneRenderSize = displaySize;
        ctx.availableVramBytes = gpu_.dedicatedVideoMemory;
        ctx.depthAvailable = depthResource_ != nullptr;
        ctx.motionVectorsAvailable = motionResource_ != nullptr;
        ctx.frameInputsAvailable = ctx.depthAvailable && ctx.motionVectorsAvailable;
        ctx.cameraJitterAvailable = false; // BeamNG exposes no jitter hook
        ctx.measuredBaseFps = renderedFrameTimes_.averageFps();
        if (frameGen_) {
            ctx.frameGenVersions = frameGen_->versions;
            ctx.estimatedFrameGenVramBytes = frameGen_->vramBytes;
        }
        if (upscale_) {
            ctx.upscalerVersions = upscale_->versions;
            ctx.estimatedUpscalerVramBytes = upscale_->vramBytes;
        }
        return ctx;
    }


    /// Upscaling's per-frame work: FSR reads the substituted backbuffer the
    /// game rendered into and writes the real one that is about to be presented.
    ///
    /// Something must write the real backbuffer every frame once substitution
    /// is on, because the game is no longer writing it. When FSR cannot run —
    /// the mod half has not published its beacons yet, the plan is inactive, a
    /// dispatch failed — the fallback blit stretches the frame instead. That
    /// path exists so a missing dispatch is a soft image rather than a black
    /// screen, and it is the reason substitution is safe to leave installed.
    void driveUpscaling(SwapChainProxy* proxy, float frameTimeMs) {
        UpscaleModule& up = *upscale_;

        if (!proxy->substituting()) {
            // No substitution means no colour input and no output to write, so
            // there is nothing for the upscaler to do. The plan is cleared so
            // the panel reports inactive rather than a stale active plan.
            if (up.upscaler.isValid()) {
                up.upscaler.destroy();
                upscaleRing_.destroy();
                blit_.destroy();
            }
            up.plan = UpscalePlan{};
            return;
        }

        const Extent2D render{proxy->renderWidth(), proxy->renderHeight()};
        const Extent2D present{proxy->presentWidth(), proxy->presentHeight()};
        if (!render.valid() || !present.valid()) return;

        RuntimeContext ctx = buildRuntimeContext(present);
        ctx.sceneRenderSize = render;
        up.plan = resolveUpscalePlan(up.settings, capabilities_, ctx);

        ID3D12Resource* source = proxy->currentSubstitute();
        ID3D12Resource* target = proxy->acquireRealBackBuffer();
        if (!source || !target) {
            if (target) target->Release();
            return;
        }

        const bool wantDispatch = up.plan.active() && depthResource_ && motionResource_;

        if (wantDispatch && up.upscaler.needsRecreate(up.plan, up.settings, colourIsHdr(proxy))) {
            const Result created = up.upscaler.create(ffx_, realDevice_, up.plan, up.settings,
                                                      colourIsHdr(proxy));
            if (!created) {
                upscaleFailure_ = created.reason();
                Logger::instance().failureBlock("FSR upscaler", created.reason(),
                                                "the FSR upscaling context",
                                                "could not be created",
                                                "presenting the rendered frame stretched "
                                                "instead");
            }
        }

        if (!upscaleRing_.isValid()) {
            const Result ring = upscaleRing_.create(realDevice_, gameQueue_);
            if (!ring) BFSR_LOG_WARN("upscaler command ring: %s", ring.reason().c_str());
        }
        auto* commandList = upscaleRing_.begin();
        if (!commandList) {
            // The previous frame's output stays on screen for one frame. The
            // release is not optional: DXGI refuses ResizeBuffers while any
            // reference to a back buffer is outstanding, so leaking one here
            // would break every resolution change from this point on.
            target->Release();
            return;
        }

        const bool dispatching = wantDispatch && up.upscaler.isValid();

        // FSR does not write the backbuffer directly. It declares its output as
        // an unordered-access resource, and a swapchain's buffers only support
        // that if the swapchain was created with DXGI_USAGE_UNORDERED_ACCESS —
        // which an engine that only renders into them never asks for. So FSR
        // writes the bridge's own display-resolution texture and the result is
        // copied across, which asks nothing of the swapchain.
        //
        // The blit needs no intermediate: every swapchain buffer supports a
        // render-target view, so it draws into the backbuffer directly.
        ID3D12Resource* const fsrTarget = proxy->upscaleTarget();
        if (dispatching && !fsrTarget) {
            upscaleRing_.submit();
            target->Release();
            return;
        }

        bool upscaled = false;
        if (dispatching) {
            UpscaleFrameInputs inputs;
            inputs.commandList = commandList;
            inputs.color = source;
            inputs.depth = depthResource_;
            inputs.motionVectors = motionResource_;
            inputs.output = fsrTarget;
            inputs.renderSize = render;
            inputs.upscaleSize = present;
            // Zero, and outside AMD's designed operating conditions: BeamNG
            // exposes no projection-jitter hook, so the temporal upscaler
            // cannot recover sub-pixel detail. LIMITATIONS.md section 1 — this
            // is the headline limitation and the UI states it whenever
            // upscaling is on.
            inputs.jitterX = 0.0f;
            inputs.jitterY = 0.0f;
            inputs.motionVectorScaleX = up.settings.calibration.motionVectorScaleX;
            inputs.motionVectorScaleY = up.settings.calibration.motionVectorScaleY;
            inputs.frameTimeDeltaMs = frameTimeMs > 0.0f ? frameTimeMs : 16.6f;
            inputs.reset = upscaleNeedsReset_;
            if (shared_.cameraValid) {
                inputs.cameraNear = shared_.cameraNear;
                inputs.cameraFar = shared_.cameraFar;
                inputs.cameraFovVerticalRadians = shared_.cameraFovVerticalRadians;
            }
            inputs.viewSpaceToMetersFactor = 1.0f; // BeamNG's world unit is the metre

            const Result dispatched = up.upscaler.dispatch(inputs, up.settings);
            upscaled = dispatched.isOk();
            if (!dispatched && dispatched.reason() != upscaleFailure_) {
                upscaleFailure_ = dispatched.reason();
                BFSR_LOG_WARN("FSR upscale dispatch: %s", upscaleFailure_.c_str());
            } else if (dispatched) {
                upscaleFailure_.clear();
            }
            upscaleNeedsReset_ = false;
        }

        if (upscaled) {
            // FSR left its own texture in the unordered-access state it was
            // handed in. Copy it over the backbuffer and put both back.
            transition(commandList, fsrTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(commandList, target, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            // Whole-resource copy: same dimensions, and the formats share a
            // typeless family even when the backbuffer is sRGB.
            commandList->CopyResource(target, fsrTarget);
            transition(commandList, target, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
            transition(commandList, fsrTarget, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else {
            // FSR either was not asked to run or failed. The frame still has to
            // reach the screen, so it is stretched instead.
            transition(commandList, source, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(commandList, target, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            const Result blitted = blit_.record(commandList, source, target, present.width,
                                                present.height);
            if (!blitted && blitted.reason() != lastBlitFailure_) {
                lastBlitFailure_ = blitted.reason();
                BFSR_LOG_WARN("fallback present blit: %s", lastBlitFailure_.c_str());
            }
            // Hand the substitute back exactly as the game left it, or its next
            // barrier from PRESENT would be starting from the wrong state.
            transition(commandList, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_PRESENT);
            transition(commandList, target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PRESENT);
            // The upscaler's history is stale once a frame has bypassed it.
            upscaleNeedsReset_ = true;
        }

        upscaleRing_.submit();
        target->Release();
    }

    /// Records one resource barrier. FSR's backend transitions the resources it
    /// is *given*; these are the ones the bridge owns and must move itself.
    static void transition(ID3D12GraphicsCommandList* commandList,
                           ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES from,
                           D3D12_RESOURCE_STATES to) {
        if (from == to) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = from;
        barrier.Transition.StateAfter = to;
        commandList->ResourceBarrier(1, &barrier);
    }

    /// Whether the swapchain's format carries high dynamic range.
    ///
    /// Read from the format rather than from a setting: FSR needs to know how
    /// to interpret the colour it is handed, and the swapchain is the authority
    /// on that.
    [[nodiscard]] static bool colourIsHdr(const SwapChainProxy* proxy) {
        switch (proxy->format()) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return true;
            default:
                return false;
        }
    }

    void driveFrameGeneration(SwapChainProxy* proxy, float frameTimeMs) {
        FrameGenModule& fg = *frameGen_;

        // The *real* swapchain's description, deliberately: the proxy reports
        // the render resolution to the game under substitution, and frame
        // generation interpolates the frames that are presented.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(proxy->real()->GetDesc1(&desc)) || desc.Width == 0 || desc.Height == 0) return;
        const Extent2D display{desc.Width, desc.Height};

        const bool resolutionChanged = displaySize_.valid() && displaySize_ != display;
        displaySize_ = display;

        RuntimeContext ctx = buildRuntimeContext(display);
        // BUG worth stating: with upscaling substituting the backbuffers, the
        // beacons are at the render resolution, not the display resolution.
        // Telling FSR the inputs are display-sized would have it sample motion
        // vectors over the wrong area of the texture.
        if (proxy->substituting()) {
            ctx.sceneRenderSize = Extent2D{proxy->renderWidth(), proxy->renderHeight()};
        }
        fg.plan = resolveFrameGenPlan(fg.settings, capabilities_, ctx);

        if (!fg.plan.active()) {
            if (fg.generator.hasContext()) {
                BFSR_LOG_INFO("frame generation is no longer planned; releasing its context");
                fg.generator.destroyContext();
                ring_.destroy();
                fg.gate.reset();
            }
            return;
        }

        if (!fg.generator.hasWrappedSwapChain()) {
            // Planned but unreachable: AMD's interpolation swapchain is what
            // actually presents generated frames, and it is created once, when
            // the game creates its swapchain. There is no way to attach later.
            if (!reportedNoSwapChain_) {
                reportedNoSwapChain_ = true;
                BFSR_LOG_WARN("frame generation is enabled but no frame-generation swapchain "
                              "was created: %s",
                              frameGenSwapChainFailure_.empty() ? "reason not recorded"
                                                                : frameGenSwapChainFailure_.c_str());
            }
            return;
        }

        if (fg.generator.needsRecreate(fg.plan, fg.settings, desc.Format)) {
            const Result created =
                fg.generator.createContext(ffx_, realDevice_, fg.plan, fg.settings, desc.Format);
            if (!created) {
                Logger::instance().failureBlock("frame-generation context", created.reason(),
                                                "the FSR frame-generation context",
                                                "could not be created",
                                                "frame generation disabled for this session");
                fg.settings.enabled = false;
                fg.store->save(fg.settings);
                return;
            }
            fg.gate.reset();

            if (!ring_.isValid()) {
                const Result ring = ring_.create(realDevice_, gameQueue_);
                if (!ring) {
                    BFSR_LOG_WARN("could not create FSR's command ring: %s",
                                  ring.reason().c_str());
                }
            }

            uint64_t bytes = 0;
            if (FsrFrameGenerator::queryMemoryUsage(ffx_, realDevice_, display, display,
                                                    fg.generator.createFlags(),
                                                    0, desc.Format, bytes)) {
                fg.vramBytes = bytes;
            }
        }

        FrameGenGateInput gate;
        gate.paused = shared_.paused;
        gate.loading = shared_.loading;
        gate.uiFocused = shared_.uiFocused;
        gate.photoMode = shared_.photoMode;
        gate.replay = shared_.replay;
        gate.cameraCutThisFrame = shared_.cameraCut;
        gate.windowOccluded = windowOccluded_;
        gate.deviceLost = realDevice_ && realDevice_->GetDeviceRemovedReason() != S_OK;
        gate.inputsAvailable = ctx.frameInputsAvailable && ring_.isValid();
        gate.enabledByPlan = true;
        gate.resolutionChangedThisFrame = resolutionChanged;
        gate.frameTimeMs = frameTimeMs > 0.0f ? frameTimeMs : 16.6f;
        const auto decision = fg.gate.evaluate(gate);

        // Anti-Lag is told about the frame regardless of whether this one is
        // interpolated: the driver paces against a continuous stream, and
        // reporting only some frames would describe a game running at half the
        // rate it is.
        if (fg.plan.antiLag2) {
            if (!fg.antiLag.isActive()) fg.antiLag.initialise(realDevice_);
            fg.antiLag.update(true, fg.plan.antiLagMaxFps);
        } else if (fg.antiLag.isActive()) {
            fg.antiLag.update(false, 0);
        }

        bool prepared = false;
        if (decision.allowInterpolation && depthResource_ && motionResource_ && ring_.isValid()) {
            if (auto* commandList = ring_.begin()) {
                FrameGenPrepareInputs inputs;
                inputs.commandList = commandList;
                inputs.depth = depthResource_;
                inputs.motionVectors = motionResource_;
                inputs.renderSize = ctx.sceneRenderSize;
                // Zero, and correct: BeamNG renders unjittered, and zero is the
                // coherent description of an unjittered frame. See
                // docs/LIMITATIONS.md §1.
                inputs.jitterX = 0.0f;
                inputs.jitterY = 0.0f;
                inputs.motionVectorScaleX = fg.settings.calibration.motionVectorScaleX;
                inputs.motionVectorScaleY = fg.settings.calibration.motionVectorScaleY;
                inputs.frameTimeDeltaMs = gate.frameTimeMs;
                inputs.reset = decision.requestReset;
                if (shared_.cameraValid) {
                    inputs.cameraNear = shared_.cameraNear;
                    inputs.cameraFar = shared_.cameraFar;
                    inputs.cameraFovVerticalRadians = shared_.cameraFovVerticalRadians;
                }
                // BeamNG's world unit is the metre, so no conversion applies.
                inputs.viewSpaceToMetersFactor = 1.0f;
                // cameraBasisValid stays false: Lua can read the camera's
                // transform but the protocol does not carry it yet, and an
                // identity basis would describe motion the camera is not
                // making. FSR omits the fields rather than receiving a lie.

                const Result dispatched = fg.generator.prepare(inputs, fg.settings, frameId_);
                const Result submitted = ring_.submit();
                prepared = dispatched.isOk() && submitted.isOk();
                if (!dispatched && dispatched.reason() != lastPrepareFailure_) {
                    lastPrepareFailure_ = dispatched.reason();
                    BFSR_LOG_WARN("frame-generation prepare: %s", lastPrepareFailure_.c_str());
                }
            }
        }

        // Configure runs every frame, including the frames that are suppressed:
        // it is what tells AMD's swapchain not to interpolate this one, and
        // skipping it would leave the previous frame's decision in force.
        fg.generator.configure(prepared, fg.settings, frameId_);
        if (fg.antiLag.isActive()) fg.antiLag.markEndOfFrameRendering();
    }

    /// Rendered rate multiplied by what AMD's swapchain reported generating.
    ///
    /// Derived rather than measured, and labelled as such in the UI: the
    /// bridge's Present hook runs once per *rendered* frame, so it cannot time
    /// the generated ones directly.
    [[nodiscard]] float computeDisplayedFps() const {
        const float rendered = renderedFrameTimes_.averageFps();
        if (!frameGen_ || rendered <= 0.0f) return rendered;
        const uint32_t generated = frameGen_->generator.observedGeneratedPerFrame();
        return rendered * static_cast<float>(1 + generated);
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
    SwapChainProxy* swapChainProxy_ = nullptr;

    // --- per-frame driver state --------------------------------------------
    FrameCommandRing ring_;
    FrameCommandRing upscaleRing_;
    PresentBlit blit_;
    bool upscaleNeedsReset_ = true;
    std::string upscaleFailure_;
    std::string lastBlitFailure_;
    SharedFrameState shared_;
    bool haveSharedState_ = false;
    std::chrono::steady_clock::time_point lastPresent_{};
    std::chrono::steady_clock::time_point lastRequestPoll_{};
    std::chrono::steady_clock::time_point lastStatusPublish_{};
    bool haveLastPresent_ = false;
    bool windowOccluded_ = false;
    uint64_t frameId_ = 0;
    Extent2D displaySize_;

    ID3D12Resource* depthResource_ = nullptr;
    ID3D12Resource* motionResource_ = nullptr;
    uint64_t beaconCacheGeneration_ = 0;
    uint32_t cachedWidth_ = 0;
    uint32_t cachedHeight_ = 0;
    bool cachedSignatureValid_ = false;
    bool beaconCacheValid_ = false;

    std::string frameGenSwapChainFailure_;
    bool reportedNoSwapChain_ = false;
    std::string lastPrepareFailure_;
    std::string lastLoggedDepthSource_;
    std::string lastFrameGenIpcDiagnostic_;
    std::string lastUpscaleIpcDiagnostic_;

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
    // Substitution happens inside the wrapped factory instead — see
    // BridgeRuntime::handleSwapChainCreated. It has to: AMD's
    // frame-interpolation swapchain is created from the same call that created
    // the game's, and the per-frame proxy has to wrap AMD's result rather than
    // the raw DXGI object. By the time a stub could call this, the object the
    // game will present through has already been decided.
    //
    // The export is kept because the dxgi stub resolves it by name and an
    // absent export would make the stub fail to attach. It is a deliberate
    // no-op, not an unimplemented one.
    return swapChain;
}

BFSR_API void bfsrCoreOnSwapChainResize(void* swapChain) {
    try {
        bfsr::BridgeRuntime::instance().onSwapChainResize(swapChain);
    } catch (...) {
    }
}

} // extern "C"
