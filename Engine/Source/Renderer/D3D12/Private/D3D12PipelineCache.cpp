#include "D3D12PipelineCache.h"

#include <array>
#include <climits>
#include <cstring>
#include <string>
#include <utility>

#include <d3dcompiler.h>
#include <dxcapi.h>

#include "FixedMeshShaderPath.h"

namespace cue::detail
{
namespace
{
/// @brief 配置済み Shader ファイルを指定 Entry Point で Compile して Error を返す
Result<Microsoft::WRL::ComPtr<IDxcBlob>> compile_shader(const wchar_t* a_entry, const wchar_t* a_target)
{
    using BlobResult = Result<Microsoft::WRL::ComPtr<IDxcBlob>>;
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    HRESULT result = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(result))
    {
        return BlobResult::failure(gpu_error("DxcCreateInstance.Utils", result));
    }
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    result = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(result))
    {
        return BlobResult::failure(gpu_error("DxcCreateInstance.Compiler", result));
    }
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> source;
    result = utils->LoadFile(k_fixedMeshShaderPath, nullptr, &source);
    if (FAILED(result))
    {
        return BlobResult::failure(gpu_error("IDxcUtils.LoadFile.FixedMesh", result));
    }
    DxcBuffer buffer{source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_UTF8};
    const wchar_t* arguments[] = {L"-E", a_entry, L"-T", a_target, L"-HV", L"2021"};
    Microsoft::WRL::ComPtr<IDxcResult> compilation;
    result = compiler->Compile(&buffer, arguments, static_cast<UINT32>(std::size(arguments)),
                               nullptr, IID_PPV_ARGS(&compilation));
    if (FAILED(result))
    {
        return BlobResult::failure(gpu_error("IDxcCompiler3.Compile.FixedMesh", result));
    }
    HRESULT status = S_OK;
    result = compilation->GetStatus(&status);
    if (FAILED(result))
    {
        return BlobResult::failure(gpu_error("IDxcResult.GetStatus.FixedMesh", result));
    }
    if (FAILED(status))
    {
        Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
        compilation->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        const auto details = errors ? std::string(errors->GetStringPointer(), errors->GetStringLength())
                                    : std::string{};
        return BlobResult::failure({ErrorCategory::PlatformFailure,
                                    std::string("DXC.FixedMesh: ") + details,
                                    static_cast<std::int64_t>(status)});
    }
    Microsoft::WRL::ComPtr<IDxcBlob> shader;
    result = compilation->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr);
    if (FAILED(result))
    {
        return BlobResult::failure(gpu_error("IDxcResult.GetOutput.FixedMesh", result));
    }
    return BlobResult::success(std::move(shader));
}
} // namespace

/// @brief 常時 Map した CBV の Upload Buffer を解放する
D3D12PipelineCache::~D3D12PipelineCache()
{
    if (m_mappedConstants)
    {
        m_constants->Unmap(0, nullptr);
    }
}

/// @brief 固定 Shader と Descriptor Table を一度だけ生成する
Result<std::unique_ptr<D3D12PipelineCache>> D3D12PipelineCache::create(D3D12DeviceContext& a_device)
{
    using CacheResult = Result<std::unique_ptr<D3D12PipelineCache>>;
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_0};
    const HRESULT support = a_device.device()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                                                                     &shaderModel, sizeof(shaderModel));
    if (FAILED(support) || shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_0)
    {
        return CacheResult::failure({ErrorCategory::PlatformFailure, "D3D12.ShaderModel6.Required",
                                     static_cast<std::int64_t>(support)});
    }
    auto vertexResult = compile_shader(L"VSMain", L"vs_6_0");
    if (!vertexResult.has_value())
    {
        return CacheResult::failure(*vertexResult.try_error());
    }
    auto pixelResult = compile_shader(L"PSMain", L"ps_6_0");
    if (!pixelResult.has_value())
    {
        return CacheResult::failure(*pixelResult.try_error());
    }
    auto cache = std::make_unique<D3D12PipelineCache>();
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC signatureDesc{};
    signatureDesc.NumParameters = 1;
    signatureDesc.pParameters = &parameter;
    signatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT result = D3D12SerializeRootSignature(&signatureDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   &serialized, &errors);
    if (FAILED(result))
    {
        return CacheResult::failure(gpu_error("D3D12SerializeRootSignature", result));
    }
    result = a_device.device()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                     IID_PPV_ARGS(&cache->m_rootSignature));
    if (FAILED(result))
    {
        return CacheResult::failure(gpu_error("ID3D12Device.CreateRootSignature", result));
    }
    result = cache->m_rootSignature->SetName(L"CueEngine Fixed Mesh Root Signature");
    if (FAILED(result))
    {
        return CacheResult::failure(gpu_error("ID3D12RootSignature.SetName", result));
    }
    constexpr D3D12_INPUT_ELEMENT_DESC k_input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = cache->m_rootSignature.Get();
    const auto vertex = vertexResult.take_value();
    const auto pixel = pixelResult.take_value();
    pso.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    pso.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    pso.InputLayout = {k_input, static_cast<UINT>(std::size(k_input))};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.NumRenderTargets = 1;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = true;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = true;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    result = a_device.device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&cache->m_pipeline));
    if (FAILED(result))
    {
        return CacheResult::failure(gpu_error("ID3D12Device.CreateGraphicsPipelineState", result));
    }
    result = cache->m_pipeline->SetName(L"CueEngine Fixed Mesh Pipeline");
    if (FAILED(result))
    {
        return CacheResult::failure(gpu_error("ID3D12PipelineState.SetName", result));
    }

    // Slot ごとに 256 Byte を確保し、Shader-visible CBV を分離する
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = k_backBufferCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = a_device.device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cache->m_cbvHeap));
    if (FAILED(result))
    {
        return CacheResult::failure(gpu_error("ID3D12Device.CreateDescriptorHeap.CBV", result));
    }
    result = cache->m_cbvHeap->SetName(L"CueEngine Fixed Mesh CBV Heap");
    if (FAILED(result))
    {
        return CacheResult::failure(gpu_error("ID3D12DescriptorHeap.SetName.CBV", result));
    }
    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 256 * k_backBufferCount;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result = a_device.device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cache->m_constants));
    if (FAILED(result))
    {
        return CacheResult::failure(gpu_error("ID3D12Device.CreateCommittedResource.CBV", result));
    }
    result = cache->m_constants->SetName(L"CueEngine Fixed Mesh Constants");
    if (FAILED(result))
    {
        return CacheResult::failure(gpu_error("ID3D12Resource.SetName.CBV", result));
    }
    result = cache->m_constants->Map(0, nullptr, reinterpret_cast<void**>(&cache->m_mappedConstants));
    if (FAILED(result))
    {
        return CacheResult::failure(gpu_error("ID3D12Resource.Map.CBV", result));
    }
    cache->m_descriptorStride = a_device.device()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = cache->m_cbvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC view{};
        view.BufferLocation = cache->m_constants->GetGPUVirtualAddress() + 256 * index;
        view.SizeInBytes = 256;
        a_device.device()->CreateConstantBufferView(&view, cpu);
        cpu.ptr += cache->m_descriptorStride;
    }
    return CacheResult::success(std::move(cache));
}

/// @brief Fence 完了済み Slot の定数と Shader-visible Descriptor を設定する
Result<void> D3D12PipelineCache::bind(ID3D12GraphicsCommandList* a_list, UINT a_slot,
                                      const std::array<float, 4>& a_tint)
{
    if (!a_list || a_slot >= k_backBufferCount)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12PipelineCache.bind"});
    }
    std::memcpy(m_mappedConstants + 256 * a_slot, a_tint.data(), sizeof(float) * a_tint.size());
    ID3D12DescriptorHeap* heaps[] = {m_cbvHeap.Get()};
    a_list->SetDescriptorHeaps(1, heaps);
    a_list->SetGraphicsRootSignature(m_rootSignature.Get());
    a_list->SetPipelineState(m_pipeline.Get());
    auto gpu = m_cbvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(a_slot) * m_descriptorStride;
    a_list->SetGraphicsRootDescriptorTable(0, gpu);
    return Result<void>::success();
}
} // namespace cue::detail
