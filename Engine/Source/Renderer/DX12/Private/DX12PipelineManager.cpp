#include "DX12PipelineManager.h"

#include <array>
#include <cctype>
#include <cstddef>
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

/// @brief Vertex 要素の公開 Format を DXGI Format へ変換する
DXGI_FORMAT vertex_format(GpuVertexFormat a_format) noexcept
{
    switch (a_format)
    {
    case GpuVertexFormat::Float1:
        return DXGI_FORMAT_R32_FLOAT;
    case GpuVertexFormat::Float2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case GpuVertexFormat::Float3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case GpuVertexFormat::Float4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case GpuVertexFormat::Uint4:
        return DXGI_FORMAT_R32G32B32A32_UINT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

/// @brief 公開 Texture Format を Pipeline の RTV／DSV Format へ変換する
DXGI_FORMAT texture_format(GpuTextureFormat a_format) noexcept
{
    switch (a_format)
    {
    case GpuTextureFormat::Rgba8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case GpuTextureFormat::Rgba8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case GpuTextureFormat::Bc6hUf16: return DXGI_FORMAT_BC6H_UF16;
    case GpuTextureFormat::Bc7Unorm: return DXGI_FORMAT_BC7_UNORM;
    case GpuTextureFormat::Bc7UnormSrgb: return DXGI_FORMAT_BC7_UNORM_SRGB;
    case GpuTextureFormat::R32Uint: return DXGI_FORMAT_R32_UINT;
    case GpuTextureFormat::Depth32Float: return DXGI_FORMAT_D32_FLOAT;
    case GpuTextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case GpuTextureFormat::R24UnormX8Typeless: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

/// @brief 一つの RTV の Legacy Blend Mode を Native Target 設定へ反映する
void set_blend(D3D12_RENDER_TARGET_BLEND_DESC& a_target, GpuBlendMode a_mode) noexcept
{
    a_target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (a_mode == GpuBlendMode::Opaque)
    {
        return;
    }
    a_target.BlendEnable = TRUE;
    a_target.BlendOp = D3D12_BLEND_OP_ADD;
    a_target.SrcBlendAlpha = D3D12_BLEND_ONE;
    a_target.DestBlendAlpha = a_mode == GpuBlendMode::Additive ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
    a_target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    a_target.SrcBlend = a_mode == GpuBlendMode::Additive ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA;
    a_target.DestBlend = a_mode == GpuBlendMode::Additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
}
} // namespace

/// @brief Device と Queue Pool を借用して Registry を作る
Result<std::unique_ptr<DX12PipelineManager>> DX12PipelineManager::create(DX12RenderDevice& a_device,
                                                                            DX12QueuePool& a_queues)
{
    using ManagerResult = Result<std::unique_ptr<DX12PipelineManager>>;
    auto compilerResult = HLSLCompiler::create();
    if (!compilerResult.has_value())
    {
        return ManagerResult::failure(*compilerResult.try_error());
    }
    auto library = std::make_unique<DX12PipelineManager>();
    library->m_device = a_device.device();
    library->m_queues = &a_queues;
    library->m_compiler = compilerResult.take_value();
    return ManagerResult::success(std::move(library));
}

/// @brief UTF-8 Source を DXC に渡し、診断を保持してから Blob を登録する
Result<GpuShaderHandle> DX12PipelineManager::create_shader(GpuShaderDesc a_desc)
{
    using ShaderResult = Result<GpuShaderHandle>;
    if (!a_desc.name.empty() && m_namedShaders.contains(a_desc.name))
    {
        return ShaderResult::failure({ErrorCategory::InvalidArgument,
                                      "DX12PipelineManager.create_shader.name"});
    }
    auto blobResult = m_compiler->compile_shader_raw(a_desc);
    if (!blobResult.has_value())
    {
        return ShaderResult::failure(*blobResult.try_error());
    }
    auto blob = blobResult.take_value();
    GpuShaderHandle handle{};
    for (std::uint32_t index = 0; index < m_shaders.size(); ++index)
    {
        auto& record = m_shaders[index];
        if (!record.blob)
        {
            record.blob = std::move(blob);
            record.stage = a_desc.stage;
            record.generation = next_generation(record.generation);
            handle = {index, record.generation, this};
            break;
        }
    }
    if (!handle.owner)
    {
        const auto index = static_cast<std::uint32_t>(m_shaders.size());
        m_shaders.push_back({std::move(blob), a_desc.stage, 1});
        handle = {index, 1, this};
    }
    if (!a_desc.name.empty())
    {
        m_namedShaders.emplace(std::move(a_desc.name), handle);
    }
    return ShaderResult::success(handle);
}

/// @brief Legacy の Root Parameter と既存 Binding を一つの Signature 契約に変換する
Result<GpuRootSignatureHandle> DX12PipelineManager::create_root_signature(GpuRootSignatureDesc a_desc)
{
    using RootResult = Result<GpuRootSignatureHandle>;
    if (!a_desc.name.empty() && m_namedRoots.contains(a_desc.name))
    {
        return RootResult::failure({ErrorCategory::InvalidArgument,
                                    "DX12PipelineManager.create_root_signature.name"});
    }
    if ((!a_desc.bindings.empty() && !a_desc.parameters.empty()) ||
        a_desc.bindings.size() > std::numeric_limits<UINT>::max() ||
        a_desc.parameters.size() > std::numeric_limits<UINT>::max())
    {
        return RootResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.rootCount"});
    }
    if (a_desc.parameters.empty())
    {
        for (const auto& binding : a_desc.bindings)
        {
            GpuRootParameterType type{};
            switch (binding.kind)
            {
            case GpuViewKind::ConstantBuffer: type = GpuRootParameterType::TableCbv; break;
            case GpuViewKind::ShaderResource: type = GpuRootParameterType::TableSrv; break;
            case GpuViewKind::UnorderedAccess: type = GpuRootParameterType::TableUav; break;
            default:
                return RootResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.rootKind"});
            }
            a_desc.parameters.push_back({type, GpuShaderVisibility::All,
                                         binding.shaderRegister, 1, binding.registerSpace});
        }
    }
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(a_desc.parameters.size());
    std::vector<D3D12_ROOT_PARAMETER> parameters(a_desc.parameters.size());
    for (std::size_t index = 0; index < a_desc.parameters.size(); ++index)
    {
        const auto& source = a_desc.parameters[index];
        auto& parameter = parameters[index];
        switch (source.visibility)
        {
        case GpuShaderVisibility::All: parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; break;
        case GpuShaderVisibility::Vertex: parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX; break;
        case GpuShaderVisibility::Pixel: parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; break;
        default:
            return RootResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.rootVisibility"});
        }
        switch (source.type)
        {
        case GpuRootParameterType::Cbv: parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; break;
        case GpuRootParameterType::Srv: parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; break;
        case GpuRootParameterType::Uav: parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; break;
        case GpuRootParameterType::Constants32:
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameter.Constants.Num32BitValues = 1;
            parameter.Constants.ShaderRegister = source.shaderRegister;
            parameter.Constants.RegisterSpace = source.registerSpace;
            continue;
        case GpuRootParameterType::TableCbv:
        case GpuRootParameterType::TableSrv:
        case GpuRootParameterType::TableUav:
        {
            if (source.descriptorCount == 0)
            {
                return RootResult::failure({ErrorCategory::InvalidArgument,
                                            "DX12PipelineManager.rootDescriptorCount"});
            }
            auto& range = ranges[index];
            range.RangeType = source.type == GpuRootParameterType::TableCbv ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                            : source.type == GpuRootParameterType::TableSrv ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                                                                              : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            range.NumDescriptors = source.descriptorCount;
            range.BaseShaderRegister = source.shaderRegister;
            range.RegisterSpace = source.registerSpace;
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable.NumDescriptorRanges = 1;
            parameter.DescriptorTable.pDescriptorRanges = &range;
            continue;
        }
        default:
            return RootResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.rootType"});
        }
        parameter.Descriptor.ShaderRegister = source.shaderRegister;
        parameter.Descriptor.RegisterSpace = source.registerSpace;
    }
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
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
    GpuRootSignatureHandle handle{};
    for (std::uint32_t index = 0; index < m_roots.size(); ++index)
    {
        auto& record = m_roots[index];
        if (!record.signature)
        {
            record.signature = std::move(signature);
            record.parameters = std::move(a_desc.parameters);
            record.generation = next_generation(record.generation);
            handle = {index, record.generation, this};
            break;
        }
    }
    if (!handle.owner)
    {
        const auto index = static_cast<std::uint32_t>(m_roots.size());
        m_roots.push_back({std::move(signature), std::move(a_desc.parameters), 1});
        handle = {index, 1, this};
    }
    if (!a_desc.name.empty())
    {
        m_namedRoots.emplace(std::move(a_desc.name), handle);
    }
    return RootResult::success(handle);
}

/// @brief Graphics 設定と依存 Handle を検証して PSO を生成する
Result<GpuPipelineHandle> DX12PipelineManager::create_graphics_pipeline(GpuGraphicsPipelineDesc a_desc)
{
    using PipelineResult = Result<GpuPipelineHandle>;
    if (!a_desc.name.empty() && m_namedGraphicsPipelines.contains(a_desc.name))
    {
        return PipelineResult::failure({ErrorCategory::InvalidArgument,
                                        "DX12PipelineManager.create_graphics_pipeline.name"});
    }
    if (!owns(a_desc.rootSignature) || !owns(a_desc.vertexShader) || !owns(a_desc.pixelShader) ||
        m_shaders[a_desc.vertexShader.index].stage != GpuShaderStage::Vertex ||
        m_shaders[a_desc.pixelShader.index].stage != GpuShaderStage::Pixel ||
        (a_desc.hasDepth && a_desc.depthFormat != GpuTextureFormat::Depth32Float &&
         a_desc.depthFormat != GpuTextureFormat::Depth24Stencil8) ||
        static_cast<unsigned>(a_desc.cullMode) > static_cast<unsigned>(GpuCullMode::Front) ||
        static_cast<unsigned>(a_desc.blendMode) > static_cast<unsigned>(GpuBlendMode::Additive) ||
        static_cast<unsigned>(a_desc.fillMode) > static_cast<unsigned>(GpuFillMode::Wireframe) ||
        static_cast<unsigned>(a_desc.topology) > static_cast<unsigned>(GpuPrimitiveTopology::Triangle) ||
        a_desc.vertexElements.size() > std::numeric_limits<UINT>::max() ||
        a_desc.colorFormats.size() > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
        a_desc.blendModes.size() > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
        a_desc.sampleCount == 0)
    {
        return PipelineResult::failure({ErrorCategory::InvalidArgument,
                                        "DX12PipelineManager.create_graphics_pipeline"});
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    elements.reserve(a_desc.vertexElements.size());
    for (const auto& element : a_desc.vertexElements)
    {
        const auto format = vertex_format(element.format);
        if (element.semantic.empty() || format == DXGI_FORMAT_UNKNOWN)
        {
            return PipelineResult::failure({ErrorCategory::InvalidArgument,
                                            "DX12PipelineManager.vertexElement"});
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
    desc.PrimitiveTopologyType = a_desc.topology == GpuPrimitiveTopology::Point
                                     ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                 : a_desc.topology == GpuPrimitiveTopology::Line
                                     ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                     : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    const auto targetCount = !a_desc.colorFormats.empty() ? a_desc.colorFormats.size()
                           : a_desc.useColorTarget ? std::size_t{1} : std::size_t{0};
    if (!a_desc.blendModes.empty() && a_desc.blendModes.size() != targetCount)
    {
        return PipelineResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.blendCount"});
    }
    for (std::size_t index = 0; index < targetCount; ++index)
    {
        const auto format = texture_format(a_desc.colorFormats.empty() ? a_desc.colorFormat
                                                                        : a_desc.colorFormats[index]);
        if (format == DXGI_FORMAT_UNKNOWN || format == DXGI_FORMAT_D32_FLOAT ||
            format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
            format == DXGI_FORMAT_BC6H_UF16 || format == DXGI_FORMAT_BC7_UNORM ||
            format == DXGI_FORMAT_BC7_UNORM_SRGB)
        {
            return PipelineResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.colorFormat"});
        }
        desc.RTVFormats[index] = format;
        const auto blendMode = a_desc.blendModes.empty() ? a_desc.blendMode : a_desc.blendModes[index];
        if (static_cast<unsigned>(blendMode) > static_cast<unsigned>(GpuBlendMode::Additive))
        {
            return PipelineResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.blendMode"});
        }
        set_blend(desc.BlendState.RenderTarget[index], blendMode);
    }
    desc.NumRenderTargets = static_cast<UINT>(targetCount);
    desc.BlendState.IndependentBlendEnable = targetCount > 1;
    desc.DSVFormat = a_desc.hasDepth ? texture_format(a_desc.depthFormat) : DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = a_desc.sampleCount;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = a_desc.fillMode == GpuFillMode::Wireframe
                                        ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = a_desc.cullMode == GpuCullMode::Back ? D3D12_CULL_MODE_BACK
                                 : a_desc.cullMode == GpuCullMode::Front ? D3D12_CULL_MODE_FRONT
                                                                          : D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = a_desc.frontCounterClockwise;
    desc.RasterizerState.DepthClipEnable = a_desc.depthClip;
    desc.RasterizerState.DepthBias = a_desc.depthBias;
    desc.RasterizerState.DepthBiasClamp = a_desc.depthBiasClamp;
    desc.RasterizerState.SlopeScaledDepthBias = a_desc.slopeScaledDepthBias;
    desc.DepthStencilState.DepthEnable = a_desc.hasDepth;
    desc.DepthStencilState.DepthWriteMask = a_desc.hasDepth && a_desc.depthWrite
                                               ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    const HRESULT result = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
    if (FAILED(result))
    {
        return PipelineResult::failure(gpu_error("ID3D12Device.CreateGraphicsPipelineState", result));
    }
    const auto handle = store_pipeline({std::move(state), a_desc.rootSignature,
                                        a_desc.vertexShader, a_desc.pixelShader,
                                        GpuPipelineKind::Graphics});
    if (!a_desc.name.empty())
    {
        m_namedGraphicsPipelines.emplace(std::move(a_desc.name), handle);
    }
    return PipelineResult::success(handle);
}

/// @brief Compute Shader と Root Signature から PSO を生成する
Result<GpuPipelineHandle> DX12PipelineManager::create_compute_pipeline(GpuComputePipelineDesc a_desc)
{
    using PipelineResult = Result<GpuPipelineHandle>;
    if (!a_desc.name.empty() && m_namedComputePipelines.contains(a_desc.name))
    {
        return PipelineResult::failure({ErrorCategory::InvalidArgument,
                                        "DX12PipelineManager.create_compute_pipeline.name"});
    }
    if (!owns(a_desc.rootSignature) || !owns(a_desc.computeShader) ||
        m_shaders[a_desc.computeShader.index].stage != GpuShaderStage::Compute)
    {
        return PipelineResult::failure({ErrorCategory::InvalidArgument,
                                        "DX12PipelineManager.create_compute_pipeline"});
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
    const auto handle = store_pipeline({std::move(state), a_desc.rootSignature,
                                        a_desc.computeShader, {}, GpuPipelineKind::Compute});
    if (!a_desc.name.empty())
    {
        m_namedComputePipelines.emplace(std::move(a_desc.name), handle);
    }
    return PipelineResult::success(handle);
}

/// @brief 名前付き Shader の現行世代を返す
Result<GpuShaderHandle> DX12PipelineManager::get_shader(std::string_view a_name) const
{
    const auto it = m_namedShaders.find(std::string(a_name));
    if (it == m_namedShaders.end() || !owns(it->second))
    {
        return Result<GpuShaderHandle>::failure({ErrorCategory::InvalidArgument,
                                                  "DX12PipelineManager.get_shader"});
    }
    return Result<GpuShaderHandle>::success(it->second);
}

/// @brief 名前付き Root Signature の現行世代を返す
Result<GpuRootSignatureHandle> DX12PipelineManager::get_root_signature(std::string_view a_name) const
{
    const auto it = m_namedRoots.find(std::string(a_name));
    if (it == m_namedRoots.end() || !owns(it->second))
    {
        return Result<GpuRootSignatureHandle>::failure({ErrorCategory::InvalidArgument,
                                                         "DX12PipelineManager.get_root_signature"});
    }
    return Result<GpuRootSignatureHandle>::success(it->second);
}

/// @brief 名前付き Graphics PSO の現行世代を返す
Result<GpuPipelineHandle> DX12PipelineManager::get_graphics_pipeline(std::string_view a_name) const
{
    const auto it = m_namedGraphicsPipelines.find(std::string(a_name));
    if (it == m_namedGraphicsPipelines.end() || !owns(it->second))
    {
        return Result<GpuPipelineHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12PipelineManager.get_graphics_pipeline"});
    }
    return Result<GpuPipelineHandle>::success(it->second);
}

/// @brief 名前付き Compute PSO の現行世代を返す
Result<GpuPipelineHandle> DX12PipelineManager::get_compute_pipeline(std::string_view a_name) const
{
    const auto it = m_namedComputePipelines.find(std::string(a_name));
    if (it == m_namedComputePipelines.end() || !owns(it->second))
    {
        return Result<GpuPipelineHandle>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12PipelineManager.get_compute_pipeline"});
    }
    return Result<GpuPipelineHandle>::success(it->second);
}

/// @brief PSO が保持する Shader 参照を調べてから Blob を失効させる
Result<void> DX12PipelineManager::destroy_shader(GpuShaderHandle a_shader)
{
    if (!owns(a_shader))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12PipelineManager.destroy_shader"});
    }
    for (const auto& pipeline : m_pipelines)
    {
        if (pipeline.state &&
            ((pipeline.firstShader.index == a_shader.index &&
              pipeline.firstShader.generation == a_shader.generation) ||
             (pipeline.secondShader.owner && pipeline.secondShader.index == a_shader.index &&
              pipeline.secondShader.generation == a_shader.generation)))
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12PipelineManager.shaderInUse"});
        }
    }
    m_shaders[a_shader.index].blob.Reset();
    std::erase_if(m_namedShaders, [a_shader](const auto& a_entry) {
        return a_entry.second.owner == a_shader.owner && a_entry.second.index == a_shader.index &&
               a_entry.second.generation == a_shader.generation;
    });
    return Result<void>::success();
}

/// @brief PSO が保持する Root 参照を調べてから失効させる
Result<void> DX12PipelineManager::destroy_root_signature(GpuRootSignatureHandle a_root)
{
    if (!owns(a_root))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12PipelineManager.destroy_root"});
    }
    for (const auto& pipeline : m_pipelines)
    {
        if (pipeline.state && pipeline.root.index == a_root.index &&
            pipeline.root.generation == a_root.generation)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12PipelineManager.rootInUse"});
        }
    }
    m_roots[a_root.index].signature.Reset();
    m_roots[a_root.index].parameters.clear();
    std::erase_if(m_namedRoots, [a_root](const auto& a_entry) {
        return a_entry.second.owner == a_root.owner && a_entry.second.index == a_root.index &&
               a_entry.second.generation == a_root.generation;
    });
    return Result<void>::success();
}

/// @brief 先行する全 Queue の GPU 作業完了後に PSO を解放する
Result<void> DX12PipelineManager::destroy_pipeline(GpuPipelineHandle a_pipeline)
{
    if (!owns(a_pipeline))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12PipelineManager.destroy_pipeline"});
    }
    auto idle = m_queues->wait_idle();
    if (!idle.has_value())
    {
        return idle;
    }
    m_pipelines[a_pipeline.index].state.Reset();
    m_pipelines[a_pipeline.index].indirectSignature.Reset();
    auto& names = a_pipeline.kind == GpuPipelineKind::Graphics ? m_namedGraphicsPipelines
                                                                : m_namedComputePipelines;
    std::erase_if(names, [a_pipeline](const auto& a_entry) {
        return a_entry.second.owner == a_pipeline.owner && a_entry.second.index == a_pipeline.index &&
               a_entry.second.generation == a_pipeline.generation;
    });
    return Result<void>::success();
}

/// @brief 有効な Graphics PSO と対応 Root Signature を設定する
Result<void> DX12PipelineManager::bind_graphics(ID3D12GraphicsCommandList* a_list,
                                                  GpuPipelineHandle a_pipeline) const
{
    if (!a_list || !owns(a_pipeline) || a_pipeline.kind != GpuPipelineKind::Graphics)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12PipelineManager.bind_graphics"});
    }
    const auto& pipeline = m_pipelines[a_pipeline.index];
    a_list->SetGraphicsRootSignature(m_roots[pipeline.root.index].signature.Get());
    a_list->SetPipelineState(pipeline.state.Get());
    return Result<void>::success();
}

/// @brief 有効な Compute PSO と対応 Root Signature を設定する
Result<void> DX12PipelineManager::bind_compute(ID3D12GraphicsCommandList* a_list,
                                                 GpuPipelineHandle a_pipeline) const
{
    if (!a_list || !owns(a_pipeline) || a_pipeline.kind != GpuPipelineKind::Compute)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12PipelineManager.bind_compute"});
    }
    const auto& pipeline = m_pipelines[a_pipeline.index];
    a_list->SetComputeRootSignature(m_roots[pipeline.root.index].signature.Get());
    a_list->SetPipelineState(pipeline.state.Get());
    return Result<void>::success();
}

/// @brief 有効な PSO の Root Table が View の種類と一致するか調べる
bool DX12PipelineManager::accepts_view(GpuPipelineHandle a_pipeline, std::uint32_t a_parameter,
                                        GpuViewKind a_kind) const noexcept
{
    if (!owns(a_pipeline))
    {
        return false;
    }
    const auto& parameters = m_roots[m_pipelines[a_pipeline.index].root.index].parameters;
    if (a_parameter >= parameters.size())
    {
        return false;
    }
    const auto type = parameters[a_parameter].type;
    return (type == GpuRootParameterType::TableCbv && a_kind == GpuViewKind::ConstantBuffer) ||
           (type == GpuRootParameterType::TableSrv && a_kind == GpuViewKind::ShaderResource) ||
           (type == GpuRootParameterType::TableUav && a_kind == GpuViewKind::UnorderedAccess);
}

/// @brief Root Descriptor と 32 bit 定数の Bind 前に型を検証する
bool DX12PipelineManager::accepts_root_parameter(GpuPipelineHandle a_pipeline,
    std::uint32_t a_parameter, GpuRootParameterType a_type) const noexcept
{
    if (!owns(a_pipeline))
    {
        return false;
    }
    const auto& parameters = m_roots[m_pipelines[a_pipeline.index].root.index].parameters;
    return a_parameter < parameters.size() && parameters[a_parameter].type == a_type;
}

/// @brief Root Table の宣言長が Texture 用領域を超えないようにする
bool DX12PipelineManager::accepts_texture_table(GpuPipelineHandle a_pipeline,
    std::uint32_t a_parameter, UINT a_capacity) const noexcept
{
    if (!accepts_root_parameter(a_pipeline, a_parameter, GpuRootParameterType::TableSrv))
    {
        return false;
    }
    const auto& parameters = m_roots[m_pipelines[a_pipeline.index].root.index].parameters;
    return parameters[a_parameter].descriptorCount <= a_capacity;
}

/// @brief Legacy の定数一つと DrawIndexed を持つ Signature を PSO の Root に結び付ける
Result<ID3D12CommandSignature*> DX12PipelineManager::indirect_signature(GpuPipelineHandle a_pipeline)
{
    if (!accepts_root_parameter(a_pipeline, 0, GpuRootParameterType::Constants32) ||
        a_pipeline.kind != GpuPipelineKind::Graphics)
    {
        return Result<ID3D12CommandSignature*>::failure({ErrorCategory::InvalidArgument,
                                                           "DX12PipelineManager.indirect_signature"});
    }
    auto& pipeline = m_pipelines[a_pipeline.index];
    if (!pipeline.indirectSignature)
    {
        D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
        arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        arguments[0].Constant.RootParameterIndex = 0;
        arguments[0].Constant.Num32BitValuesToSet = 1;
        arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = sizeof(std::uint32_t) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        desc.NumArgumentDescs = 2;
        desc.pArgumentDescs = arguments;
        const HRESULT result = m_device->CreateCommandSignature(&desc,
            m_roots[pipeline.root.index].signature.Get(), IID_PPV_ARGS(&pipeline.indirectSignature));
        if (FAILED(result))
        {
            return Result<ID3D12CommandSignature*>::failure(
                gpu_error("ID3D12Device.CreateCommandSignature", result));
        }
    }
    return Result<ID3D12CommandSignature*>::success(pipeline.indirectSignature.Get());
}

/// @brief Shader Handle がこの Registry の生存 Blob を指すか調べる
bool DX12PipelineManager::owns(GpuShaderHandle a_shader) const noexcept
{
    return a_shader.owner == this && a_shader.index < m_shaders.size() &&
           m_shaders[a_shader.index].generation == a_shader.generation && m_shaders[a_shader.index].blob;
}

/// @brief Root Handle がこの Registry の生存 Signature を指すか調べる
bool DX12PipelineManager::owns(GpuRootSignatureHandle a_root) const noexcept
{
    return a_root.owner == this && a_root.index < m_roots.size() &&
           m_roots[a_root.index].generation == a_root.generation && m_roots[a_root.index].signature;
}

/// @brief PSO Handle がこの Registry の生存 PSO を指すか調べる
bool DX12PipelineManager::owns(GpuPipelineHandle a_pipeline) const noexcept
{
    return a_pipeline.owner == this && a_pipeline.index < m_pipelines.size() &&
           m_pipelines[a_pipeline.index].generation == a_pipeline.generation &&
           m_pipelines[a_pipeline.index].kind == a_pipeline.kind && m_pipelines[a_pipeline.index].state;
}

/// @brief 空いた Slot を世代更新して PSO を再登録する
GpuPipelineHandle DX12PipelineManager::store_pipeline(PipelineRecord a_record)
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
