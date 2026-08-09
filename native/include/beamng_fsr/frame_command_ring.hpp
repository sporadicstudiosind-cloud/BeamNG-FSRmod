// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// Command recording for FSR's own GPU work.
//
// FSR's prepare dispatch takes an ID3D12GraphicsCommandList and records into
// it. The bridge cannot borrow one of BeamNG's: the game owns its allocators'
// lifetimes and reset points, and recording into a list the game is about to
// reset is a use-after-free with GPU timing. So the bridge brings its own.
//
// A ring rather than a single allocator because a command allocator may not be
// reset while the GPU is still executing anything recorded from it. With N
// slots and a fence per slot, frame `n` reuses slot `n % N` only after the GPU
// has signalled that slot's previous submission, which is the documented
// requirement and costs no CPU stall in steady state.
#pragma once

#include <d3d12.h>

#include <cstdint>

#include "beamng_fsr/result.hpp"

namespace bfsr {

class FrameCommandRing {
public:
    /// Three deep: matches the deepest swapchain BeamNG is likely to use, so a
    /// slot is never waited on in steady state.
    static constexpr uint32_t kSlots = 3;

    FrameCommandRing() = default;
    ~FrameCommandRing();

    FrameCommandRing(const FrameCommandRing&) = delete;
    FrameCommandRing& operator=(const FrameCommandRing&) = delete;

    Result create(ID3D12Device* device, ID3D12CommandQueue* queue);
    void destroy();

    [[nodiscard]] bool isValid() const noexcept { return list_ != nullptr; }

    /// Waits for the slot this frame will use, resets it, and returns a list
    /// open for recording. Returns null on failure; the caller then skips FSR
    /// for the frame rather than treating it as fatal.
    ID3D12GraphicsCommandList* begin();

    /// Closes the list, submits it on the queue the ring was created against,
    /// and signals the slot's fence.
    Result submit();

    /// Blocks until every submission from this ring has completed. Required
    /// before destroying resources the recorded work refers to.
    void waitForIdle();

private:
    void waitForFence(uint64_t value);

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12CommandAllocator* allocators_[kSlots] = {};
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fenceEvent_ = nullptr;
    uint64_t slotFenceValue_[kSlots] = {};
    uint64_t nextFenceValue_ = 1;
    uint32_t current_ = 0;
    bool recording_ = false;
};

} // namespace bfsr
