// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/antilag2.hpp"

#include "beamng_fsr/logging.hpp"

// AMD's ffx_antilag2_dx12.h contains
//     static_cast<PFNAmdExtD3DCreateInterface>((VOID*)GetProcAddress(...))
// which MSVC accepts and GCC/Clang reject as ill-formed (a static_cast from
// void* to a function pointer is not a valid conversion in standard C++).
//
// The header is AMD redistributable source and is deliberately not patched
// here. Shipping builds use MSVC, where it compiles as written. The MinGW
// configuration exists to compile-verify the rest of the bridge on non-Windows
// hosts, and there Anti-Lag 2 degrades to "unavailable" — the same path taken
// on a non-AMD GPU, and reported to the user with the same honesty.
#if defined(_MSC_VER)
#  define BFSR_HAVE_ANTILAG2 1
#  include <ffx_antilag2_dx12.h>
#else
#  define BFSR_HAVE_ANTILAG2 0
#endif

namespace bfsr {
namespace {

#if BFSR_HAVE_ANTILAG2

AMD::AntiLag2DX12::Context* asContext(void* p) {
    return static_cast<AMD::AntiLag2DX12::Context*>(p);
}
#endif

} // namespace

AntiLag2::~AntiLag2() { shutdown(); }

Result AntiLag2::initialise(ID3D12Device* device) {
    shutdown();
    if (!device) return Result::fail(Status::InvalidArgument, "no Direct3D 12 device");
#if !BFSR_HAVE_ANTILAG2
    return Result::fail(Status::Unsupported,
                        "Anti-Lag 2 is not compiled into this build of the bridge. Use an "
                        "MSVC build to enable it.");
#else

    // AMD requires the context to be zeroed before Initialize and not touched
    // afterwards.
    auto* context = new AMD::AntiLag2DX12::Context{};
    const HRESULT hr = AMD::AntiLag2DX12::Initialize(context, device);
    if (FAILED(hr)) {
        delete context;
        return Result::fail(
            Status::Unsupported,
            "Anti-Lag 2 is not available: the AMD driver extension interface could not be "
            "created. This is expected on non-AMD GPUs and on AMD drivers without Anti-Lag 2.");
    }

    context_ = context;
    active_ = true;
    BFSR_LOG_INFO("AMD Radeon Anti-Lag 2 initialised");
    return Result::ok();
#endif
}

void AntiLag2::shutdown() {
#if BFSR_HAVE_ANTILAG2
    if (context_) {
        AMD::AntiLag2DX12::DeInitialize(asContext(context_));
        delete asContext(context_);
        context_ = nullptr;
    }
#endif
    active_ = false;
}

Result AntiLag2::update(bool enabled, unsigned maxFps) {
    if (!active_) return Result::fail(Status::NotInitialised, "Anti-Lag 2 is not active");
#if BFSR_HAVE_ANTILAG2
    const HRESULT hr = AMD::AntiLag2DX12::Update(asContext(context_), enabled, maxFps);
    return SUCCEEDED(hr) ? Result::ok()
                         : Result::fail(Status::RuntimeError, "Anti-Lag 2 update failed");
#else
    return Result::fail(Status::Unsupported, "Anti-Lag 2 is not compiled into this build");
#endif
}

Result AntiLag2::markEndOfFrameRendering() {
    if (!active_) return Result::fail(Status::NotInitialised, "Anti-Lag 2 is not active");
#if BFSR_HAVE_ANTILAG2
    const HRESULT hr = AMD::AntiLag2DX12::MarkEndOfFrameRendering(asContext(context_));
    return SUCCEEDED(hr) ? Result::ok()
                         : Result::fail(Status::RuntimeError, "Anti-Lag 2 end-of-frame mark failed");
#else
    return Result::fail(Status::Unsupported, "Anti-Lag 2 is not compiled into this build");
#endif
}

Result AntiLag2::setFrameGenFrameType(bool isInterpolated) {
    if (!active_) return Result::fail(Status::NotInitialised, "Anti-Lag 2 is not active");
#if BFSR_HAVE_ANTILAG2
    const HRESULT hr = AMD::AntiLag2DX12::SetFrameGenFrameType(asContext(context_), isInterpolated);
    return SUCCEEDED(hr) ? Result::ok()
                         : Result::fail(Status::RuntimeError, "Anti-Lag 2 frame-type hint failed");
#else
    return Result::fail(Status::Unsupported, "Anti-Lag 2 is not compiled into this build");
#endif
}

} // namespace bfsr
