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

#include <d3d12.h>
#include <dxgi1_5.h>

#include <atomic>
#include <vector>

#include "beamng_fsr/result.hpp"

namespace bfsr {

class SwapChainProxy;

/// Called on the game's thread immediately before each Present. Must not throw.
using PresentHook = void (*)(SwapChainProxy* proxy, UINT syncInterval, UINT flags, void* context);

/// Called immediately before ResizeBuffers, so FSR resources sized against the
/// old surfaces can be released while they are still valid.
using ResizeHook = void (*)(SwapChainProxy* proxy, void* context);

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

    // -----------------------------------------------------------------------
    // Backbuffer substitution — what makes upscaling possible.
    //
    // Upscaling means presenting more pixels than the game rendered, and a
    // DXGI-level layer has exactly one way to arrange that: let the real
    // swapchain stay at display resolution, hand the game textures of our own
    // at render resolution in place of its backbuffers, and report the smaller
    // size from GetDesc so the engine sizes its viewports, depth buffer and
    // render targets to match. At Present, FSR reads the substitute and writes
    // the real backbuffer.
    //
    // This is the highest-risk mechanism in the bridge and it is strictly
    // opt-in: with substitution off, every method here is a plain forward and
    // the proxy behaves exactly as it does for frame generation alone.
    //
    // One property makes it far safer than it looks: Direct3D 12 requires an
    // application to transition a backbuffer to PRESENT before presenting it.
    // The game believes the substitutes are backbuffers, so it does — which
    // means their resource state at Present is *known*, not assumed. That is
    // the opposite of the beacon situation (LIMITATIONS.md section 5).
    // -----------------------------------------------------------------------

    /// `ratio` is AMD's per-dimension divisor: render = display / ratio.
    /// Returns a failure and changes nothing if the substitutes cannot be
    /// created, leaving the proxy a pass-through.
    Result enableSubstitution(ID3D12Device* device, float ratio);
    void disableSubstitution();

    [[nodiscard]] bool substituting() const noexcept { return !substitutes_.empty(); }
    /// The texture the game most recently rendered into — FSR's colour input.
    [[nodiscard]] ID3D12Resource* currentSubstitute() const noexcept;
    /// The real swapchain's current backbuffer — FSR's output. Returns a
    /// reference the caller must release.
    [[nodiscard]] ID3D12Resource* acquireRealBackBuffer() const;

    [[nodiscard]] uint32_t renderWidth() const noexcept { return renderWidth_; }
    [[nodiscard]] uint32_t renderHeight() const noexcept { return renderHeight_; }
    [[nodiscard]] uint32_t presentWidth() const noexcept { return presentWidth_; }
    [[nodiscard]] uint32_t presentHeight() const noexcept { return presentHeight_; }
    [[nodiscard]] DXGI_FORMAT format() const noexcept { return format_; }

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

    Result createSubstitutes();
    /// Rewrites a resize request from the game's coordinates to the real
    /// swapchain's. See the definition for the rule and why it is ambiguous.
    HRESULT resizeCommon(UINT& width, UINT& height);
    void releaseSubstitutes();
    /// Re-reads the real swapchain's size and recomputes the render size from
    /// the stored ratio.
    void refreshSizes();

    std::atomic<long> refCount_{1};
    IDXGISwapChain4* real_ = nullptr;
    std::atomic<PresentHook> present_{nullptr};
    std::atomic<ResizeHook> resize_{nullptr};
    void* context_ = nullptr;

    ID3D12Device* device_ = nullptr;
    std::vector<ID3D12Resource*> substitutes_;
    uint32_t substituteIndex_ = 0;
    float ratio_ = 1.0f;
    uint32_t renderWidth_ = 0;
    uint32_t renderHeight_ = 0;
    uint32_t presentWidth_ = 0;
    uint32_t presentHeight_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

} // namespace bfsr
