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

#include "beamng_fsr/logging.hpp"

#include <algorithm>

namespace bfsr {
namespace {

/// Render resolution from a display resolution and AMD's per-dimension ratio,
/// clamped so a rounding error can never produce a zero-sized texture.
uint32_t scaled(uint32_t value, float ratio) {
    if (ratio <= 1.0f) return value;
    const auto out = static_cast<uint32_t>(static_cast<float>(value) / ratio + 0.5f);
    return std::max<uint32_t>(out, 1u);
}

} // namespace

SwapChainProxy::SwapChainProxy(IDXGISwapChain4* real, PresentHook present, ResizeHook resize,
                               void* context)
    : real_(real), context_(context) {
    present_.store(present, std::memory_order_relaxed);
    resize_.store(resize, std::memory_order_relaxed);
    if (real_) real_->AddRef();
}

SwapChainProxy::~SwapChainProxy() {
    // The substitutes and the device reference are owned here, not by the
    // runtime: the game holds the last reference to this proxy, so this is the
    // only place their lifetime reliably ends.
    releaseSubstitutes();
    if (device_) device_->Release();
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
        hook(this, syncInterval, flags, context_);
    }
    const HRESULT hr = real_->Present(syncInterval, flags);
    // Only a real present rotates the buffer the game draws into next. A test
    // present displays nothing, so advancing on one would desynchronise the
    // game's idea of the current backbuffer from ours.
    if ((flags & DXGI_PRESENT_TEST) == 0 && substituting()) {
        substituteIndex_ = (substituteIndex_ + 1) % substitutes_.size();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::Present1(UINT syncInterval, UINT flags,
                                                   const DXGI_PRESENT_PARAMETERS* params) {
    if (auto hook = present_.load(std::memory_order_acquire)) {
        hook(this, syncInterval, flags, context_);
    }
    const HRESULT hr = real_->Present1(syncInterval, flags, params);
    if ((flags & DXGI_PRESENT_TEST) == 0 && substituting()) {
        substituteIndex_ = (substituteIndex_ + 1) % substitutes_.size();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetBuffer(UINT buffer, REFIID riid, void** surface) {
    if (!substituting()) return real_->GetBuffer(buffer, riid, surface);
    if (!surface) return E_POINTER;
    if (buffer >= substitutes_.size()) return DXGI_ERROR_INVALID_CALL;
    // The game asked for its backbuffer and gets one of ours, at the resolution
    // GetDesc reported. It will create render-target views against these and
    // keep them, which is why the substitutes live until a resize replaces
    // them rather than being recreated per frame.
    return substitutes_[buffer]->QueryInterface(riid, surface);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::SetFullscreenState(BOOL fullscreen,
                                                             IDXGIOutput* target) {
    // A fullscreen transition re-creates the swapchain's surfaces, exactly like
    // a resize does, so FSR's resources have to go the same way.
    if (auto hook = resize_.load(std::memory_order_acquire)) hook(this, context_);
    releaseSubstitutes();
    const HRESULT hr = real_->SetFullscreenState(fullscreen, target);
    if (SUCCEEDED(hr) && device_) {
        refreshSizes();
        if (!createSubstitutes()) {
            BFSR_LOG_WARN("substitute buffers could not be recreated after a fullscreen "
                          "transition; upscaling is off for the rest of this session");
            disableSubstitution();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetFullscreenState(BOOL* fullscreen,
                                                             IDXGIOutput** target) {
    return real_->GetFullscreenState(fullscreen, target);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetDesc(DXGI_SWAP_CHAIN_DESC* desc) {
    const HRESULT hr = real_->GetDesc(desc);
    // The engine sizes its viewports, depth buffer and render targets from
    // this. Under substitution it must read the *render* resolution, or it
    // would draw a display-resolution frame into a smaller texture.
    if (SUCCEEDED(hr) && desc && substituting()) {
        desc->BufferDesc.Width = renderWidth_;
        desc->BufferDesc.Height = renderHeight_;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) {
    const HRESULT hr = real_->GetDesc1(desc);
    if (SUCCEEDED(hr) && desc && substituting()) {
        desc->Width = renderWidth_;
        desc->Height = renderHeight_;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetSourceSize(UINT* width, UINT* height) {
    const HRESULT hr = real_->GetSourceSize(width, height);
    if (SUCCEEDED(hr) && substituting()) {
        if (width) *width = renderWidth_;
        if (height) *height = renderHeight_;
    }
    return hr;
}

/// Translates a resize request from the game's view of the world to the real
/// swapchain's.
///
/// The ambiguity this has to resolve: the game got its idea of the swapchain
/// size from GetDesc, so it may pass the *render* size back on a no-change
/// resize, or it may pass a genuinely new *display* size it read from the
/// window. Zeros mean "match the window", which DXGI resolves itself.
///
/// The rule: zeros pass straight through; a size equal to the render size is a
/// no-change resize and becomes the present size; anything else is a real
/// display size and is used as given. Both engine conventions land correctly.
HRESULT SwapChainProxy::resizeCommon(UINT& width, UINT& height) {
    if (!substituting()) return S_OK;
    if (width == 0 && height == 0) return S_OK;
    if (width == renderWidth_ && height == renderHeight_) {
        width = presentWidth_;
        height = presentHeight_;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::ResizeBuffers(UINT bufferCount, UINT width, UINT height,
                                                        DXGI_FORMAT format, UINT flags) {
    if (auto hook = resize_.load(std::memory_order_acquire)) hook(this, context_);
    // The substitutes must die before the resize: the game is about to release
    // its views of them, and the ones we create afterwards are a different size.
    const bool wasSubstituting = substituting();
    resizeCommon(width, height);
    releaseSubstitutes();

    const HRESULT hr = real_->ResizeBuffers(bufferCount, width, height, format, flags);
    if (wasSubstituting && SUCCEEDED(hr)) {
        refreshSizes();
        if (!createSubstitutes()) {
            BFSR_LOG_WARN("substitute buffers could not be recreated after a resize; upscaling "
                          "is off for the rest of this session");
            disableSubstitution();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::ResizeBuffers1(UINT bufferCount, UINT width, UINT height,
                                                         DXGI_FORMAT format, UINT flags,
                                                         const UINT* nodeMask,
                                                         IUnknown* const* presentQueue) {
    if (auto hook = resize_.load(std::memory_order_acquire)) hook(this, context_);
    const bool wasSubstituting = substituting();
    resizeCommon(width, height);
    releaseSubstitutes();

    const HRESULT hr =
        real_->ResizeBuffers1(bufferCount, width, height, format, flags, nodeMask, presentQueue);
    if (wasSubstituting && SUCCEEDED(hr)) {
        refreshSizes();
        if (!createSubstitutes()) {
            BFSR_LOG_WARN("substitute buffers could not be recreated after a resize; upscaling "
                          "is off for the rest of this session");
            disableSubstitution();
        }
    }
    return hr;
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
    // Under substitution the game's notion of "source" is one of our textures,
    // not a region of the real swapchain — and FSR always writes the real
    // backbuffer in full. Forwarding a render-resolution size would tell DXGI
    // to present only the top-left corner of a frame that fills it, so this is
    // accepted and dropped rather than passed on.
    if (substituting()) return S_OK;
    return real_->SetSourceSize(width, height);
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
    // Our own rotation, not the real swapchain's: the game uses this to pick
    // which buffer to render into, and it must name one of the substitutes.
    if (substituting()) return substituteIndex_;
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

// ---------------------------------------------------------------------------
// Backbuffer substitution
// ---------------------------------------------------------------------------

void SwapChainProxy::refreshSizes() {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(real_->GetDesc1(&desc))) return;
    presentWidth_ = desc.Width;
    presentHeight_ = desc.Height;
    format_ = desc.Format;
    renderWidth_ = scaled(presentWidth_, ratio_);
    renderHeight_ = scaled(presentHeight_, ratio_);
}

Result SwapChainProxy::createSubstitutes() {
    if (!device_ || presentWidth_ == 0 || presentHeight_ == 0) {
        return Result::fail(Status::NotInitialised, "the swapchain size is not known yet");
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(real_->GetDesc1(&desc)) || desc.BufferCount == 0) {
        return Result::fail(Status::RuntimeError, "the swapchain description is unreadable");
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = renderWidth_;
    texture.Height = renderHeight_;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = format_;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // RENDER_TARGET because the game will draw into these believing they are
    // backbuffers; UNORDERED_ACCESS because FSR may want to write them and
    // because the flag cannot be added after creation.
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = format_;

    substitutes_.reserve(desc.BufferCount);
    for (uint32_t i = 0; i < desc.BufferCount; ++i) {
        ID3D12Resource* resource = nullptr;
        // Created in PRESENT state, which is what a real backbuffer starts in.
        // The game's first barrier on it will be PRESENT -> RENDER_TARGET,
        // exactly as it would be for a genuine swapchain buffer, so no engine
        // assumption is broken by the substitution.
        const HRESULT hr = device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_PRESENT, &clear,
            __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
        if (FAILED(hr) || !resource) {
            releaseSubstitutes();
            return Result::fail(Status::RuntimeError,
                                "a substitute backbuffer could not be created");
        }
        substitutes_.push_back(resource);
    }

    substituteIndex_ = 0;
    BFSR_LOG_INFO("backbuffer substitution active: game renders %ux%u, presenting %ux%u "
                  "(%zu buffers)",
                  renderWidth_, renderHeight_, presentWidth_, presentHeight_,
                  substitutes_.size());
    return Result::ok();
}

void SwapChainProxy::releaseSubstitutes() {
    for (auto* resource : substitutes_) {
        if (resource) resource->Release();
    }
    substitutes_.clear();
    substituteIndex_ = 0;
}

Result SwapChainProxy::enableSubstitution(ID3D12Device* device, float ratio) {
    if (!device) return Result::fail(Status::InvalidArgument, "no Direct3D 12 device");
    if (substituting()) return Result::ok();
    if (ratio <= 1.0f) {
        return Result::fail(Status::InvalidArgument,
                            "an upscale ratio of 1.0 or less needs no substitution");
    }

    ID3D12Device* previousDevice = device_;
    device_ = device;
    device_->AddRef();
    ratio_ = ratio;
    refreshSizes();

    const Result created = createSubstitutes();
    if (!created) {
        // Leave the proxy exactly as it was: a pure pass-through. This is the
        // failure mode that matters most, because the alternative to a working
        // substitution is a black screen, not a soft image.
        device_->Release();
        device_ = previousDevice;
        ratio_ = 1.0f;
        return created;
    }
    if (previousDevice) previousDevice->Release();
    return Result::ok();
}

void SwapChainProxy::disableSubstitution() {
    releaseSubstitutes();
    ratio_ = 1.0f;
    renderWidth_ = presentWidth_;
    renderHeight_ = presentHeight_;
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
}

ID3D12Resource* SwapChainProxy::currentSubstitute() const noexcept {
    if (substitutes_.empty()) return nullptr;
    return substitutes_[substituteIndex_ % substitutes_.size()];
}

ID3D12Resource* SwapChainProxy::acquireRealBackBuffer() const {
    ID3D12Resource* buffer = nullptr;
    const UINT index = real_->GetCurrentBackBufferIndex();
    if (FAILED(real_->GetBuffer(index, __uuidof(ID3D12Resource),
                                reinterpret_cast<void**>(&buffer)))) {
        return nullptr;
    }
    return buffer;
}

} // namespace bfsr
