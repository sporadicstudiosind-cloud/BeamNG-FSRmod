// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <dxgi1_6.h>

#include <atomic>

namespace bfsr {

/// Called with a freshly created swapchain and the device/queue it was made
/// against. Returns a replacement object, or null to leave the original in
/// place. Never takes ownership on the null path.
using SwapChainWrapHook = IUnknown* (*)(IUnknown* swapChain, IUnknown* deviceOrQueue, void* context);

/// Pass-through IDXGIFactory7 that offers every created swapchain to a hook.
/// See factory_proxy.cpp for why the whole interface chain is implemented.
class DxgiFactoryProxy final : public IDXGIFactory7 {
public:
    DxgiFactoryProxy(IDXGIFactory7* real, SwapChainWrapHook hook, void* hookContext);

    [[nodiscard]] IDXGIFactory7* real() const noexcept { return real_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* obj) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override;

    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter** adapter) override;
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND hwnd, UINT flags) override;
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* hwnd) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE module, IDXGIAdapter** adapter) override;

    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1** adapter) override;
    BOOL STDMETHODCALLTYPE IsCurrent() override;

    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) override;
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE resource, LUID* luid) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND hwnd, UINT msg, DWORD* cookie) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE evt, DWORD* cookie) override;
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD cookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND hwnd, UINT msg, DWORD* cookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE evt, DWORD* cookie) override;
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD cookie) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) override;

    UINT STDMETHODCALLTYPE GetCreationFlags() override;
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid, void** adapter) override;
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** adapter) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void* data, UINT size) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT index, DXGI_GPU_PREFERENCE preference, REFIID riid, void** adapter) override;
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE evt, DWORD* cookie) override;
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override;

private:
    ~DxgiFactoryProxy();
    IUnknown* maybeWrap(IUnknown* created, IUnknown* queue);

    std::atomic<long> refCount_{1};
    IDXGIFactory7* real_ = nullptr;
    SwapChainWrapHook hook_ = nullptr;
    void* hookContext_ = nullptr;
};

} // namespace bfsr
