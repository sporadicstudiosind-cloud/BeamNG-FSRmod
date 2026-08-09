// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// The d3d12.dll proxy stub. Companion to dxgi_proxy.cpp; see bridge_api.h for
// why both are needed. Same rules apply: forward everything, never be the
// reason the game fails to start.
#include "beamng_fsr/bridge_api.h"

#include <d3d12.h>

namespace {

HMODULE g_realD3D12 = nullptr;
HMODULE g_core = nullptr;
HMODULE g_selfModule = nullptr;

using PfnCoreInitialise = void (*)(HMODULE);
using PfnCoreShutdown = void (*)(void);
using PfnCoreWrapDevice = void* (*)(void*, const IID*);

PfnCoreWrapDevice g_wrapDevice = nullptr;

template <typename T>
T castProc(FARPROC proc) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(proc));
}

HMODULE loadRealD3D12() {
    if (g_realD3D12) return g_realD3D12;
    wchar_t path[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len > MAX_PATH - 16) return nullptr;
    wcscat_s(path, MAX_PATH, L"\\d3d12.dll");
    g_realD3D12 = LoadLibraryW(path);
    return g_realD3D12;
}

template <typename T>
T realProc(const char* name) {
    HMODULE m = loadRealD3D12();
    return m ? castProc<T>(GetProcAddress(m, name)) : nullptr;
}

void loadCore() {
    if (g_core) return;
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(g_selfModule, path, MAX_PATH) == 0) return;
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (!lastSlash) return;
    lastSlash[1] = L'\0';
    wcscat_s(path, MAX_PATH, L"beamng_fsr_bridge.dll");

    // If the dxgi stub already loaded the core, this just bumps its refcount
    // and returns the same module — which is exactly the shared state the two
    // stubs need.
    g_core = LoadLibraryW(path);
    if (!g_core) return;

    auto init = castProc<PfnCoreInitialise>(GetProcAddress(g_core, "bfsrCoreInitialise"));
    g_wrapDevice = castProc<PfnCoreWrapDevice>(GetProcAddress(g_core, "bfsrCoreWrapDevice"));
    if (init) init(g_selfModule);
}

} // namespace

extern "C" {

HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel,
                                 REFIID riid, void** ppDevice) {
    using Pfn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto fn = realProc<Pfn>("D3D12CreateDevice");
    if (!fn) return E_FAIL;

    const HRESULT hr = fn(adapter, minimumFeatureLevel, riid, ppDevice);

    // A null ppDevice means "check support only" — there is nothing to wrap.
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        loadCore();
        if (g_wrapDevice) {
            void* replacement = g_wrapDevice(*ppDevice, &riid);
            if (replacement) *ppDevice = replacement;
        }
    }
    return hr;
}

HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** ppvDebug) {
    using Pfn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = realProc<Pfn>("D3D12GetDebugInterface");
    return fn ? fn(riid, ppvDebug) : E_NOINTERFACE;
}

HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                           D3D_ROOT_SIGNATURE_VERSION version,
                                           ID3DBlob** blob, ID3DBlob** errorBlob) {
    using Pfn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                 ID3DBlob**, ID3DBlob**);
    auto fn = realProc<Pfn>("D3D12SerializeRootSignature");
    return fn ? fn(desc, version, blob, errorBlob) : E_NOTIMPL;
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc, ID3DBlob** blob, ID3DBlob** errorBlob) {
    using Pfn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    auto fn = realProc<Pfn>("D3D12SerializeVersionedRootSignature");
    return fn ? fn(desc, blob, errorBlob) : E_NOTIMPL;
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(LPCVOID data, SIZE_T size, REFIID riid,
                                                    void** ppDeserializer) {
    using Pfn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto fn = realProc<Pfn>("D3D12CreateRootSignatureDeserializer");
    return fn ? fn(data, size, riid, ppDeserializer) : E_NOTIMPL;
}

HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(LPCVOID data, SIZE_T size,
                                                             REFIID riid, void** ppDeserializer) {
    using Pfn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto fn = realProc<Pfn>("D3D12CreateVersionedRootSignatureDeserializer");
    return fn ? fn(data, size, riid, ppDeserializer) : E_NOTIMPL;
}

HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT numFeatures, const IID* iids,
                                               void* configurations, UINT* sizes) {
    using Pfn = HRESULT(WINAPI*)(UINT, const IID*, void*, UINT*);
    auto fn = realProc<Pfn>("D3D12EnableExperimentalFeatures");
    return fn ? fn(numFeatures, iids, configurations, sizes) : E_NOTIMPL;
}

HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID riid, void** ppvDebug) {
    using Pfn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    auto fn = realProc<Pfn>("D3D12GetInterface");
    return fn ? fn(clsid, riid, ppvDebug) : E_NOINTERFACE;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_selfModule = instance;
            DisableThreadLibraryCalls(instance);
            break;
        case DLL_PROCESS_DETACH:
            if (g_core) {
                auto shutdown = castProc<PfnCoreShutdown>(GetProcAddress(g_core, "bfsrCoreShutdown"));
                if (shutdown) shutdown();
            }
            break;
        default:
            break;
    }
    return TRUE;
}
