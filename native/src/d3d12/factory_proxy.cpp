// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// Pass-through IDXGIFactory7.
//
// This is the one interception the whole frame-generation feature rests on:
// AMD's frame-interpolation swapchain has to replace the game's swapchain
// object, and the only place to substitute it is where the game creates it.
//
// The full IDXGIFactory..IDXGIFactory7 chain is implemented rather than a
// convenient subset. A partial wrapper would hand the real factory back on
// QueryInterface for a newer version, and the game would then create its
// swapchain somewhere we cannot see — silently disabling frame generation with
// no diagnosable cause. The device wrapper can afford that trade-off (missing
// resources fails visibly and safely); this one cannot.
#include "beamng_fsr/factory_proxy.hpp"

#include "beamng_fsr/logging.hpp"

namespace bfsr {

DxgiFactoryProxy::DxgiFactoryProxy(IDXGIFactory7* real, SwapChainWrapHook hook, void* hookContext)
    : real_(real), hook_(hook), hookContext_(hookContext) {
    if (real_) real_->AddRef();
}

DxgiFactoryProxy::~DxgiFactoryProxy() {
    if (real_) real_->Release();
}

IUnknown* DxgiFactoryProxy::maybeWrap(IUnknown* created, IUnknown* queue) {
    if (!created || !hook_) return created;
    IUnknown* replacement = hook_(created, queue, hookContext_);
    return replacement ? replacement : created;
}

// --- IUnknown --------------------------------------------------------------

HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IDXGIFactory7) || riid == __uuidof(IDXGIFactory6) ||
        riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory4) ||
        riid == __uuidof(IDXGIFactory3) || riid == __uuidof(IDXGIFactory2) ||
        riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory) ||
        riid == __uuidof(IDXGIObject) || riid == __uuidof(IUnknown)) {
        *ppv = static_cast<IDXGIFactory7*>(this);
        AddRef();
        return S_OK;
    }
    return real_->QueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE DxgiFactoryProxy::AddRef() {
    return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
}

ULONG STDMETHODCALLTYPE DxgiFactoryProxy::Release() {
    const ULONG remaining =
        static_cast<ULONG>(refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1);
    if (remaining == 0) delete this;
    return remaining;
}

// --- IDXGIObject -----------------------------------------------------------

HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::SetPrivateData(REFGUID name, UINT size, const void* data) {
    return real_->SetPrivateData(name, size, data);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::SetPrivateDataInterface(REFGUID name, const IUnknown* obj) {
    return real_->SetPrivateDataInterface(name, obj);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::GetPrivateData(REFGUID name, UINT* size, void* data) {
    return real_->GetPrivateData(name, size, data);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::GetParent(REFIID riid, void** parent) {
    return real_->GetParent(riid, parent);
}

// --- IDXGIFactory ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::EnumAdapters(UINT index, IDXGIAdapter** adapter) {
    return real_->EnumAdapters(index, adapter);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::MakeWindowAssociation(HWND hwnd, UINT flags) {
    return real_->MakeWindowAssociation(hwnd, flags);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::GetWindowAssociation(HWND* hwnd) {
    return real_->GetWindowAssociation(hwnd);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::CreateSoftwareAdapter(HMODULE module, IDXGIAdapter** adapter) {
    return real_->CreateSoftwareAdapter(module, adapter);
}

HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::CreateSwapChain(IUnknown* device,
                                                            DXGI_SWAP_CHAIN_DESC* desc,
                                                            IDXGISwapChain** swapChain) {
    const HRESULT hr = real_->CreateSwapChain(device, desc, swapChain);
    if (SUCCEEDED(hr) && swapChain && *swapChain) {
        *swapChain = reinterpret_cast<IDXGISwapChain*>(maybeWrap(*swapChain, device));
    }
    return hr;
}

// --- IDXGIFactory1 ---------------------------------------------------------

HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::EnumAdapters1(UINT index, IDXGIAdapter1** adapter) {
    return real_->EnumAdapters1(index, adapter);
}
BOOL STDMETHODCALLTYPE DxgiFactoryProxy::IsCurrent() { return real_->IsCurrent(); }

// --- IDXGIFactory2 ---------------------------------------------------------

BOOL STDMETHODCALLTYPE DxgiFactoryProxy::IsWindowedStereoEnabled() {
    return real_->IsWindowedStereoEnabled();
}

HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::CreateSwapChainForHwnd(
    IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, IDXGIOutput* restrictToOutput,
    IDXGISwapChain1** swapChain) {
    // The path a D3D12 renderer actually uses.
    const HRESULT hr =
        real_->CreateSwapChainForHwnd(device, hwnd, desc, fullscreenDesc, restrictToOutput, swapChain);
    if (SUCCEEDED(hr) && swapChain && *swapChain) {
        BFSR_LOG_INFO("swapchain created: %ux%u, format %d, %u buffers, swap effect %d",
                      desc ? desc->Width : 0, desc ? desc->Height : 0,
                      desc ? static_cast<int>(desc->Format) : 0,
                      desc ? desc->BufferCount : 0,
                      desc ? static_cast<int>(desc->SwapEffect) : 0);
        *swapChain = reinterpret_cast<IDXGISwapChain1*>(maybeWrap(*swapChain, device));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::CreateSwapChainForCoreWindow(
    IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc,
    IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
    return real_->CreateSwapChainForCoreWindow(device, window, desc, restrictToOutput, swapChain);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::GetSharedResourceAdapterLuid(HANDLE resource, LUID* luid) {
    return real_->GetSharedResourceAdapterLuid(resource, luid);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::RegisterStereoStatusWindow(HWND hwnd, UINT msg, DWORD* cookie) {
    return real_->RegisterStereoStatusWindow(hwnd, msg, cookie);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::RegisterStereoStatusEvent(HANDLE evt, DWORD* cookie) {
    return real_->RegisterStereoStatusEvent(evt, cookie);
}
void STDMETHODCALLTYPE DxgiFactoryProxy::UnregisterStereoStatus(DWORD cookie) {
    real_->UnregisterStereoStatus(cookie);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::RegisterOcclusionStatusWindow(HWND hwnd, UINT msg, DWORD* cookie) {
    return real_->RegisterOcclusionStatusWindow(hwnd, msg, cookie);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::RegisterOcclusionStatusEvent(HANDLE evt, DWORD* cookie) {
    return real_->RegisterOcclusionStatusEvent(evt, cookie);
}
void STDMETHODCALLTYPE DxgiFactoryProxy::UnregisterOcclusionStatus(DWORD cookie) {
    real_->UnregisterOcclusionStatus(cookie);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::CreateSwapChainForComposition(
    IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput,
    IDXGISwapChain1** swapChain) {
    return real_->CreateSwapChainForComposition(device, desc, restrictToOutput, swapChain);
}

// --- IDXGIFactory3..7 ------------------------------------------------------

UINT STDMETHODCALLTYPE DxgiFactoryProxy::GetCreationFlags() { return real_->GetCreationFlags(); }

HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::EnumAdapterByLuid(LUID luid, REFIID riid, void** adapter) {
    return real_->EnumAdapterByLuid(luid, riid, adapter);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::EnumWarpAdapter(REFIID riid, void** adapter) {
    return real_->EnumWarpAdapter(riid, adapter);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::CheckFeatureSupport(DXGI_FEATURE feature, void* data, UINT size) {
    return real_->CheckFeatureSupport(feature, data, size);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::EnumAdapterByGpuPreference(
    UINT index, DXGI_GPU_PREFERENCE preference, REFIID riid, void** adapter) {
    return real_->EnumAdapterByGpuPreference(index, preference, riid, adapter);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::RegisterAdaptersChangedEvent(HANDLE evt, DWORD* cookie) {
    return real_->RegisterAdaptersChangedEvent(evt, cookie);
}
HRESULT STDMETHODCALLTYPE DxgiFactoryProxy::UnregisterAdaptersChangedEvent(DWORD cookie) {
    return real_->UnregisterAdaptersChangedEvent(cookie);
}

} // namespace bfsr
