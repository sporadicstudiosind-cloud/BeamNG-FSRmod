// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// The dxgi.dll proxy stub.
//
// Placed beside BeamNG.exe, Windows loads this instead of the system dxgi.dll
// because the executable's directory precedes the system directory in the
// default DLL search order. Every export forwards to the genuine system DLL, so
// behaviour is unchanged except that the core gets a chance to wrap the
// swapchain.
//
// This file must be paranoid. It runs before anything else in the process is
// ready, and if it misbehaves BeamNG does not start at all. Every failure path
// therefore degrades to plain forwarding.
#include "beamng_fsr/bridge_api.h"

#include <dxgi1_6.h>

namespace {

HMODULE g_realDxgi = nullptr;
HMODULE g_core = nullptr;
HMODULE g_selfModule = nullptr;

using PfnCoreInitialise = void (*)(HMODULE);
using PfnCoreShutdown = void (*)(void);
using PfnCoreWrapFactory = void* (*)(void*, const IID*);
using PfnCoreWrapSwapChain = void* (*)(void*, void*, const IID*);
using PfnCoreOnSwapChainResize = void (*)(void*);

PfnCoreWrapFactory g_wrapFactory = nullptr;
PfnCoreWrapSwapChain g_wrapSwapChain = nullptr;
PfnCoreOnSwapChainResize g_onResize = nullptr;

/// Loads the genuine dxgi.dll from the system directory.
///
/// An explicit system path is essential: LoadLibrary("dxgi.dll") from inside a
/// module *named* dxgi.dll returns this module and the process deadlocks or
/// recurses.
HMODULE loadRealDxgi() {
    if (g_realDxgi) return g_realDxgi;
    wchar_t path[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len > MAX_PATH - 16) return nullptr;
    wcscat_s(path, MAX_PATH, L"\\dxgi.dll");
    g_realDxgi = LoadLibraryW(path);
    return g_realDxgi;
}

/// GetProcAddress returns FARPROC, so every use is a cast between incompatible
/// function types. Routing them through one helper documents that this is
/// deliberate and keeps -Wcast-function-type quiet at the single legitimate site
/// rather than suppressing it file-wide.
template <typename T>
T castProc(FARPROC proc) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(proc));
}

template <typename T>
T realProc(const char* name) {
    HMODULE m = loadRealDxgi();
    return m ? castProc<T>(GetProcAddress(m, name)) : nullptr;
}

void loadCore() {
    if (g_core) return;
    // The core sits next to this stub, not in the search path, so resolve an
    // absolute path from our own module handle.
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(g_selfModule, path, MAX_PATH) == 0) return;
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (!lastSlash) return;
    lastSlash[1] = L'\0';
    wcscat_s(path, MAX_PATH, L"beamng_fsr_bridge.dll");

    g_core = LoadLibraryW(path);
    if (!g_core) return;

    auto init = castProc<PfnCoreInitialise>(GetProcAddress(g_core, "bfsrCoreInitialise"));
    g_wrapFactory = castProc<PfnCoreWrapFactory>(GetProcAddress(g_core, "bfsrCoreWrapFactory"));
    g_wrapSwapChain =
        castProc<PfnCoreWrapSwapChain>(GetProcAddress(g_core, "bfsrCoreWrapSwapChain"));
    g_onResize =
        castProc<PfnCoreOnSwapChainResize>(GetProcAddress(g_core, "bfsrCoreOnSwapChainResize"));
    if (init) init(g_selfModule);
}

void* offerSwapChain(void* swapChain, void* queue, const IID* riid) {
    if (!swapChain) return swapChain;
    loadCore();
    if (!g_wrapSwapChain) return swapChain;
    void* replacement = g_wrapSwapChain(swapChain, queue, riid);
    return replacement ? replacement : swapChain;
}

/// Substitutes a wrapped factory for the real one so the core can see the
/// swapchain the game is about to create. Any failure leaves the game holding
/// the genuine factory.
void offerFactory(HRESULT hr, REFIID riid, void** ppFactory) {
    if (FAILED(hr) || !ppFactory || !*ppFactory) return;
    loadCore();
    if (!g_wrapFactory) return;
    void* replacement = g_wrapFactory(*ppFactory, &riid);
    if (replacement) *ppFactory = replacement;
}

} // namespace

extern "C" {

// --- factory creation ------------------------------------------------------
//
// Each successfully created factory is offered to the core, which substitutes a
// pass-through wrapper. That wrapper is what observes swapchain creation — the
// single point where AMD's frame-interpolation swapchain can take over
// presentation.

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    using Pfn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = realProc<Pfn>("CreateDXGIFactory");
    if (!fn) return E_FAIL;
    loadCore();
    const HRESULT hr = fn(riid, ppFactory);
    offerFactory(hr, riid, ppFactory);
    return hr;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    using Pfn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = realProc<Pfn>("CreateDXGIFactory1");
    if (!fn) return E_FAIL;
    loadCore();
    const HRESULT hr = fn(riid, ppFactory);
    offerFactory(hr, riid, ppFactory);
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
    using Pfn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto fn = realProc<Pfn>("CreateDXGIFactory2");
    if (!fn) return E_FAIL;
    loadCore();
    const HRESULT hr = fn(flags, riid, ppFactory);
    offerFactory(hr, riid, ppFactory);
    return hr;
}

// --- remaining documented exports, forwarded verbatim ----------------------

HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** pDebug) {
    using Pfn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto fn = realProc<Pfn>("DXGIGetDebugInterface1");
    return fn ? fn(flags, riid, pDebug) : E_NOINTERFACE;
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    using Pfn = HRESULT(WINAPI*)();
    auto fn = realProc<Pfn>("DXGIDeclareAdapterRemovalSupport");
    return fn ? fn() : E_NOTIMPL;
}

// Undocumented but exported ordinals some runtimes resolve by name. Forwarding
// them keeps behaviour identical to the system DLL; implementing them is not
// our business.
HRESULT WINAPI DXGID3D10CreateDevice(HMODULE mod, void* factory, void* adapter,
                                     UINT flags, void* unknown, void** device) {
    using Pfn = HRESULT(WINAPI*)(HMODULE, void*, void*, UINT, void*, void**);
    auto fn = realProc<Pfn>("DXGID3D10CreateDevice");
    return fn ? fn(mod, factory, adapter, flags, unknown, device) : E_NOTIMPL;
}

HRESULT WINAPI DXGID3D10CreateLayeredDevice(void* a, void* b, void* c, void* d, void* e) {
    using Pfn = HRESULT(WINAPI*)(void*, void*, void*, void*, void*);
    auto fn = realProc<Pfn>("DXGID3D10CreateLayeredDevice");
    return fn ? fn(a, b, c, d, e) : E_NOTIMPL;
}

SIZE_T WINAPI DXGID3D10GetLayeredDeviceSize(const void* layers, UINT count) {
    using Pfn = SIZE_T(WINAPI*)(const void*, UINT);
    auto fn = realProc<Pfn>("DXGID3D10GetLayeredDeviceSize");
    return fn ? fn(layers, count) : 0;
}

HRESULT WINAPI DXGID3D10RegisterLayers(const void* layers, UINT count) {
    using Pfn = HRESULT(WINAPI*)(const void*, UINT);
    auto fn = realProc<Pfn>("DXGID3D10RegisterLayers");
    return fn ? fn(layers, count) : E_NOTIMPL;
}

/// Entry point the core calls back into when it has wrapped a swapchain
/// created through a factory it observed. Exported so the core does not have
/// to duplicate the "which stub am I" logic.
__declspec(dllexport) void* WINAPI bfsrStubOfferSwapChain(void* swapChain, void* queue,
                                                          const IID* riid) {
    return offerSwapChain(swapChain, queue, riid);
}

__declspec(dllexport) void WINAPI bfsrStubOnSwapChainResize(void* swapChain) {
    if (g_onResize) g_onResize(swapChain);
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_selfModule = instance;
            // No loader-lock-unsafe work here: LoadLibrary during
            // DLL_PROCESS_ATTACH is a documented deadlock hazard. The real DLL
            // and the core are loaded lazily on the first export call instead.
            DisableThreadLibraryCalls(instance);
            break;
        case DLL_PROCESS_DETACH: {
            if (g_core) {
                auto shutdown = castProc<PfnCoreShutdown>(GetProcAddress(g_core, "bfsrCoreShutdown"));
                if (shutdown) shutdown();
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}
