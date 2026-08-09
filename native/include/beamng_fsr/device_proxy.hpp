// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <d3d12.h>

#include <atomic>

#include "beamng_fsr/resource_registry.hpp"

namespace bfsr {

/// Pass-through ID3D12Device that records 2D texture creation.
/// Rationale and scope limits are documented at the top of device_proxy.cpp.
class D3D12DeviceProxy final : public ID3D12Device {
public:
    D3D12DeviceProxy(ID3D12Device* real, ResourceRegistry* registry);

    [[nodiscard]] ID3D12Device* real() const noexcept { return real_; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* obj) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override;

    // ID3D12Device
    UINT STDMETHODCALLTYPE GetNodeCount() override;
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** ppv) override;
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppv) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) override;
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) override;
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initialState, REFIID riid, void** ppv) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT dataSize) override;
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** ppv) override;
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE type) override;
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT nodeMask, const void* blob, SIZE_T blobLength, REFIID riid, void** ppv) override;
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* resource, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CopyDescriptors(UINT numDestRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* destStarts, const UINT* destSizes, UINT numSrcRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts, const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE heapType) override;
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT numDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE destStart, D3D12_CPU_DESCRIPTOR_HANDLE srcStart, D3D12_DESCRIPTOR_HEAP_TYPE heapType) override;
    // Methods returning an aggregate by value.
    //
    // MSVC and the mingw-w64/widl headers disagree on the calling convention
    // here: widl declares them with a hidden out-pointer (the GCC struct-return
    // ABI) under WIDL_EXPLICIT_AGGREGATE_RETURNS, while MSVC uses the plain
    // by-value form. Both are the same vtable slot; only the C++ signature we
    // must override differs, so the declaration follows whichever headers are
    // in use rather than assuming one toolchain.
#if defined(WIDL_EXPLICIT_AGGREGATE_RETURNS)
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo(D3D12_RESOURCE_ALLOCATION_INFO* ret, UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* descs) override;
    D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE GetCustomHeapProperties(D3D12_HEAP_PROPERTIES* ret, UINT nodeMask, D3D12_HEAP_TYPE heapType) override;
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* descs) override;
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE heapType) override;
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heapProperties, D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* optimizedClearValue, REFIID riid, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* desc, REFIID riid, void** ppv) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* heap, UINT64 heapOffset, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* optimizedClearValue, REFIID riid, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* optimizedClearValue, REFIID riid, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild* object, const SECURITY_ATTRIBUTES* attributes, DWORD access, LPCWSTR name, HANDLE* handle) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE handle, REFIID riid, void** ppv) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR name, DWORD access, HANDLE* handle) override;
    HRESULT STDMETHODCALLTYPE MakeResident(UINT numObjects, ID3D12Pageable* const* objects) override;
    HRESULT STDMETHODCALLTYPE Evict(UINT numObjects, ID3D12Pageable* const* objects) override;
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initialValue, D3D12_FENCE_FLAGS flags, REFIID riid, void** ppv) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* desc, UINT firstSubresource, UINT numSubresources, UINT64 baseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* numRows, UINT64* rowSizeInBytes, UINT64* totalBytes) override;
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** ppv) override;
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL enable) override;
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* rootSignature, REFIID riid, void** ppv) override;
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource* resource, UINT* numTilesForEntireResource, D3D12_PACKED_MIP_INFO* packedMipDesc, D3D12_TILE_SHAPE* standardTileShapeForNonPackedMips, UINT* numSubresourceTilings, UINT firstSubresourceTilingToGet, D3D12_SUBRESOURCE_TILING* subresourceTilingsForNonPackedMips) override;
#if defined(WIDL_EXPLICIT_AGGREGATE_RETURNS)
    LUID* STDMETHODCALLTYPE GetAdapterLuid(LUID* ret) override;
#else
    LUID STDMETHODCALLTYPE GetAdapterLuid() override;
#endif

private:
    ~D3D12DeviceProxy();

    void trackIfResource(HRESULT hr, const D3D12_RESOURCE_DESC* desc, REFIID riid, void** ppvResource);

    std::atomic<long> refCount_{1};
    ID3D12Device* real_ = nullptr;
    ResourceRegistry* registry_ = nullptr;
    bool warnedAboutUnwrappedInterface_ = false;
};

} // namespace bfsr
