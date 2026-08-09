// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// A pass-through implementation of ID3D12Device.
//
// Why a wrapper and not a vtable patch: patching the runtime's vtable mutates
// memory shared by every device in the process and races with any other overlay
// doing the same. A wrapper is inert — the game holds our object, we hold the
// real one, and if anything here fails we can hand the real pointer back and
// disappear.
//
// Why wrap at all: FSR frame generation needs the game's depth and motion
// vector textures as ID3D12Resource pointers, and D3D12 offers no way to
// enumerate live resources. Observing creation is the only route, and this is
// the least invasive one that touches nothing belonging to BeamNG.
//
// Scope, stated honestly: only the base ID3D12Device interface is wrapped.
// QueryInterface for ID3D12Device1 and later returns the *real* interface, so
// resources created through those newer interfaces are not observed. That
// degrades into "render inputs unavailable" and FSR stays off — it never
// produces a wrong image. See docs/LIMITATIONS.md.
#include "beamng_fsr/device_proxy.hpp"

#include "beamng_fsr/logging.hpp"

namespace bfsr {

D3D12DeviceProxy::D3D12DeviceProxy(ID3D12Device* real, ResourceRegistry* registry)
    : real_(real), registry_(registry) {
    if (real_) real_->AddRef();
}

D3D12DeviceProxy::~D3D12DeviceProxy() {
    if (real_) real_->Release();
}

// --- IUnknown --------------------------------------------------------------

HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    if (riid == __uuidof(ID3D12Device) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(IUnknown)) {
        *ppv = static_cast<ID3D12Device*>(this);
        AddRef();
        return S_OK;
    }

    // Anything newer goes straight to the real device. The game keeps working
    // at full capability; we simply stop seeing resources created through it.
    const HRESULT hr = real_->QueryInterface(riid, ppv);
    if (SUCCEEDED(hr) && !warnedAboutUnwrappedInterface_) {
        warnedAboutUnwrappedInterface_ = true;
        BFSR_LOG_WARN("the game requested a Direct3D 12 device interface this build does "
                      "not wrap; resources created through it will not be observed. If FSR "
                      "reports that render inputs are unavailable, this is why.");
    }
    return hr;
}

ULONG STDMETHODCALLTYPE D3D12DeviceProxy::AddRef() {
    return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
}

ULONG STDMETHODCALLTYPE D3D12DeviceProxy::Release() {
    const ULONG remaining =
        static_cast<ULONG>(refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1);
    if (remaining == 0) delete this;
    return remaining;
}

// --- ID3D12Object ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::GetPrivateData(REFGUID guid, UINT* size, void* data) {
    return real_->GetPrivateData(guid, size, data);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::SetPrivateData(REFGUID guid, UINT size, const void* data) {
    return real_->SetPrivateData(guid, size, data);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* obj) {
    return real_->SetPrivateDataInterface(guid, obj);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::SetName(LPCWSTR name) {
    return real_->SetName(name);
}

// --- ID3D12Device: intercepted -------------------------------------------

HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES* heapProperties, D3D12_HEAP_FLAGS heapFlags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* optimizedClearValue, REFIID riid, void** ppvResource) {
    const HRESULT hr = real_->CreateCommittedResource(heapProperties, heapFlags, desc,
                                                      initialState, optimizedClearValue,
                                                      riid, ppvResource);
    trackIfResource(hr, desc, riid, ppvResource);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreatePlacedResource(
    ID3D12Heap* heap, UINT64 heapOffset, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* optimizedClearValue,
    REFIID riid, void** ppvResource) {
    const HRESULT hr = real_->CreatePlacedResource(heap, heapOffset, desc, initialState,
                                                   optimizedClearValue, riid, ppvResource);
    trackIfResource(hr, desc, riid, ppvResource);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateReservedResource(
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* optimizedClearValue, REFIID riid, void** ppvResource) {
    const HRESULT hr = real_->CreateReservedResource(desc, initialState, optimizedClearValue,
                                                     riid, ppvResource);
    trackIfResource(hr, desc, riid, ppvResource);
    return hr;
}

void D3D12DeviceProxy::trackIfResource(HRESULT hr, const D3D12_RESOURCE_DESC* desc,
                                       REFIID riid, void** ppvResource) {
    // ppvResource may legitimately be null: D3D12 allows a null out-pointer to
    // mean "just tell me whether this would succeed".
    if (FAILED(hr) || !desc || !ppvResource || !*ppvResource || !registry_) return;
    if (riid != __uuidof(ID3D12Resource)) return;
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;

    auto* resource = static_cast<ID3D12Resource*>(*ppvResource);
    registry_->recordTexture(resource, *desc);
    attachDestructionWatcher(resource, registry_);
}

// --- ID3D12Device: straight forwarding ------------------------------------

UINT STDMETHODCALLTYPE D3D12DeviceProxy::GetNodeCount() { return real_->GetNodeCount(); }

HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateCommandQueue(
    const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** ppv) {
    return real_->CreateCommandQueue(desc, riid, ppv);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateCommandAllocator(
    D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppv) {
    return real_->CreateCommandAllocator(type, riid, ppv);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateGraphicsPipelineState(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) {
    return real_->CreateGraphicsPipelineState(desc, riid, ppv);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateComputePipelineState(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) {
    return real_->CreateComputePipelineState(desc, riid, ppv);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateCommandList(
    UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
    ID3D12PipelineState* initialState, REFIID riid, void** ppv) {
    return real_->CreateCommandList(nodeMask, type, allocator, initialState, riid, ppv);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CheckFeatureSupport(
    D3D12_FEATURE feature, void* data, UINT dataSize) {
    return real_->CheckFeatureSupport(feature, data, dataSize);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateDescriptorHeap(
    const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** ppv) {
    return real_->CreateDescriptorHeap(desc, riid, ppv);
}
UINT STDMETHODCALLTYPE D3D12DeviceProxy::GetDescriptorHandleIncrementSize(
    D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return real_->GetDescriptorHandleIncrementSize(type);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateRootSignature(
    UINT nodeMask, const void* blob, SIZE_T blobLength, REFIID riid, void** ppv) {
    return real_->CreateRootSignature(nodeMask, blob, blobLength, riid, ppv);
}
void STDMETHODCALLTYPE D3D12DeviceProxy::CreateConstantBufferView(
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    real_->CreateConstantBufferView(desc, dest);
}
void STDMETHODCALLTYPE D3D12DeviceProxy::CreateShaderResourceView(
    ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    real_->CreateShaderResourceView(resource, desc, dest);
}
void STDMETHODCALLTYPE D3D12DeviceProxy::CreateUnorderedAccessView(
    ID3D12Resource* resource, ID3D12Resource* counter,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    real_->CreateUnorderedAccessView(resource, counter, desc, dest);
}
void STDMETHODCALLTYPE D3D12DeviceProxy::CreateRenderTargetView(
    ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    real_->CreateRenderTargetView(resource, desc, dest);
}
void STDMETHODCALLTYPE D3D12DeviceProxy::CreateDepthStencilView(
    ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    real_->CreateDepthStencilView(resource, desc, dest);
}
void STDMETHODCALLTYPE D3D12DeviceProxy::CreateSampler(
    const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    real_->CreateSampler(desc, dest);
}
void STDMETHODCALLTYPE D3D12DeviceProxy::CopyDescriptors(
    UINT numDestRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* destStarts,
    const UINT* destSizes, UINT numSrcRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts,
    const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE heapType) {
    real_->CopyDescriptors(numDestRanges, destStarts, destSizes, numSrcRanges, srcStarts,
                           srcSizes, heapType);
}
void STDMETHODCALLTYPE D3D12DeviceProxy::CopyDescriptorsSimple(
    UINT numDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE destStart,
    D3D12_CPU_DESCRIPTOR_HANDLE srcStart, D3D12_DESCRIPTOR_HEAP_TYPE heapType) {
    real_->CopyDescriptorsSimple(numDescriptors, destStart, srcStart, heapType);
}
#if defined(WIDL_EXPLICIT_AGGREGATE_RETURNS)
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE D3D12DeviceProxy::GetResourceAllocationInfo(
    D3D12_RESOURCE_ALLOCATION_INFO* ret, UINT visibleMask, UINT numResourceDescs,
    const D3D12_RESOURCE_DESC* descs) {
    return real_->GetResourceAllocationInfo(ret, visibleMask, numResourceDescs, descs);
}
D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE D3D12DeviceProxy::GetCustomHeapProperties(
    D3D12_HEAP_PROPERTIES* ret, UINT nodeMask, D3D12_HEAP_TYPE heapType) {
    return real_->GetCustomHeapProperties(ret, nodeMask, heapType);
}
#else
D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE D3D12DeviceProxy::GetResourceAllocationInfo(
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* descs) {
    return real_->GetResourceAllocationInfo(visibleMask, numResourceDescs, descs);
}
D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE D3D12DeviceProxy::GetCustomHeapProperties(
    UINT nodeMask, D3D12_HEAP_TYPE heapType) {
    return real_->GetCustomHeapProperties(nodeMask, heapType);
}
#endif
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateHeap(
    const D3D12_HEAP_DESC* desc, REFIID riid, void** ppv) {
    return real_->CreateHeap(desc, riid, ppv);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateSharedHandle(
    ID3D12DeviceChild* object, const SECURITY_ATTRIBUTES* attributes, DWORD access,
    LPCWSTR name, HANDLE* handle) {
    return real_->CreateSharedHandle(object, attributes, access, name, handle);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::OpenSharedHandle(
    HANDLE handle, REFIID riid, void** ppv) {
    return real_->OpenSharedHandle(handle, riid, ppv);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::OpenSharedHandleByName(
    LPCWSTR name, DWORD access, HANDLE* handle) {
    return real_->OpenSharedHandleByName(name, access, handle);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::MakeResident(UINT numObjects, ID3D12Pageable* const* objects) {
    return real_->MakeResident(numObjects, objects);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::Evict(UINT numObjects, ID3D12Pageable* const* objects) {
    return real_->Evict(numObjects, objects);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateFence(
    UINT64 initialValue, D3D12_FENCE_FLAGS flags, REFIID riid, void** ppv) {
    return real_->CreateFence(initialValue, flags, riid, ppv);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::GetDeviceRemovedReason() {
    return real_->GetDeviceRemovedReason();
}
void STDMETHODCALLTYPE D3D12DeviceProxy::GetCopyableFootprints(
    const D3D12_RESOURCE_DESC* desc, UINT firstSubresource, UINT numSubresources,
    UINT64 baseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* numRows,
    UINT64* rowSizeInBytes, UINT64* totalBytes) {
    real_->GetCopyableFootprints(desc, firstSubresource, numSubresources, baseOffset, layouts,
                                 numRows, rowSizeInBytes, totalBytes);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateQueryHeap(
    const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** ppv) {
    return real_->CreateQueryHeap(desc, riid, ppv);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::SetStablePowerState(BOOL enable) {
    return real_->SetStablePowerState(enable);
}
HRESULT STDMETHODCALLTYPE D3D12DeviceProxy::CreateCommandSignature(
    const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* rootSignature,
    REFIID riid, void** ppv) {
    return real_->CreateCommandSignature(desc, rootSignature, riid, ppv);
}
void STDMETHODCALLTYPE D3D12DeviceProxy::GetResourceTiling(
    ID3D12Resource* resource, UINT* numTilesForEntireResource,
    D3D12_PACKED_MIP_INFO* packedMipDesc, D3D12_TILE_SHAPE* standardTileShapeForNonPackedMips,
    UINT* numSubresourceTilings, UINT firstSubresourceTilingToGet,
    D3D12_SUBRESOURCE_TILING* subresourceTilingsForNonPackedMips) {
    real_->GetResourceTiling(resource, numTilesForEntireResource, packedMipDesc,
                             standardTileShapeForNonPackedMips, numSubresourceTilings,
                             firstSubresourceTilingToGet, subresourceTilingsForNonPackedMips);
}
#if defined(WIDL_EXPLICIT_AGGREGATE_RETURNS)
LUID* STDMETHODCALLTYPE D3D12DeviceProxy::GetAdapterLuid(LUID* ret) {
    return real_->GetAdapterLuid(ret);
}
#else
LUID STDMETHODCALLTYPE D3D12DeviceProxy::GetAdapterLuid() { return real_->GetAdapterLuid(); }
#endif

} // namespace bfsr
