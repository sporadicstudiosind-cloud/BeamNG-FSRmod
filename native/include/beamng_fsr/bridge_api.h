// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// The C boundary between the two proxy stubs and the bridge core.
//
// Why two stubs: the D3D12 device is created by d3d12.dll's D3D12CreateDevice,
// while the swapchain is created by a DXGI factory from dxgi.dll. Intercepting
// both without touching the game's import table means providing a proxy for
// each — a documented Windows DLL-search-order mechanism that leaves every
// BeamNG file untouched. Both stubs load this one core DLL, so Windows'
// module refcounting gives them shared state for free.
//
// This header is deliberately C and deliberately narrow: the stubs must stay
// tiny and must never fail to load, so they know nothing about FSR, D3D12
// wrappers, or the runtime's state machine.
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BFSR_CORE_EXPORTS
#  define BFSR_API __declspec(dllexport)
#else
#  define BFSR_API __declspec(dllimport)
#endif

/// Called from each stub's DllMain(DLL_PROCESS_ATTACH). Idempotent.
/// `stubModule` is used to locate the bridge's own directory (config, log, the
/// AMD DLLs). Never throws; a failure here leaves the game running unmodified.
BFSR_API void bfsrCoreInitialise(HMODULE stubModule);

/// Called on process detach. Safe to call more than once.
BFSR_API void bfsrCoreShutdown(void);

/// Offers a freshly created D3D12 device for wrapping.
///
/// Returns the pointer the stub should hand back to the game: either a wrapper
/// or, if wrapping is declined or fails, `device` unchanged. The core takes no
/// ownership of `device` on the decline path.
BFSR_API void* bfsrCoreWrapDevice(void* device, const IID* riid);

/// Offers a freshly created DXGI factory for wrapping, so the core can observe
/// the swapchain the game creates from it. Same non-destructive contract.
BFSR_API void* bfsrCoreWrapFactory(void* factory, const IID* riid);

/// Offers a freshly created swapchain. Same contract as bfsrCoreWrapDevice:
/// returns what the stub should give the game, and never fails destructively.
/// `queue` is the ID3D12CommandQueue the swapchain was created against.
BFSR_API void* bfsrCoreWrapSwapChain(void* swapChain, void* queue, const IID* riid);

/// Notifies the core that the game is about to resize buffers, so frame
/// generation can be torn down before the swapchain's resources are released.
BFSR_API void bfsrCoreOnSwapChainResize(void* swapChain);

#ifdef __cplusplus
} // extern "C"
#endif
