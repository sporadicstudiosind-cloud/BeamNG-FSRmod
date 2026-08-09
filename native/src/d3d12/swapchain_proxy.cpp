// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// Every method here forwards. The only two that do anything first are Present
// and Present1 (the per-frame tick) and ResizeBuffers/ResizeBuffers1 (where FSR
// resources must be released before the surfaces they were sized against go
// away).
//
// The whole IDXGISwapChain..4 chain is implemented deliberately. A partial
// wrapper would fail QueryInterface for an interface the game asks for, the
// game would fall back to the unwrapped object, and Present would stop reaching
// the hook — silently, with frame generation simply never running and no error
// anywhere. The same reasoning is why factory_proxy.cpp implements the full
// IDXGIFactory chain.
#include "beamng_fsr/swapchain_proxy.hpp"

namespace bfsr {

SwapChainProxy::SwapChainProxy(IDXGISwapChain4* real, PresentHook present, ResizeHook resize,
                               void* context)
    : real_(real), context_(context) {
    present_.store(present, std::memory_order_relaxed);
    resize_.store(resize, std::memory_order_relaxed);
    if (real_) real_->AddRef();
}

SwapChainProxy::~SwapChainProxy() {
    if (real_) real_->Release();
}

void SwapChainProxy::detachHooks() noexcept {
    present_.store(nullptr, std::memory_order_release);
    resize_.store(nullptr, std::memory_order_release);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain) ||
        riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4)) {
        AddRef();
        *ppv = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    // Anything else — a private DXGI interface, a debug interface, AMD's own
    // swapchain interfaces — is served by the object underneath. Handing the
    // real pointer out means a caller holding that interface bypasses the
    // hook, which is correct: those interfaces are not Present.
    return real_ ? real_->QueryInterface(riid, ppv) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE SwapChainProxy::AddRef() {
    return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
}

ULONG STDMETHODCALLTYPE SwapChainProxy::Release() {
    const long remaining = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) delete this;
    return static_cast<ULONG>(remaining < 0 ? 0 : remaining);
}

// --- IDXGIObject -----------------------------------------------------------

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetPrivateData(REFGUID name, UINT size,
                                                         const void* data) {
    return real_->SetPrivateData(name, size, data);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetPrivateDataInterface(REFGUID name,
                                                                  const IUnknown* obj) {
    return real_->SetPrivateDataInterface(name, obj);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetPrivateData(REFGUID name, UINT* size, void* data) {
    return real_->GetPrivateData(name, size, data);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetParent(REFIID riid, void** parent) {
    return real_->GetParent(riid, parent);
}

// --- IDXGIDeviceSubObject --------------------------------------------------

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetDevice(REFIID riid, void** device) {
    return real_->GetDevice(riid, device);
}

// --- IDXGISwapChain --------------------------------------------------------

HRESULT STDMETHODCALLTYPE SwapChainProxy::Present(UINT syncInterval, UINT flags) {
    if (auto hook = present_.load(std::memory_order_acquire)) {
        hook(real_, syncInterval, flags, context_);
    }
    return real_->Present(syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::Present1(UINT syncInterval, UINT flags,
                                                   const DXGI_PRESENT_PARAMETERS* params) {
    if (auto hook = present_.load(std::memory_order_acquire)) {
        hook(real_, syncInterval, flags, context_);
    }
    return real_->Present1(syncInterval, flags, params);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetBuffer(UINT buffer, REFIID riid, void** surface) {
    return real_->GetBuffer(buffer, riid, surface);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetFullscreenState(BOOL fullscreen,
                                                             IDXGIOutput* target) {
    // A fullscreen transition re-creates the swapchain's surfaces, exactly like
    // a resize does, so FSR's resources have to go the same way.
    if (auto hook = resize_.load(std::memory_order_acquire)) hook(real_, context_);
    return real_->SetFullscreenState(fullscreen, target);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetFullscreenState(BOOL* fullscreen,
                                                             IDXGIOutput** target) {
    return real_->GetFullscreenState(fullscreen, target);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetDesc(DXGI_SWAP_CHAIN_DESC* desc) {
    return real_->GetDesc(desc);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::ResizeBuffers(UINT bufferCount, UINT width, UINT height,
                                                        DXGI_FORMAT format, UINT flags) {
    if (auto hook = resize_.load(std::memory_order_acquire)) hook(real_, context_);
    return real_->ResizeBuffers(bufferCount, width, height, format, flags);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::ResizeBuffers1(UINT bufferCount, UINT width, UINT height,
                                                         DXGI_FORMAT format, UINT flags,
                                                         const UINT* nodeMask,
                                                         IUnknown* const* presentQueue) {
    if (auto hook = resize_.load(std::memory_order_acquire)) hook(real_, context_);
    return real_->ResizeBuffers1(bufferCount, width, height, format, flags, nodeMask, presentQueue);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::ResizeTarget(const DXGI_MODE_DESC* params) {
    return real_->ResizeTarget(params);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetContainingOutput(IDXGIOutput** output) {
    return real_->GetContainingOutput(output);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) {
    return real_->GetFrameStatistics(stats);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetLastPresentCount(UINT* count) {
    return real_->GetLastPresentCount(count);
}

// --- IDXGISwapChain1 -------------------------------------------------------

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) {
    return real_->GetDesc1(desc);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetFullscreenDesc(
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) {
    return real_->GetFullscreenDesc(desc);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetHwnd(HWND* hwnd) { return real_->GetHwnd(hwnd); }

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetCoreWindow(REFIID riid, void** window) {
    return real_->GetCoreWindow(riid, window);
}

BOOL STDMETHODCALLTYPE SwapChainProxy::IsTemporaryMonoSupported() {
    return real_->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetRestrictToOutput(IDXGIOutput** output) {
    return real_->GetRestrictToOutput(output);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetBackgroundColor(const DXGI_RGBA* colour) {
    return real_->SetBackgroundColor(colour);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetBackgroundColor(DXGI_RGBA* colour) {
    return real_->GetBackgroundColor(colour);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetRotation(DXGI_MODE_ROTATION rotation) {
    return real_->SetRotation(rotation);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetRotation(DXGI_MODE_ROTATION* rotation) {
    return real_->GetRotation(rotation);
}

// --- IDXGISwapChain2 -------------------------------------------------------

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetSourceSize(UINT width, UINT height) {
    return real_->SetSourceSize(width, height);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetSourceSize(UINT* width, UINT* height) {
    return real_->GetSourceSize(width, height);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetMaximumFrameLatency(UINT maxLatency) {
    return real_->SetMaximumFrameLatency(maxLatency);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetMaximumFrameLatency(UINT* maxLatency) {
    return real_->GetMaximumFrameLatency(maxLatency);
}

HANDLE STDMETHODCALLTYPE SwapChainProxy::GetFrameLatencyWaitableObject() {
    return real_->GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) {
    return real_->SetMatrixTransform(matrix);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) {
    return real_->GetMatrixTransform(matrix);
}

// --- IDXGISwapChain3 -------------------------------------------------------

UINT STDMETHODCALLTYPE SwapChainProxy::GetCurrentBackBufferIndex() {
    return real_->GetCurrentBackBufferIndex();
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space,
                                                                 UINT* support) {
    return real_->CheckColorSpaceSupport(space, support);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetColorSpace1(DXGI_COLOR_SPACE_TYPE space) {
    return real_->SetColorSpace1(space);
}

// --- IDXGISwapChain4 -------------------------------------------------------

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size,
                                                         void* metadata) {
    return real_->SetHDRMetaData(type, size, metadata);
}

} // namespace bfsr
