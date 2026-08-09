// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// A fullscreen texture copy, used only as upscaling's fallback path.
//
// When backbuffer substitution is active the game renders into textures this
// bridge owns, and something has to put pixels into the real backbuffer before
// every Present. Normally that is the FSR upscale. When the upscale cannot run
// — the mod half has not reported its beacons yet, the plan went inactive, a
// dispatch failed — the frame still has to reach the screen, and the sizes
// differ so CopyTextureRegion cannot do it.
//
// Hence one small graphics pipeline: a fullscreen triangle sampling the render-
// resolution texture into the display-resolution backbuffer. It is a plain
// bilinear stretch and makes no claim to be an upscaler; it exists so that a
// missing FSR dispatch degrades to a soft image instead of a black screen.
//
// The shader is compiled at load time through d3dcompiler_47.dll, resolved by
// name rather than linked, for the same reason FfxLoader resolves AMD's entry
// points that way: no import to satisfy, and a clean failure if it is absent.
#pragma once

#include <d3d12.h>

#include <cstdint>

#include "beamng_fsr/result.hpp"

namespace bfsr {

class PresentBlit {
public:
    PresentBlit() = default;
    ~PresentBlit();

    PresentBlit(const PresentBlit&) = delete;
    PresentBlit& operator=(const PresentBlit&) = delete;

    /// `renderTargetFormat` must match the swapchain's, since the pipeline
    /// state bakes it in.
    Result create(ID3D12Device* device, DXGI_FORMAT renderTargetFormat);
    void destroy();

    [[nodiscard]] bool isValid() const noexcept { return pipeline_ != nullptr; }

    /// Records source → destination. The caller owns both resources' states and
    /// must have `source` in a shader-readable state and `destination` in
    /// RENDER_TARGET before calling.
    Result record(ID3D12GraphicsCommandList* commandList,
                  ID3D12Resource* source,
                  ID3D12Resource* destination,
                  uint32_t destinationWidth,
                  uint32_t destinationHeight);

private:
    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* rootSignature_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
    /// One shader-visible slot for the source SRV, plus a non-shader-visible
    /// RTV heap for the destination. Both are single-slot: the blit records one
    /// draw and the command list is submitted before the next is recorded.
    ID3D12DescriptorHeap* srvHeap_ = nullptr;
    ID3D12DescriptorHeap* rtvHeap_ = nullptr;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

} // namespace bfsr
