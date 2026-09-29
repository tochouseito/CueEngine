#include "D3D12PipelineLibrary.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <d3dcompiler.h>

namespace cue::detail
{
namespace
{
/// @brief 解放後に再利用した Handle を古い世代から分離する
std::uint64_t next_generation(std::uint64_t a_generation) noexcept
{
    ++a_generation;
    return a_generation == 0 ? 1 : a_generation;
}

/// @brief 公開 Stage を DXC Profile に変換する
const wchar_t* shader_profile(GpuShaderStage a_stage) noexcept
{
    switch (a_stage)
    {
    case GpuShaderStage::Vertex:
        return L"vs_6_0";
    case GpuShaderStage::Pixel:
        return L"ps_6_0";
    case GpuShaderStage::Compute:
        return L"cs_6_0";
    default:
        return nullptr;
    }
}

/// @brief Root Table の View 種別を D3D12 Range 種別へ変換する
bool descriptor_type(GpuViewKind a_kind, D3D12_DESCRIPTOR_RANGE_TYPE& a_out) noexcept
{
    switch (a_kind)
    {
    case GpuViewKind::ConstantBuffer:
        a_out = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        return true;
    case GpuViewKind::ShaderResource:
        a_out = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        return true;
    case GpuViewKind::UnorderedAccess:
        a_out = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        return true;
    default:
        return false;
    }
}

/// @brief Vertex 要素の公開 Format を DXGI Format へ変換する
DXGI_FORMAT vertex_format(GpuVertexFormat a_format) noexcept
{
    switch (a_format)
    {
    case GpuVertexFormat::Float2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case GpuVertexFormat::Float3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case GpuVertexFormat::Float4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
} // namespace

/// @brief Device と Queue Pool を借用して Registry を作る
std::unique_ptr<D3D12PipelineLibrary> D3D12PipelineLibrary::create(D3D12DeviceContext& a_device,
                                                                    D3D12QueuePool& a_queues)
{
    auto library = std::make_unique<D3D12PipelineLibrary>();
    library->m_device = a_device.device();
    library->m_queues = &a_queues;
    return library;
}

/// @brief UTF-8 Source を DXC に渡し、診断を保持してから Blob を登録する
Result<GpuShaderHandle> D3D12PipelineLibrary::create_shader(GpuShaderDesc a_desc)
{
    using ShaderResult = Result<GpuShaderHandle>;
    const wchar_t* defaultProfile = shader_profile(a_desc.stage);
    const std::string prefix = a_desc.stage == GpuShaderStage::Vertex ? "vs_"
                               : a_desc.stage == GpuShaderStage::Pixel ? "ps_" : "cs_";
    if (!defaultProfile || a_desc.source.empty() || a_desc.entry.empty() ||
        (!std::isalpha(static_cast<unsigned char>(a_desc.entry.front())) &&
         a_desc.entry.front() != '_') ||
        a_desc.entry.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
            std::string::npos ||
        (!a_desc.profile.empty() &&
         (a_desc.profile.starts_with(prefix) == false ||
          a_desc.profile.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)))
    {
        return ShaderResult::failure({ErrorCategory::InvalidArgument, "D3D12PipelineLibrary.create_shader"});
    }
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    HRESULT result = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(result))
    {
        return ShaderResult::failure(gpu_error("DxcCreateInstance.Utils", result));
    }
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    result = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(result))
    {
        return ShaderResult::failure(gpu_error("DxcCreateInstance.Compiler", result));
    }
    const auto entry = std::wstring(a_desc.entry.begin(), a_desc.entry.end());
    const auto explicitProfile = std::wstring(a_desc.profile.begin(), a_desc.profile.end());
    const wchar_t* profile = a_desc.profile.empty() ? defaultProfile : explicitProfile.c_str();
    const wchar_t* arguments[] = {L"-E", entry.c_str(), L"-T", profile, L"-HV", L"2021"};
    DxcBuffer source{a_desc.source.data(), a_desc.source.size(), DXC_CP_UTF8};
    Microsoft::WRL::ComPtr<IDxcResult> compilation;
    result = compiler->Compile(&source, arguments, static_cast<UINT32>(std::size(arguments)),
                               nullptr, IID_PPV_ARGS(&compilation));
    if (FAILED(result))
    {
        return ShaderResult::failure(gpu_error("IDxcCompiler3.Compile", result));
    }
    HRESULT status = S_OK;
    result = compilation->GetStatus(&status);
    if (FAILED(result))
    {
        return ShaderResult::failure(gpu_error("IDxcResult.GetStatus", result));
    }
    if (FAILED(status))
    {
        Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
        compilation->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        const std::string details = errors ? std::string(errors->GetStringPointer(), errors->GetStringLength())
                                            : std::string{};
        return ShaderResult::failure({ErrorCategory::PlatformFailure, "DXC: " + details,
                                      static_cast<std::int64_t>(status)});
    }
    Microsoft::WRL::ComPtr<IDxcBlob> blob;
    result = compilation->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
    if (FAILED(result))
    {
        return ShaderResult::failure(gpu_error("IDxcResult.GetOutput", result));
    }
    for (std::uint32_t index = 0; index < m_shaders.size(); ++index)
    {
        auto& record = m_shaders[index];
        if (!record.blob)
        {
            record.blob = std::move(blob);
            record.stage = a_desc.stage;
            record.generation = next_generation(record.generation);
            return ShaderResult::success({index, record.generation, this});
        }
    }
    const auto index = static_cast<std::uint32_t>(m_shaders.size());
    m_shaders.push_back({std::move(blob), a_desc.stage, 1});
    return ShaderResult::success({index, 1, this});
}

/// @brief 各 Binding を一つの Root Descriptor Table に変換して登録する
Result<GpuRootSignatureHandle> D3D12PipelineLibrary::create_root_signature(GpuRootSignatureDesc a_desc)
{
    using RootResult = Result<GpuRootSignatureHandle>;
    if (a_desc.bindings.size() > std::numeric_limits<UINT>::max())
    {
        return RootResult::failure({ErrorCategory::InvalidArgument, "D3D12PipelineLibrary.rootCount"});
    }
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(a_desc.bindings.size());
    std::vector<D3D12_ROOT_PARAMETER> parameters(a_desc.bindings.size());
    for (std::size_t index = 0; index < a_desc.bindings.size(); ++index)
    {
        auto& range = ranges[index];
        if (!descriptor_type(a_desc.bindings[index].kind, range.RangeType))
        {
            return RootResult::failure({ErrorCategory::InvalidArgument, "D3D12PipelineLibrary.rootKind"});
        }
        range.NumDescriptors = 1;
        range.BaseShaderRegister = a_desc.bindings[index].shaderRegister;
        range.RegisterSpace = a_desc.bindings[index].registerSpace;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        auto& parameter = parameters[index];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT serialize = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                           &serialized, &errors);
    if (FAILED(serialize))
    {
        const std::string details = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                                          errors->GetBufferSize()) : std::string{};
        return RootResult::failure({ErrorCategory::PlatformFailure, "D3D12SerializeRootSignature: " + details,
                                    static_cast<std::int64_t>(serialize)});
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    const HRESULT result = m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                          serialized->GetBufferSize(), IID_PPV_ARGS(&signature));
    if (FAILED(result))
    {
        return RootResult::failure(gpu_error("ID3D12Device.CreateRootSignature", result));
    }
    for (std::uint32_t index = 0; index < m_roots.size(); ++index)
    {
        auto& record = m_roots[index];
        if (!record.signature)
        {
            record.signature = std::move(signature);
            record.bindings = std::move(a_desc.bindings);
            record.generation = next_generation(record.generation);
            return RootResult::success({index, record.generation, this});
        }
    }
    const auto index = static_cast<std::uint32_t>(m_roots.size());
    m_roots.push_back({std::move(signature), std::move(a_desc.bindings), 1});
    return RootResult::success({index, 1, this});
}

/// @brief Graphics 設定と依存 Handle を検証して PSO を生成する
Result<GpuPipelineHandle> D3D12PipelineLibrary::create_graphics_pipeline(GpuGraphicsPipelineDesc a_desc)
{
    using PipelineResult = Result<GpuPipelineHandle>;
    if (!owns(a_desc.rootSignature) || !owns(a_desc.vertexShader) || !owns(a_desc.pixelShader) ||
        m_shaders[a_desc.vertexShader.index].stage != GpuShaderStage::Vertex ||
        m_shaders[a_desc.pixelShader.index].stage != GpuShaderStage::Pixel ||
        a_desc.colorFormat != GpuTextureFormat::Rgba8Unorm ||
        (a_desc.hasDepth && a_desc.depthFormat != GpuTextureFormat::Depth32Float) ||
        static_cast<unsigned>(a_desc.cullMode) > static_cast<unsigned>(GpuCullMode::Front) ||
        static_cast<unsigned>(a_desc.blendMode) > static_cast<unsigned>(GpuBlendMode::Alpha) ||
        a_desc.vertexElements.size() > std::numeric_limits<UINT>::max())
    {
        return PipelineResult::failure({ErrorCategory::InvalidArgument,
                                        "D3D12PipelineLibrary.create_graphics_pipeline"});
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    elements.reserve(a_desc.vertexElements.size());
    for (const auto& element : a_desc.vertexElements)
    {
        const auto format = vertex_format(element.format);
        if (element.semantic.empty() || format == DXGI_FORMAT_UNKNOWN)
        {
            return PipelineResult::failure({ErrorCategory::InvalidArgument,
                                            "D3D12PipelineLibrary.vertexElement"});
        }
        elements.push_back({element.semantic.c_str(), element.semanticIndex, format, element.slot,
                            element.byteOffset, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_roots[a_desc.rootSignature.index].signature.Get();
    const auto& vertex = m_shaders[a_desc.vertexShader.index].blob;
    const auto& pixel = m_shaders[a_desc.pixelShader.index].blob;
    desc.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    desc.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.NumRenderTargets = 1;
    desc.DSVFormat = a_desc.hasDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = a_desc.cullMode == GpuCullMode::Back ? D3D12_CULL_MODE_BACK
                                 : a_desc.cullMode == GpuCullMode::Front ? D3D12_CULL_MODE_FRONT
                                                                          : D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = true;
    auto& blend = desc.BlendState.RenderTarget[0];
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (a_desc.blendMode == GpuBlendMode::Alpha)
    {
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    desc.DepthStencilState.DepthEnable = a_desc.hasDepth;
    desc.DepthStencilState.DepthWriteMask = a_desc.hasDepth ? D3D12_DEPTH_WRITE_MASK_ALL
                                                            : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    const HRESULT result = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
    if (FAILED(result))
    {
        return PipelineResult::failure(gpu_error("ID3D12Device.CreateGraphicsPipelineState", result));
    }
    return PipelineResult::success(store_pipeline({std::move(state), a_desc.rootSignature,
                                                  a_desc.vertexShader, a_desc.pixelShader,
                                                  GpuPipelineKind::Graphics}));
}

/// @brief Compute Shader と Root Signature から PSO を生成する
Result<GpuPipelineHandle> D3D12PipelineLibrary::create_compute_pipeline(GpuComputePipelineDesc a_desc)
{
    using PipelineResult = Result<GpuPipelineHandle>;
    if (!owns(a_desc.rootSignature) || !owns(a_desc.computeShader) ||
        m_shaders[a_desc.computeShader.index].stage != GpuShaderStage::Compute)
    {
        return PipelineResult::failure({ErrorCategory::InvalidArgument,
                                        "D3D12PipelineLibrary.create_compute_pipeline"});
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_roots[a_desc.rootSignature.index].signature.Get();
    const auto& shader = m_shaders[a_desc.computeShader.index].blob;
    desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    const HRESULT result = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&state));
    if (FAILED(result))
    {
        return PipelineResult::failure(gpu_error("ID3D12Device.CreateComputePipelineState", result));
    }
    return PipelineResult::success(store_pipeline({std::move(state), a_desc.rootSignature,
                                                  a_desc.computeShader, {}, GpuPipelineKind::Compute}));
}

/// @brief PSO が保持する Shader 参照を調べてから Blob を失効させる
Result<void> D3D12PipelineLibrary::destroy_shader(GpuShaderHandle a_shader)
{
    if (!owns(a_shader))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12PipelineLibrary.destroy_shader"});
    }
    for (const auto& pipeline : m_pipelines)
    {
        if (pipeline.state &&
            ((pipeline.firstShader.index == a_shader.index &&
              pipeline.firstShader.generation == a_shader.generation) ||
             (pipeline.secondShader.owner && pipeline.secondShader.index == a_shader.index &&
              pipeline.secondShader.generation == a_shader.generation)))
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "D3D12PipelineLibrary.shaderInUse"});
        }
    }
    m_shaders[a_shader.index].blob.Reset();
    return Result<void>::success();
}

/// @brief PSO が保持する Root 参照を調べてから失効させる
Result<void> D3D12PipelineLibrary::destroy_root_signature(GpuRootSignatureHandle a_root)
{
    if (!owns(a_root))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12PipelineLibrary.destroy_root"});
    }
    for (const auto& pipeline : m_pipelines)
    {
        if (pipeline.state && pipeline.root.index == a_root.index &&
            pipeline.root.generation == a_root.generation)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "D3D12PipelineLibrary.rootInUse"});
        }
    }
    m_roots[a_root.index].signature.Reset();
    m_roots[a_root.index].bindings.clear();
    return Result<void>::success();
}

/// @brief 先行する全 Queue の GPU 作業完了後に PSO を解放する
Result<void> D3D12PipelineLibrary::destroy_pipeline(GpuPipelineHandle a_pipeline)
{
    if (!owns(a_pipeline))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12PipelineLibrary.destroy_pipeline"});
    }
    auto idle = m_queues->wait_idle();
    if (!idle.has_value())
    {
        return idle;
    }
    m_pipelines[a_pipeline.index].state.Reset();
    return Result<void>::success();
}

/// @brief 有効な Graphics PSO と対応 Root Signature を設定する
Result<void> D3D12PipelineLibrary::bind_graphics(ID3D12GraphicsCommandList* a_list,
                                                  GpuPipelineHandle a_pipeline) const
{
    if (!a_list || !owns(a_pipeline) || a_pipeline.kind != GpuPipelineKind::Graphics)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12PipelineLibrary.bind_graphics"});
    }
    const auto& pipeline = m_pipelines[a_pipeline.index];
    a_list->SetGraphicsRootSignature(m_roots[pipeline.root.index].signature.Get());
    a_list->SetPipelineState(pipeline.state.Get());
    return Result<void>::success();
}

/// @brief 有効な Compute PSO と対応 Root Signature を設定する
Result<void> D3D12PipelineLibrary::bind_compute(ID3D12GraphicsCommandList* a_list,
                                                 GpuPipelineHandle a_pipeline) const
{
    if (!a_list || !owns(a_pipeline) || a_pipeline.kind != GpuPipelineKind::Compute)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12PipelineLibrary.bind_compute"});
    }
    const auto& pipeline = m_pipelines[a_pipeline.index];
    a_list->SetComputeRootSignature(m_roots[pipeline.root.index].signature.Get());
    a_list->SetPipelineState(pipeline.state.Get());
    return Result<void>::success();
}

/// @brief 有効な PSO の Root Table が View の種類と一致するか調べる
bool D3D12PipelineLibrary::accepts_view(GpuPipelineHandle a_pipeline, std::uint32_t a_parameter,
                                        GpuViewKind a_kind) const noexcept
{
    if (!owns(a_pipeline))
    {
        return false;
    }
    const auto& bindings = m_roots[m_pipelines[a_pipeline.index].root.index].bindings;
    return a_parameter < bindings.size() && bindings[a_parameter].kind == a_kind;
}

/// @brief Shader Handle がこの Registry の生存 Blob を指すか調べる
bool D3D12PipelineLibrary::owns(GpuShaderHandle a_shader) const noexcept
{
    return a_shader.owner == this && a_shader.index < m_shaders.size() &&
           m_shaders[a_shader.index].generation == a_shader.generation && m_shaders[a_shader.index].blob;
}

/// @brief Root Handle がこの Registry の生存 Signature を指すか調べる
bool D3D12PipelineLibrary::owns(GpuRootSignatureHandle a_root) const noexcept
{
    return a_root.owner == this && a_root.index < m_roots.size() &&
           m_roots[a_root.index].generation == a_root.generation && m_roots[a_root.index].signature;
}

/// @brief PSO Handle がこの Registry の生存 PSO を指すか調べる
bool D3D12PipelineLibrary::owns(GpuPipelineHandle a_pipeline) const noexcept
{
    return a_pipeline.owner == this && a_pipeline.index < m_pipelines.size() &&
           m_pipelines[a_pipeline.index].generation == a_pipeline.generation &&
           m_pipelines[a_pipeline.index].kind == a_pipeline.kind && m_pipelines[a_pipeline.index].state;
}

/// @brief 空いた Slot を世代更新して PSO を再登録する
GpuPipelineHandle D3D12PipelineLibrary::store_pipeline(PipelineRecord a_record)
{
    for (std::uint32_t index = 0; index < m_pipelines.size(); ++index)
    {
        auto& record = m_pipelines[index];
        if (!record.state)
        {
            a_record.generation = next_generation(record.generation);
            record = std::move(a_record);
            return {index, record.generation, record.kind, this};
        }
    }
    const auto index = static_cast<std::uint32_t>(m_pipelines.size());
    a_record.generation = 1;
    m_pipelines.push_back(std::move(a_record));
    return {index, 1, m_pipelines.back().kind, this};
}
} // namespace cue::detail
