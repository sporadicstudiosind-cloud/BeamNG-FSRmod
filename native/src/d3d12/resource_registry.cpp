// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/resource_registry.hpp"

#include "beamng_fsr/logging.hpp"

#include <atomic>

namespace bfsr {
namespace {

// The portable matcher duplicates the DXGI_FORMAT values it needs so it can be
// unit-tested without Windows headers. Verify the duplicates here, where the
// real enum is in scope, so a divergence is a build error rather than a silent
// misclassification at runtime.
static_assert(dxgi_format::kD32Float == DXGI_FORMAT_D32_FLOAT, "DXGI format mirror drifted");
static_assert(dxgi_format::kD24UnormS8Uint == DXGI_FORMAT_D24_UNORM_S8_UINT, "DXGI format mirror drifted");
static_assert(dxgi_format::kD16Unorm == DXGI_FORMAT_D16_UNORM, "DXGI format mirror drifted");
static_assert(dxgi_format::kD32FloatS8X24Uint == DXGI_FORMAT_D32_FLOAT_S8X24_UINT, "DXGI format mirror drifted");
static_assert(dxgi_format::kR32Typeless == DXGI_FORMAT_R32_TYPELESS, "DXGI format mirror drifted");
static_assert(dxgi_format::kR24G8Typeless == DXGI_FORMAT_R24G8_TYPELESS, "DXGI format mirror drifted");
static_assert(dxgi_format::kR32G8X24Typeless == DXGI_FORMAT_R32G8X24_TYPELESS, "DXGI format mirror drifted");
static_assert(dxgi_format::kR16Typeless == DXGI_FORMAT_R16_TYPELESS, "DXGI format mirror drifted");
static_assert(dxgi_format::kR16G16Float == DXGI_FORMAT_R16G16_FLOAT, "DXGI format mirror drifted");
static_assert(dxgi_format::kR32G32Float == DXGI_FORMAT_R32G32_FLOAT, "DXGI format mirror drifted");
static_assert(dxgi_format::kR8G8B8A8Unorm == DXGI_FORMAT_R8G8B8A8_UNORM, "DXGI format mirror drifted");
static_assert(dxgi_format::kR16G16B16A16Float == DXGI_FORMAT_R16G16B16A16_FLOAT, "DXGI format mirror drifted");
static_assert(dxgi_format::kR11G11B10Float == DXGI_FORMAT_R11G11B10_FLOAT, "DXGI format mirror drifted");
static_assert(dxgi_format::kB8G8R8A8Unorm == DXGI_FORMAT_B8G8R8A8_UNORM, "DXGI format mirror drifted");
static_assert(dxgi_format::kR32Float == DXGI_FORMAT_R32_FLOAT, "DXGI format mirror drifted");
static_assert(dxgi_format::kUnknown == DXGI_FORMAT_UNKNOWN, "DXGI format mirror drifted");

/// GUID under which the destruction watcher is attached. Private to this
/// bridge; nothing else reads it.
// {6C8B4F2E-9A31-4D77-B4E1-0F6D2A5C8E93}
constexpr GUID kDestructionWatcherGuid = {
    0x6c8b4f2e, 0x9a31, 0x4d77, {0xb4, 0xe1, 0x0f, 0x6d, 0x2a, 0x5c, 0x8e, 0x93}};

/// Minimal IUnknown whose final Release tells the registry the resource is gone.
class DestructionWatcher final : public IUnknown {
public:
    DestructionWatcher(ID3D12Resource* resource, ResourceRegistry* registry)
        : resource_(resource), registry_(registry) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining =
            static_cast<ULONG>(refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (remaining == 0) {
            // The D3D12 runtime releases private-data interfaces as the
            // resource is destroyed, so this is our destruction notification.
            if (registry_) registry_->forget(resource_);
            delete this;
        }
        return remaining;
    }

private:
    ~DestructionWatcher() = default;

    std::atomic<long> refCount_{1};
    ID3D12Resource* resource_ = nullptr;
    ResourceRegistry* registry_ = nullptr;
};

} // namespace

ResourceDescriptor ResourceRegistry::describe(ID3D12Resource* resource,
                                              const D3D12_RESOURCE_DESC& desc,
                                              uint64_t sequence) {
    ResourceDescriptor d;
    d.handle = reinterpret_cast<uint64_t>(resource);
    d.width = static_cast<uint32_t>(desc.Width);
    d.height = desc.Height;
    d.depthOrArraySize = desc.DepthOrArraySize;
    d.mipLevels = desc.MipLevels;
    d.format = static_cast<uint32_t>(desc.Format);
    d.sampleCount = desc.SampleDesc.Count;
    d.allowRenderTarget = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    d.allowDepthStencil = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    d.allowUnorderedAccess = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    d.isTexture2D = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.creationSequence = sequence;
    return d;
}

void ResourceRegistry::recordTexture(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) {
    if (!resource) return;
    // Buffers and 1D/3D textures cannot be FSR inputs; not tracking them keeps
    // the map small in a game that allocates constantly.
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;

    std::lock_guard<std::mutex> lock(mutex_);
    textures_[resource] = describe(resource, desc, ++sequence_);
}

void ResourceRegistry::forget(ID3D12Resource* resource) {
    if (!resource) return;
    std::lock_guard<std::mutex> lock(mutex_);
    textures_.erase(resource);
}

void ResourceRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    textures_.clear();
}

std::vector<ResourceDescriptor> ResourceRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ResourceDescriptor> out;
    out.reserve(textures_.size());
    for (const auto& [ptr, desc] : textures_) out.push_back(desc);
    return out;
}

size_t ResourceRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return textures_.size();
}

void attachDestructionWatcher(ID3D12Resource* resource, ResourceRegistry* registry) {
    if (!resource || !registry) return;
    auto* watcher = new DestructionWatcher(resource, registry);
    // SetPrivateDataInterface takes its own reference, so drop ours; the
    // runtime now owns the watcher and releases it when the resource dies.
    const HRESULT hr = resource->SetPrivateDataInterface(kDestructionWatcherGuid, watcher);
    watcher->Release();
    if (FAILED(hr)) {
        BFSR_LOG_DEBUG("could not attach a destruction watcher (hr 0x%08lx); the registry "
                       "will rely on description matching to reject stale entries",
                       static_cast<unsigned long>(hr));
    }
}

} // namespace bfsr
