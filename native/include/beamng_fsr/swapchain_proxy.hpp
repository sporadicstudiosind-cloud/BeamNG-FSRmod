// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// The per-frame tick.
//
// Everything else in this bridge is driven by an event that happens once —
// device creation, factory creation, swapchain creation. FSR needs something
// that happens every frame: the prepare dispatch has to be recorded after the
// scene is drawn and before the frame is presented, and frame generation has to
// be reconfigured with a new frame ID each time.
//
// BeamNG exposes no render-loop hook to mods (docs/FEASIBILITY.md §1.3), so the
// only per-frame event a DXGI-level layer can legitimately observe is Present.
// That is what this class provides: a pass-through IDXGISwapChain4 that calls a
// hook immediately before forwarding Present to the real swapchain.
//
// Present is also the correct point rather than merely the available one. By
// the time the game presents, the depth and velocity buffers for the frame are
// final and the beacon PostFX has already copied them, so the resources FSR
// needs are complete and no longer being written.
#pragma once

#include <dxgi1_5.h>

#include <atomic>

namespace bfsr {

/// Called on the game's thread immediately before each Present, with the
/// swapchain the game is presenting through. Must not throw.
using PresentHook = void (*)(IDXGISwapChain4* swapChain, UINT syncInterval, UINT flags,
                             void* context);

/// Called immediately before ResizeBuffers, so FSR resources sized against the
/// old surfaces can be released while they are still valid.
using ResizeHook = void (*)(IDXGISwapChain4* swapChain, void* context);

/// Pass-through IDXGISwapChain4 wrapping whatever the game was going to
/// present through.
///
/// When frame generation is active the object underneath is AMD's
/// frame-interpolation swapchain rather than DXGI's, and the composition order
/// matters: the game presents into this proxy, the hook records FSR's work for
/// the frame, and only then does AMD's swapchain see the Present and schedule
/// the interpolated frame. Hooking after AMD's swapchain instead would prepare
/// the frame that had already been presented.
class SwapChainProxy final : public IDXGISwapChain4 {
public:
    SwapChainProxy(IDXGISwapChain4* real, PresentHook present, ResizeHook resize, void* context);

    [[nodiscard]] IDXGISwapChain4* real() const noexcept { return real_; }

    /// Stops the hooks firing without unwrapping. Used during shutdown, where
    /// the runtime is being torn down but the game may still present.
    void detachHooks() noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* obj) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override;

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override;

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void** surface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT bufferCount, UINT width, UINT height,
                                            DXGI_FORMAT format, UINT flags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* params) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override;

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** window) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* colour) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* colour) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override;

    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT maxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* maxLatency) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override;

    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space,
                                                     UINT* support) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE space) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT bufferCount, UINT width, UINT height,
                                             DXGI_FORMAT format, UINT flags,
                                             const UINT* nodeMask,
                                             IUnknown* const* presentQueue) override;

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size,
                                             void* metadata) override;

private:
    ~SwapChainProxy();

    std::atomic<long> refCount_{1};
    IDXGISwapChain4* real_ = nullptr;
    std::atomic<PresentHook> present_{nullptr};
    std::atomic<ResizeHook> resize_{nullptr};
    void* context_ = nullptr;
};

} // namespace bfsr
