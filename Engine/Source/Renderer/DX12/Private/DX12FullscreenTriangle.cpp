#include "DX12FullscreenTriangle.h"

#include <limits>
#include <new>
#include <utility>

#include <d3dcompiler.h>

#include <DX12/DX12RenderDevice.h>

#include <FullscreenTrianglePS.h>
#include <FullscreenTriangleVS.h>

namespace cue::dx12
{
namespace
{
/// @brief HRESULT を共通 Error に変換する
Error fullscreen_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}
} // namespace

/// @brief 初期化済み DX12 Object の所有権を受け取る
DX12FullscreenTriangle::DX12FullscreenTriangle(
    Microsoft::WRL::ComPtr<ID3D12RootSignature> a_rootSignature,
    Microsoft::WRL::ComPtr<ID3D12PipelineState> a_pipeline) noexcept
    : m_rootSignature(std::move(a_rootSignature)), m_pipeline(std::move(a_pipeline))
{
}

/// @brief SRV 一つと線形 Clamp Sampler を持つ固定 Graphics Pipeline を生成する
Result<std::unique_ptr<DX12FullscreenTriangle>> DX12FullscreenTriangle::create(
    DX12RenderDevice& a_device, DXGI_FORMAT a_format)
{
    using TriangleResult = Result<std::unique_ptr<DX12FullscreenTriangle>>;
    if (!a_device.device() || (a_format != DXGI_FORMAT_R8G8B8A8_UNORM &&
                               a_format != DXGI_FORMAT_B8G8R8A8_UNORM))
    {
        return TriangleResult::failure({ErrorCategory::InvalidArgument, "DX12FullscreenTriangle.create"});
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameter.DescriptorTable.NumDescriptorRanges = 1;
    rootParameter.DescriptorTable.pDescriptorRanges = &srvRange;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &rootParameter;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostics);
    if (FAILED(serializeResult))
    {
        return TriangleResult::failure(fullscreen_error("D3D12SerializeRootSignature", serializeResult));
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    const HRESULT rootResult = a_device.device()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    if (FAILED(rootResult))
    {
        return TriangleResult::failure(fullscreen_error("ID3D12Device.CreateRootSignature", rootResult));
    }
    const HRESULT rootNameResult = rootSignature->SetName(L"Cue.FullscreenTriangle.RootSignature");
    if (FAILED(rootNameResult))
    {
        return TriangleResult::failure(fullscreen_error("ID3D12RootSignature.SetName", rootNameResult));
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = rootSignature.Get();
    pipelineDesc.VS = {g_fullscreenTriangleVs, sizeof(g_fullscreenTriangleVs)};
    pipelineDesc.PS = {g_fullscreenTrianglePs, sizeof(g_fullscreenTrianglePs)};
    pipelineDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
    pipelineDesc.DepthStencilState.DepthEnable = FALSE;
    pipelineDesc.DepthStencilState.StencilEnable = FALSE;
    pipelineDesc.SampleMask = (std::numeric_limits<UINT>::max)();
    pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDesc.NumRenderTargets = 1;
    pipelineDesc.RTVFormats[0] = a_format;
    pipelineDesc.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT pipelineResult = a_device.device()->CreateGraphicsPipelineState(
        &pipelineDesc, IID_PPV_ARGS(&pipeline));
    if (FAILED(pipelineResult))
    {
        return TriangleResult::failure(fullscreen_error("ID3D12Device.CreateGraphicsPipelineState", pipelineResult));
    }
    const HRESULT pipelineNameResult = pipeline->SetName(L"Cue.FullscreenTriangle.Pipeline");
    if (FAILED(pipelineNameResult))
    {
        return TriangleResult::failure(fullscreen_error("ID3D12PipelineState.SetName", pipelineNameResult));
    }
    try
    {
        return TriangleResult::success(std::make_unique<DX12FullscreenTriangle>(
            std::move(rootSignature), std::move(pipeline)));
    }
    catch (const std::bad_alloc&)
    {
        return TriangleResult::failure({ErrorCategory::PlatformFailure,
                                        "DX12FullscreenTriangle.create.allocation"});
    }
}

/// @brief BackBuffer 全体を覆う三角形を頂点 Buffer なしで描画する
void DX12FullscreenTriangle::draw(ID3D12GraphicsCommandList& a_command, ID3D12DescriptorHeap& a_heap,
                                   D3D12_GPU_DESCRIPTOR_HANDLE a_srv, D3D12_CPU_DESCRIPTOR_HANDLE a_rtv,
                                   std::uint32_t a_width, std::uint32_t a_height) const noexcept
{
    ID3D12DescriptorHeap* heaps[] = {&a_heap};
    a_command.SetDescriptorHeaps(1, heaps);
    a_command.SetGraphicsRootSignature(m_rootSignature.Get());
    a_command.SetPipelineState(m_pipeline.Get());
    a_command.SetGraphicsRootDescriptorTable(0, a_srv);
    a_command.OMSetRenderTargets(1, &a_rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(a_width),
                                  static_cast<float>(a_height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height)};
    a_command.RSSetViewports(1, &viewport);
    a_command.RSSetScissorRects(1, &scissor);
    a_command.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a_command.DrawInstanced(3, 1, 0, 0);
}
} // namespace cue::dx12
