// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/present_blit.hpp"

#include "beamng_fsr/logging.hpp"

#include <windows.h>

namespace bfsr {
namespace {

/// A fullscreen triangle generated from the vertex id — no vertex buffer, no
/// input layout, three vertices covering the viewport.
///
/// Shader model 5.0 rather than 6.x deliberately: this is compiled through
/// D3DCompile, which tops out at 5.1, and a bilinear stretch needs nothing
/// newer. FSR's own shaders come precompiled inside AMD's DLLs and are
/// unaffected by this choice.
constexpr char kShaderSource[] = R"(
Texture2D<float4> gSource : register(t0);
SamplerState      gLinear : register(s0);

struct VSOut {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut input) : SV_TARGET
{
    return gSource.SampleLevel(gLinear, input.uv, 0);
}
)";

using PfnD3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR,
                                       LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
using PfnD3D12SerializeRootSignature = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*,
                                                        D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                                        ID3DBlob**);

template <typename T>
void releaseAndNull(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

/// Compiles one entry point. Returns null on failure, logging the compiler's
/// own message rather than a generic one — a shader that will not compile is
/// something a maintainer needs the real text of.
ID3DBlob* compile(PfnD3DCompile fn, const char* entryPoint, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = fn(kShaderSource, sizeof(kShaderSource) - 1, "bfsr_present_blit", nullptr,
                          nullptr, entryPoint, target, 0, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) {
            BFSR_LOG_WARN("present blit shader (%s) failed to compile: %.512s", entryPoint,
                          static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        } else {
            BFSR_LOG_WARN("present blit shader (%s) failed to compile: 0x%08lx", entryPoint,
                          static_cast<unsigned long>(hr));
        }
        if (code) code->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    return code;
}

} // namespace

PresentBlit::~PresentBlit() { destroy(); }

void PresentBlit::destroy() {
    releaseAndNull(pipeline_);
    releaseAndNull(rootSignature_);
    releaseAndNull(srvHeap_);
    releaseAndNull(rtvHeap_);
    releaseAndNull(device_);
    format_ = DXGI_FORMAT_UNKNOWN;
}

Result PresentBlit::create(ID3D12Device* device, DXGI_FORMAT renderTargetFormat) {
    destroy();
    if (!device) return Result::fail(Status::InvalidArgument, "no Direct3D 12 device");
    if (renderTargetFormat == DXGI_FORMAT_UNKNOWN) {
        return Result::fail(Status::InvalidArgument, "the swapchain format is unknown");
    }

    // Resolved by name: d3dcompiler_47.dll ships with Windows, and a missing
    // import would stop the bridge loading at all rather than disabling one
    // fallback path.
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) {
        return Result::fail(Status::MissingDependency,
                            "d3dcompiler_47.dll could not be loaded, so the fallback present "
                            "blit is unavailable");
    }
    auto compileFn = reinterpret_cast<PfnD3DCompile>(
        reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")));
    if (!compileFn) {
        FreeLibrary(compiler);
        return Result::fail(Status::MissingDependency,
                            "d3dcompiler_47.dll exports no D3DCompile");
    }

    ID3DBlob* vertexCode = compile(compileFn, "VSMain", "vs_5_0");
    ID3DBlob* pixelCode = vertexCode ? compile(compileFn, "PSMain", "ps_5_0") : nullptr;
    // The compiled blobs outlive the module handle's usefulness; the DLL itself
    // stays loaded for the process, which is fine and is what every D3D
    // application does.
    FreeLibrary(compiler);
    if (!vertexCode || !pixelCode) {
        if (vertexCode) vertexCode->Release();
        if (pixelCode) pixelCode->Release();
        return Result::fail(Status::RuntimeError, "the fallback present blit shader did not "
                                                  "compile; see the log for the compiler's "
                                                  "message");
    }

    // One SRV table plus one static sampler. Clamped bilinear: the source is a
    // full frame, so anything sampled outside it is a rounding error at the
    // edge rather than real content.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &parameter;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // Resolved by name for the same reason as the compiler: the proxy stub
    // named d3d12.dll re-exports this symbol, and importing it directly would
    // make the bridge's own import resolve back through the proxy.
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialiseFn =
        d3d12 ? reinterpret_cast<PfnD3D12SerializeRootSignature>(
                    reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")))
              : nullptr;
    if (!serialiseFn) {
        vertexCode->Release();
        pixelCode->Release();
        return Result::fail(Status::MissingDependency,
                            "D3D12SerializeRootSignature could not be resolved");
    }

    ID3DBlob* serialised = nullptr;
    ID3DBlob* rootErrors = nullptr;
    if (FAILED(serialiseFn(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialised, &rootErrors))) {
        if (rootErrors) rootErrors->Release();
        vertexCode->Release();
        pixelCode->Release();
        return Result::fail(Status::RuntimeError,
                            "the present blit root signature could not be serialised");
    }
    if (rootErrors) rootErrors->Release();

    const HRESULT rootHr = device->CreateRootSignature(
        0, serialised->GetBufferPointer(), serialised->GetBufferSize(),
        __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&rootSignature_));
    serialised->Release();
    if (FAILED(rootHr)) {
        vertexCode->Release();
        pixelCode->Release();
        return Result::fail(Status::RuntimeError,
                            "the present blit root signature could not be created");
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature_;
    pso.VS = {vertexCode->GetBufferPointer(), vertexCode->GetBufferSize()};
    pso.PS = {pixelCode->GetBufferPointer(), pixelCode->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    // No depth buffer is bound, so depth and stencil testing must be off or the
    // draw is discarded against a nonexistent target.
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = renderTargetFormat;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count = 1;

    const HRESULT psoHr = device->CreateGraphicsPipelineState(
        &pso, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pipeline_));
    vertexCode->Release();
    pixelCode->Release();
    if (FAILED(psoHr)) {
        releaseAndNull(rootSignature_);
        return Result::fail(Status::RuntimeError,
                            "the present blit pipeline state could not be created");
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 1;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvDesc, __uuidof(ID3D12DescriptorHeap),
                                            reinterpret_cast<void**>(&srvHeap_)))) {
        releaseAndNull(pipeline_);
        releaseAndNull(rootSignature_);
        return Result::fail(Status::RuntimeError,
                            "the present blit descriptor heap could not be created");
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, __uuidof(ID3D12DescriptorHeap),
                                            reinterpret_cast<void**>(&rtvHeap_)))) {
        releaseAndNull(srvHeap_);
        releaseAndNull(pipeline_);
        releaseAndNull(rootSignature_);
        return Result::fail(Status::RuntimeError,
                            "the present blit render-target heap could not be created");
    }

    device_ = device;
    device_->AddRef();
    format_ = renderTargetFormat;
    BFSR_LOG_INFO("fallback present blit ready (format %d)", static_cast<int>(renderTargetFormat));
    return Result::ok();
}

Result PresentBlit::record(ID3D12GraphicsCommandList* commandList,
                           ID3D12Resource* source,
                           ID3D12Resource* destination,
                           uint32_t destinationWidth,
                           uint32_t destinationHeight) {
    if (!isValid() || !device_) {
        return Result::fail(Status::NotInitialised, "the present blit is not created");
    }
    if (!commandList || !source || !destination || destinationWidth == 0 ||
        destinationHeight == 0) {
        return Result::fail(Status::InvalidArgument, "the present blit needs both resources");
    }

    // Descriptors are rewritten each frame rather than cached per resource: the
    // substitute textures are recreated on every resize, and a cached view of a
    // dead resource is exactly the bug this avoids. Writing one descriptor is
    // cheaper than tracking their lifetimes.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format_;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(source, &srv,
                                      srvHeap_->GetCPUDescriptorHandleForHeapStart());

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = format_;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(destination, &rtv, rtvHandle);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(destinationWidth);
    viewport.Height = static_cast<float>(destinationHeight);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor{};
    scissor.right = static_cast<LONG>(destinationWidth);
    scissor.bottom = static_cast<LONG>(destinationHeight);

    ID3D12DescriptorHeap* heaps[] = {srvHeap_};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(rootSignature_);
    commandList->SetGraphicsRootDescriptorTable(0,
                                                srvHeap_->GetGPUDescriptorHandleForHeapStart());
    commandList->SetPipelineState(pipeline_);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
    return Result::ok();
}

} // namespace bfsr
