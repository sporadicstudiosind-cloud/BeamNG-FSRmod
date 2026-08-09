// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/frame_command_ring.hpp"

#include "beamng_fsr/logging.hpp"

namespace bfsr {
namespace {

template <typename T>
void releaseAndNull(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

FrameCommandRing::~FrameCommandRing() { destroy(); }

Result FrameCommandRing::create(ID3D12Device* device, ID3D12CommandQueue* queue) {
    destroy();
    if (!device || !queue) {
        return Result::fail(Status::InvalidArgument,
                            "a device and command queue are required to record FSR work");
    }

    // Direct rather than compute: FSR's prepare pass is compute, but the queue
    // the work is submitted on is the game's own direct queue, and a command
    // list's type must match the queue it executes on.
    const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;

    for (uint32_t i = 0; i < kSlots; ++i) {
        if (FAILED(device->CreateCommandAllocator(type, __uuidof(ID3D12CommandAllocator),
                                                  reinterpret_cast<void**>(&allocators_[i])))) {
            destroy();
            return Result::fail(Status::RuntimeError,
                                "could not create a command allocator for FSR's own work");
        }
    }

    if (FAILED(device->CreateCommandList(0, type, allocators_[0], nullptr,
                                         __uuidof(ID3D12GraphicsCommandList),
                                         reinterpret_cast<void**>(&list_)))) {
        destroy();
        return Result::fail(Status::RuntimeError,
                            "could not create the command list FSR records into");
    }
    // Created open; close it so the first begin() can reset it uniformly.
    list_->Close();

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                   reinterpret_cast<void**>(&fence_)))) {
        destroy();
        return Result::fail(Status::RuntimeError, "could not create the FSR submission fence");
    }

    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) {
        destroy();
        return Result::fail(Status::RuntimeError, "could not create the FSR fence event");
    }

    device_ = device;
    queue_ = queue;
    queue_->AddRef();
    device_->AddRef();
    nextFenceValue_ = 1;
    current_ = 0;
    for (uint32_t i = 0; i < kSlots; ++i) slotFenceValue_[i] = 0;
    return Result::ok();
}

void FrameCommandRing::destroy() {
    if (fence_ && queue_) waitForIdle();

    releaseAndNull(list_);
    for (auto& a : allocators_) releaseAndNull(a);
    releaseAndNull(fence_);
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    releaseAndNull(queue_);
    releaseAndNull(device_);
    recording_ = false;
    current_ = 0;
    nextFenceValue_ = 1;
    for (auto& v : slotFenceValue_) v = 0;
}

void FrameCommandRing::waitForFence(uint64_t value) {
    if (!fence_ || value == 0) return;
    if (fence_->GetCompletedValue() >= value) return;
    if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_))) return;
    WaitForSingleObject(fenceEvent_, INFINITE);
}

void FrameCommandRing::waitForIdle() {
    if (!fence_ || !queue_) return;
    const uint64_t value = nextFenceValue_++;
    if (SUCCEEDED(queue_->Signal(fence_, value))) waitForFence(value);
}

ID3D12GraphicsCommandList* FrameCommandRing::begin() {
    if (!list_ || recording_) return nullptr;

    // The slot's previous submission must have retired before its allocator can
    // be reset. In steady state this has long since happened and the wait is
    // free; the check is what makes that safe rather than assumed.
    waitForFence(slotFenceValue_[current_]);

    if (FAILED(allocators_[current_]->Reset())) return nullptr;
    if (FAILED(list_->Reset(allocators_[current_], nullptr))) return nullptr;

    recording_ = true;
    return list_;
}

Result FrameCommandRing::submit() {
    if (!recording_ || !list_ || !queue_) {
        return Result::fail(Status::NotInitialised, "no FSR command list is open");
    }
    recording_ = false;

    if (FAILED(list_->Close())) {
        return Result::fail(Status::RuntimeError, "could not close the FSR command list");
    }

    ID3D12CommandList* lists[] = {list_};
    queue_->ExecuteCommandLists(1, lists);

    const uint64_t value = nextFenceValue_++;
    if (FAILED(queue_->Signal(fence_, value))) {
        // The work was submitted; only the tracking failed. Fall back to a
        // conservative value so the slot is not reused before it is safe.
        BFSR_LOG_WARN("could not signal the FSR fence; the command ring will stall instead");
        slotFenceValue_[current_] = nextFenceValue_;
    } else {
        slotFenceValue_[current_] = value;
    }

    current_ = (current_ + 1) % kSlots;
    return Result::ok();
}

} // namespace bfsr
