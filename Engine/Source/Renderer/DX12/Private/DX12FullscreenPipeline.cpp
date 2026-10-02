#include <DX12/DX12FullscreenPipeline.h>

#include <climits>
#include <cstdint>
#include <memory>

#include <d3dcompiler.h>

#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>

#include "FullscreenPS.h"
#include "FullscreenVS.h"

namespace cue::dx12
{
namespace
{
/// @brief Native 生成失敗を操作名と HRESULT で返す
Error pipeline_error(const char* a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}
} // namespace

/// @brief create の内部でだけ空の Pipeline を構築する
DX12FullscreenPipeline::DX12FullscreenPipeline(CreateToken) noexcept
{
}

/// @brief DXC Bytecode と固定 SRV/Sampler 契約から Graphics PSO を作る
Result<std::unique_ptr<DX12FullscreenPipeline>> DX12FullscreenPipeline::create(
    DX12RenderDevice& a_device, DXGI_FORMAT a_targetFormat)
{
    using PipelineResult = Result<std::unique_ptr<DX12FullscreenPipeline>>;
    if (!a_device.device() ||
        (a_targetFormat != DXGI_FORMAT_R8G8B8A8_UNORM && a_targetFormat != DXGI_FORMAT_B8G8R8A8_UNORM))
    {
        return PipelineResult::failure({ErrorCategory::InvalidArgument, "DX12FullscreenPipeline.create"});
    }
    auto pipeline = std::make_unique<DX12FullscreenPipeline>(CreateToken{});

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
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
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &parameter;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> rootBytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> rootError;
    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytecode, &rootError);
    if (FAILED(serializeResult))
    {
        return PipelineResult::failure(pipeline_error("D3D12SerializeRootSignature", serializeResult));
    }
    const HRESULT rootResult = a_device.device()->CreateRootSignature(
        0, rootBytecode->GetBufferPointer(), rootBytecode->GetBufferSize(),
        IID_PPV_ARGS(&pipeline->m_rootSignature));
    if (FAILED(rootResult))
    {
        return PipelineResult::failure(pipeline_error("ID3D12Device.CreateRootSignature", rootResult));
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = pipeline->m_rootSignature.Get();
    psoDesc.VS = {g_fullscreenVS, sizeof(g_fullscreenVS)};
    psoDesc.PS = {g_fullscreenPS, sizeof(g_fullscreenPS)};
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = true;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = a_targetFormat;
    psoDesc.SampleDesc.Count = 1;
    const HRESULT psoResult = a_device.device()->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&pipeline->m_pipeline));
    if (FAILED(psoResult))
    {
        return PipelineResult::failure(pipeline_error("ID3D12Device.CreateGraphicsPipelineState", psoResult));
    }
    const HRESULT rootNameResult = pipeline->m_rootSignature->SetName(L"CueEngine DX12 Fullscreen Root Signature");
    if (FAILED(rootNameResult))
    {
        report_error("DX12FullscreenPipeline", pipeline_error("ID3D12RootSignature.SetName", rootNameResult),
                     DiagnosticSeverity::Warning);
    }
    const HRESULT pipelineNameResult = pipeline->m_pipeline->SetName(L"CueEngine DX12 Fullscreen Pipeline");
    if (FAILED(pipelineNameResult))
    {
        report_error("DX12FullscreenPipeline", pipeline_error("ID3D12PipelineState.SetName", pipelineNameResult),
                     DiagnosticSeverity::Warning);
    }
    return PipelineResult::success(std::move(pipeline));
}

/// @brief Vertex Buffer を使わず SV_VertexID の三点を全画面へ展開する
Result<void> DX12FullscreenPipeline::draw(
    ID3D12GraphicsCommandList& a_list, ID3D12DescriptorHeap& a_srvHeap,
    D3D12_GPU_DESCRIPTOR_HANDLE a_srv, D3D12_CPU_DESCRIPTOR_HANDLE a_target,
    std::uint32_t a_width, std::uint32_t a_height) const
{
    const auto heapDesc = a_srvHeap.GetDesc();
    if (!m_rootSignature || !m_pipeline || a_width == 0 || a_height == 0 ||
        a_width > LONG_MAX || a_height > LONG_MAX || a_srv.ptr == 0 || a_target.ptr == 0 ||
        heapDesc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        (heapDesc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12FullscreenPipeline.draw"});
    }
    ID3D12DescriptorHeap* heaps[] = {&a_srvHeap};
    a_list.SetDescriptorHeaps(1, heaps);
    a_list.SetGraphicsRootSignature(m_rootSignature.Get());
    a_list.SetPipelineState(m_pipeline.Get());
    a_list.SetGraphicsRootDescriptorTable(0, a_srv);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(a_width),
                                  static_cast<float>(a_height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height)};
    a_list.RSSetViewports(1, &viewport);
    a_list.RSSetScissorRects(1, &scissor);
    a_list.OMSetRenderTargets(1, &a_target, false, nullptr);
    a_list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a_list.DrawInstanced(3, 1, 0, 0);
    return Result<void>::success();
}
} // namespace cue::dx12
