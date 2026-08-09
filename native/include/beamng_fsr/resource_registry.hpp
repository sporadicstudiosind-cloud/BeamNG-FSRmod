// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <d3d12.h>

#include <mutex>
#include <unordered_map>
#include <vector>

#include "beamng_fsr/beacon_signature.hpp"

namespace bfsr {

/// Tracks textures created through the wrapped D3D12 device.
///
/// The registry stores *descriptions*, never a reference. Holding an AddRef on
/// the game's resources would change their lifetime and keep dead render
/// targets alive across a resolution change, so entries are recorded on
/// creation and dropped when the game destroys the resource. A stale pointer is
/// never dereferenced: the bridge only resolves a handle to a live resource
/// inside a frame it is already synchronised with.
class ResourceRegistry {
public:
    void recordTexture(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc);
    void forget(ID3D12Resource* resource);
    void clear();

    /// Snapshot for the matcher. Copies deliberately: matching happens outside
    /// the lock so a slow match never blocks resource creation on the render
    /// thread.
    [[nodiscard]] std::vector<ResourceDescriptor> snapshot() const;

    [[nodiscard]] size_t size() const;

    /// Bumped whenever a texture is recorded or forgotten.
    ///
    /// The per-frame driver caches the resources it resolved from a beacon
    /// signature, and a cached ID3D12Resource* is only safe to reuse while no
    /// resource has been destroyed since. Comparing this counter is how the
    /// driver knows the cache is still describing live objects without copying
    /// and re-matching the whole registry on every present.
    [[nodiscard]] uint64_t generation() const;

    /// Converts a D3D12 description into the portable form the matcher uses.
    static ResourceDescriptor describe(ID3D12Resource* resource,
                                       const D3D12_RESOURCE_DESC& desc,
                                       uint64_t sequence);

private:
    mutable std::mutex mutex_;
    std::unordered_map<ID3D12Resource*, ResourceDescriptor> textures_;
    uint64_t sequence_ = 0;
    uint64_t generation_ = 0;
};

/// Watches a resource for destruction so the registry can drop its entry.
///
/// D3D12 gives no destruction callback, so this attaches an object with
/// SetPrivateDataInterface: the runtime releases it when the resource dies,
/// which is the documented way to observe resource lifetime.
void attachDestructionWatcher(ID3D12Resource* resource, ResourceRegistry* registry);

} // namespace bfsr
