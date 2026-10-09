#include <DX12/DX12PipelineManager.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12RenderDevice.h>
#include <Foundation/Windows/UtfConversion.h>

#include "DX12ShaderCompiler.h"

namespace cue::dx12
{
namespace
{
std::atomic<std::uint64_t> g_nextManagerId = 1;

/// @brief Registry の Slot を再利用しても古い Handle を復活させない
template <typename T> struct Slot final
{
    std::uint64_t generation = 1;
    std::shared_ptr<T> value;
};
/// @brief Root の構成と Native 実体を依存 PSO と共有する
struct RootRecord final
{
    RootSignatureDesc desc;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> native;
};
/// @brief Shader のコンパイル設定と CPU Blob を所有する
struct ShaderRecord final
{
    ShaderCompileDesc desc;
    Microsoft::WRL::ComPtr<IDxcBlob> native;
};
/// @brief Command が Manager と独立して PSO と対応 Root を維持する
struct PipelineRecord final
{
    bool isCompute = false;
    GpuTextureFormat format = GpuTextureFormat::Rgba8Unorm;
    PrimitiveTopology topology = PrimitiveTopology::Triangle;
    std::shared_ptr<RootRecord> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> native;
};

/// @brief Native 失敗値を操作名とともに保持する
Error pipeline_error(const char *a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, a_operation, static_cast<std::int64_t>(a_result)};
}
/// @brief 所属、世代と貸出状態を照合して Registry の実体を借用する
template <typename T, typename H>
std::shared_ptr<T> find_record(const std::vector<Slot<T>> &a_slots, H a_handle, std::uint64_t a_id)
{
    return a_handle.managerId == a_id && a_handle.is_valid() && a_handle.index < a_slots.size() &&
                   a_slots[a_handle.index].generation == a_handle.generation
               ? a_slots[a_handle.index].value
               : nullptr;
}
/// @brief 世代が尽きた Slot を再利用せず、生成物を一度だけ登録する
template <typename H, typename T>
Result<H> insert_record(std::vector<Slot<T>> &a_slots, std::shared_ptr<T> a_record, std::uint64_t a_id)
{
    for (std::size_t index = 0; index < a_slots.size(); ++index)
    {
        auto &slot = a_slots[index];
        if (!slot.value && slot.generation != (std::numeric_limits<std::uint64_t>::max)())
        {
            slot.value = std::move(a_record);
            return Result<H>::success({static_cast<std::uint32_t>(index), slot.generation, a_id});
        }
    }
    if (a_slots.size() >= (std::numeric_limits<std::uint32_t>::max)())
    {
        return Result<H>::failure({ErrorCategory::InvalidState, "DX12PipelineManager.capacity"});
    }
    a_slots.push_back({1, std::move(a_record)});
    return Result<H>::success({static_cast<std::uint32_t>(a_slots.size() - 1), 1, a_id});
}
/// @brief Handle を失効させ、Command や PSO が持つ共有参照へ寿命を引き継ぐ
template <typename T, typename H>
Result<void> retire_record(std::vector<Slot<T>> &a_slots, H a_handle, std::uint64_t a_id)
{
    if (!find_record(a_slots, a_handle, a_id))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.retire.handle"});
    }
    auto &slot = a_slots[a_handle.index];
    slot.value.reset();
    ++slot.generation;
    return Result<void>::success();
}
/// @brief 不正な列挙値を Native API へ渡さない
bool visibility(ShaderVisibility a_visibility, D3D12_SHADER_VISIBILITY &a_native)
{
    switch (a_visibility)
    {
    case ShaderVisibility::All:
        a_native = D3D12_SHADER_VISIBILITY_ALL;
        return true;
    case ShaderVisibility::Vertex:
        a_native = D3D12_SHADER_VISIBILITY_VERTEX;
        return true;
    case ShaderVisibility::Pixel:
        a_native = D3D12_SHADER_VISIBILITY_PIXEL;
        return true;
    }
    return false;
}
/// @brief 現行 Color Format を Native PSO Format に対応させる
DXGI_FORMAT texture_format(GpuTextureFormat a_format)
{
    switch (a_format)
    {
    case GpuTextureFormat::Rgba8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case GpuTextureFormat::Bgra8Unorm:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case GpuTextureFormat::Rgba16Float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case GpuTextureFormat::R32Float:
        return DXGI_FORMAT_R32_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}
/// @brief 生成した Object の名前を UTF-16 へ変換して診断用に設定する
Result<void> name_object(ID3D12Object &a_object, const std::string &a_name)
{
    if (a_name.find('\0') != std::string::npos)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.name"});
    }
    auto converted = utf8_to_utf16(a_name.empty() ? "Cue.PipelineObject" : a_name);
    if (!converted.has_value())
    {
        return Result<void>::failure(*converted.try_error());
    }
    const HRESULT hr = a_object.SetName(converted.try_value()->c_str());
    return FAILED(hr) ? Result<void>::failure(pipeline_error("ID3D12Object.SetName", hr)) : Result<void>::success();
}
} // namespace

struct DX12PipelineManager::State final
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    // Blob より先に配置し、Registry の COM 参照が消えてから Compiler Module を解放する
    std::unique_ptr<DX12ShaderCompiler> compiler;
    std::uint64_t id = 0;
    std::vector<Slot<RootRecord>> roots;
    std::vector<Slot<ShaderRecord>> shaders;
    std::vector<Slot<PipelineRecord>> pipelines;
};

/// @brief Factory が成功するまで空の所有状態を保持する
DX12PipelineManager::DX12PipelineManager(CreateToken) noexcept
{
}
/// @brief Command が参照する PSO と Root は Registry の外でも生存できる
DX12PipelineManager::~DX12PipelineManager() = default;

/// @brief Native Device と DXC の生成基盤が揃った場合だけ Manager を公開する
Result<std::unique_ptr<DX12PipelineManager>> DX12PipelineManager::create(DX12RenderDevice &a_device,
                                                                         IFileSystem *a_files)
{
    using managerResult = Result<std::unique_ptr<DX12PipelineManager>>;
    if (!a_device.device())
    {
        return managerResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.create.device"});
    }
    std::uint64_t id = g_nextManagerId.load(std::memory_order_relaxed);
    while (id != (std::numeric_limits<std::uint64_t>::max)() &&
           !g_nextManagerId.compare_exchange_weak(id, id + 1, std::memory_order_relaxed))
    {
    }
    if (id == (std::numeric_limits<std::uint64_t>::max)())
    {
        return managerResult::failure({ErrorCategory::Fatal, "DX12PipelineManager.id_exhausted"});
    }
    auto compilerResult = DX12ShaderCompiler::create(a_files);
    if (!compilerResult.has_value())
    {
        return managerResult::failure(*compilerResult.try_error());
    }
    try
    {
        auto manager = std::make_unique<DX12PipelineManager>(CreateToken{});
        manager->m_state = std::make_unique<State>();
        manager->m_state->device = a_device.device();
        manager->m_state->id = id;
        manager->m_state->compiler = compilerResult.take_value();
        return managerResult::success(std::move(manager));
    }
    catch (const std::bad_alloc &)
    {
        return managerResult::failure({ErrorCategory::PlatformFailure, "DX12PipelineManager.create.allocation"});
    }
}

/// @brief Descriptor Table、Root Descriptor、Constants と Static Sampler を Native 設定へ変換する
Result<RootSignatureHandle> DX12PipelineManager::create_root_signature(RootSignatureDesc a_desc)
{
    using rootResult = Result<RootSignatureHandle>;
    if (a_desc.parameters.size() > 64 || a_desc.samplers.size() > 16)
    {
        return rootResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.root.size"});
    }
    try
    {
        std::vector<D3D12_ROOT_PARAMETER> parameters(a_desc.parameters.size());
        std::vector<D3D12_DESCRIPTOR_RANGE> ranges(a_desc.parameters.size());
        std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(a_desc.samplers.size());
        for (std::size_t index = 0; index < parameters.size(); ++index)
        {
            const auto &source = a_desc.parameters[index];
            auto &target = parameters[index];
            if (!visibility(source.visibility, target.ShaderVisibility) || source.count == 0)
            {
                return rootResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.root.parameter"});
            }
            switch (source.type)
            {
            case RootParameterType::SrvTable:
            case RootParameterType::CbvTable:
            case RootParameterType::UavTable:
            {
                auto &range = ranges[index];
                range.RangeType = source.type == RootParameterType::SrvTable   ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                                  : source.type == RootParameterType::CbvTable ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                                                                               : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                range.NumDescriptors = source.count;
                range.BaseShaderRegister = source.shaderRegister;
                range.RegisterSpace = source.registerSpace;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                target.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                target.DescriptorTable = {1, &range};
                break;
            }
            case RootParameterType::Constants:
                target.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                target.Constants = {source.shaderRegister, source.registerSpace, source.count};
                break;
            case RootParameterType::ConstantBuffer:
            case RootParameterType::ShaderResource:
            case RootParameterType::UnorderedAccess:
                if (source.count != 1)
                {
                    return rootResult::failure(
                        {ErrorCategory::InvalidArgument, "DX12PipelineManager.root.descriptor_count"});
                }
                target.ParameterType = source.type == RootParameterType::ConstantBuffer ? D3D12_ROOT_PARAMETER_TYPE_CBV
                                       : source.type == RootParameterType::ShaderResource
                                           ? D3D12_ROOT_PARAMETER_TYPE_SRV
                                           : D3D12_ROOT_PARAMETER_TYPE_UAV;
                target.Descriptor = {source.shaderRegister, source.registerSpace};
                break;
            default:
                return rootResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.root.type"});
            }
        }
        for (std::size_t index = 0; index < samplers.size(); ++index)
        {
            const auto &source = a_desc.samplers[index];
            auto &target = samplers[index];
            if (!visibility(source.visibility, target.ShaderVisibility) ||
                (source.filter != SamplerFilter::Point && source.filter != SamplerFilter::Linear) ||
                (source.address != SamplerAddressMode::Clamp && source.address != SamplerAddressMode::Wrap))
            {
                return rootResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.root.sampler"});
            }
            target.Filter = source.filter == SamplerFilter::Linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                                                                   : D3D12_FILTER_MIN_MAG_MIP_POINT;
            target.AddressU = target.AddressV = target.AddressW = source.address == SamplerAddressMode::Clamp
                                                                      ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                                                      : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            target.ShaderRegister = source.shaderRegister;
            target.RegisterSpace = source.registerSpace;
            target.MaxLOD = D3D12_FLOAT32_MAX;
            target.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            target.MaxAnisotropy = 1;
        }
        D3D12_ROOT_SIGNATURE_DESC native{};
        native.NumParameters = static_cast<UINT>(parameters.size());
        native.pParameters = parameters.data();
        native.NumStaticSamplers = static_cast<UINT>(samplers.size());
        native.pStaticSamplers = samplers.data();
        native.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
        HRESULT hr = D3D12SerializeRootSignature(&native, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostics);
        if (FAILED(hr))
        {
            return rootResult::failure(pipeline_error("D3D12SerializeRootSignature", hr));
        }
        auto record = std::make_shared<RootRecord>();
        hr = device()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&record->native));
        if (FAILED(hr))
        {
            return rootResult::failure(pipeline_error("ID3D12Device.CreateRootSignature", hr));
        }
        auto nameResult = name_object(*record->native.Get(), a_desc.name);
        if (!nameResult.has_value())
        {
            return rootResult::failure(*nameResult.try_error());
        }
        record->desc = std::move(a_desc);
        return insert_record<RootSignatureHandle>(m_state->roots, std::move(record), m_state->id);
    }
    catch (const std::bad_alloc &)
    {
        return rootResult::failure({ErrorCategory::PlatformFailure, "DX12PipelineManager.root.allocation"});
    }
}

/// @brief Stage とコンパイル診断を保持した Blob だけを Registry に公開する
Result<ShaderBlobHandle> DX12PipelineManager::create_shader_blob(ShaderCompileDesc a_desc)
{
    using shaderResult = Result<ShaderBlobHandle>;
    auto compiled = m_state->compiler->compile(a_desc);
    if (!compiled.has_value())
    {
        return shaderResult::failure(*compiled.try_error());
    }
    try
    {
        auto record = std::make_shared<ShaderRecord>();
        record->desc = std::move(a_desc);
        record->native = compiled.take_value();
        return insert_record<ShaderBlobHandle>(m_state->shaders, std::move(record), m_state->id);
    }
    catch (const std::bad_alloc &)
    {
        return shaderResult::failure({ErrorCategory::PlatformFailure, "DX12PipelineManager.shader.allocation"});
    }
}

/// @brief Shader Stage と依存 Handle を照合して単一 Color Target の PSO を生成する
Result<PipelineStateHandle> DX12PipelineManager::create_graphics_pipeline(GraphicsPipelineStateDesc a_desc)
{
    using pipelineResult = Result<PipelineStateHandle>;
    auto root = find_record(m_state->roots, a_desc.rootSignature, m_state->id);
    auto vs = find_record(m_state->shaders, a_desc.vertexShader, m_state->id);
    auto ps = find_record(m_state->shaders, a_desc.pixelShader, m_state->id);
    if (!root || !vs || !ps || vs->desc.stage != ShaderStage::Vertex || ps->desc.stage != ShaderStage::Pixel ||
        texture_format(a_desc.renderTargetFormat) == DXGI_FORMAT_UNKNOWN ||
        (a_desc.topology != PrimitiveTopology::Triangle && a_desc.topology != PrimitiveTopology::Line &&
         a_desc.topology != PrimitiveTopology::Point) ||
        (a_desc.cullMode != CullMode::None && a_desc.cullMode != CullMode::Front && a_desc.cullMode != CullMode::Back))
    {
        return pipelineResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.graphics.desc"});
    }
    try
    {
        auto record = std::make_shared<PipelineRecord>();
        record->root = std::move(root);
        record->format = a_desc.renderTargetFormat;
        record->topology = a_desc.topology;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC native{};
        native.pRootSignature = record->root->native.Get();
        native.VS = {vs->native->GetBufferPointer(), vs->native->GetBufferSize()};
        native.PS = {ps->native->GetBufferPointer(), ps->native->GetBufferSize()};
        auto &blend = native.BlendState.RenderTarget[0];
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.BlendEnable = a_desc.isAlphaBlendEnabled;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        native.RasterizerState.FillMode = a_desc.isWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
        native.RasterizerState.CullMode = a_desc.cullMode == CullMode::None   ? D3D12_CULL_MODE_NONE
                                          : a_desc.cullMode == CullMode::Back ? D3D12_CULL_MODE_BACK
                                                                              : D3D12_CULL_MODE_FRONT;
        native.RasterizerState.DepthClipEnable = true;
        native.SampleMask = (std::numeric_limits<UINT>::max)();
        native.PrimitiveTopologyType =
            a_desc.topology == PrimitiveTopology::Triangle ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
            : a_desc.topology == PrimitiveTopology::Line   ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                           : D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        native.NumRenderTargets = 1;
        native.RTVFormats[0] = texture_format(a_desc.renderTargetFormat);
        native.SampleDesc.Count = 1;
        const HRESULT hr = device()->CreateGraphicsPipelineState(&native, IID_PPV_ARGS(&record->native));
        if (FAILED(hr))
        {
            return pipelineResult::failure(pipeline_error("ID3D12Device.CreateGraphicsPipelineState", hr));
        }
        auto nameResult = name_object(*record->native.Get(), a_desc.name);
        if (!nameResult.has_value())
        {
            return pipelineResult::failure(*nameResult.try_error());
        }
        return insert_record<PipelineStateHandle>(m_state->pipelines, std::move(record), m_state->id);
    }
    catch (const std::bad_alloc &)
    {
        return pipelineResult::failure({ErrorCategory::PlatformFailure, "DX12PipelineManager.graphics.allocation"});
    }
}

/// @brief Root と Compute Shader の組合せから Compute PSO を生成する
Result<PipelineStateHandle> DX12PipelineManager::create_compute_pipeline(ComputePipelineStateDesc a_desc)
{
    using pipelineResult = Result<PipelineStateHandle>;
    auto root = find_record(m_state->roots, a_desc.rootSignature, m_state->id);
    auto shader = find_record(m_state->shaders, a_desc.computeShader, m_state->id);
    if (!root || !shader || shader->desc.stage != ShaderStage::Compute)
    {
        return pipelineResult::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.compute.desc"});
    }
    try
    {
        auto record = std::make_shared<PipelineRecord>();
        record->isCompute = true;
        record->root = std::move(root);
        D3D12_COMPUTE_PIPELINE_STATE_DESC native{};
        native.pRootSignature = record->root->native.Get();
        native.CS = {shader->native->GetBufferPointer(), shader->native->GetBufferSize()};
        const HRESULT hr = device()->CreateComputePipelineState(&native, IID_PPV_ARGS(&record->native));
        if (FAILED(hr))
        {
            return pipelineResult::failure(pipeline_error("ID3D12Device.CreateComputePipelineState", hr));
        }
        auto nameResult = name_object(*record->native.Get(), a_desc.name);
        if (!nameResult.has_value())
        {
            return pipelineResult::failure(*nameResult.try_error());
        }
        return insert_record<PipelineStateHandle>(m_state->pipelines, std::move(record), m_state->id);
    }
    catch (const std::bad_alloc &)
    {
        return pipelineResult::failure({ErrorCategory::PlatformFailure, "DX12PipelineManager.compute.allocation"});
    }
}

/// @brief 依存 PSO の共有参照を維持したまま Root Handle を失効させる
Result<void> DX12PipelineManager::retire(RootSignatureHandle a_handle)
{
    return retire_record(m_state->roots, a_handle, m_state->id);
}
/// @brief 生成済み PSO を変更せず Shader Blob を回収する
Result<void> DX12PipelineManager::retire(ShaderBlobHandle a_handle)
{
    return retire_record(m_state->shaders, a_handle, m_state->id);
}
/// @brief 記録済み Command の参照を維持したまま PSO Handle を失効させる
Result<void> DX12PipelineManager::retire(PipelineStateHandle a_handle)
{
    return retire_record(m_state->pipelines, a_handle, m_state->id);
}

/// @brief PSO と Root の実体を Command の保持領域へ移してから Native Binding を記録する
Result<GpuTextureFormat> DX12PipelineManager::bind_graphics(DX12GpuCommandContext &a_command,
                                                            PipelineStateHandle a_handle)
{
    auto record = find_record(m_state->pipelines, a_handle, m_state->id);
    if (!record || record->isCompute || a_command.type() != QueueType::Graphics ||
        a_command.state() != CommandState::Recording || a_command.m_device.Get() != device())
    {
        return Result<GpuTextureFormat>::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.bind_graphics"});
    }
    auto retained = a_command.retain_pipeline(record);
    if (!retained.has_value())
    {
        return Result<GpuTextureFormat>::failure(*retained.try_error());
    }
    auto *list = a_command.command_list();
    list->SetGraphicsRootSignature(record->root->native.Get());
    list->SetPipelineState(record->native.Get());
    list->IASetPrimitiveTopology(record->topology == PrimitiveTopology::Triangle ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                 : record->topology == PrimitiveTopology::Line   ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                                                                 : D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    return Result<GpuTextureFormat>::success(record->format);
}

/// @brief Compute Root と PSO を Graphics または Compute List に記録する
Result<void> DX12PipelineManager::bind_compute(DX12GpuCommandContext &a_command, PipelineStateHandle a_handle)
{
    auto record = find_record(m_state->pipelines, a_handle, m_state->id);
    if (!record || !record->isCompute || a_command.type() == QueueType::Copy ||
        a_command.state() != CommandState::Recording || a_command.m_device.Get() != device())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.bind_compute"});
    }
    auto retained = a_command.retain_pipeline(record);
    if (!retained.has_value())
    {
        return retained;
    }
    a_command.command_list()->SetComputeRootSignature(record->root->native.Get());
    a_command.command_list()->SetPipelineState(record->native.Get());
    return Result<void>::success();
}

/// @brief 範囲外や SRV 以外の Root Table へ Texture を Bind させない
Result<void> DX12PipelineManager::validate_texture_binding(PipelineStateHandle a_handle,
                                                           std::uint32_t a_rootParameter) const
{
    auto record = find_record(m_state->pipelines, a_handle, m_state->id);
    if (!record || record->isCompute || a_rootParameter >= record->root->desc.parameters.size())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.texture.parameter"});
    }
    const auto &parameter = record->root->desc.parameters[a_rootParameter];
    if (parameter.type != RootParameterType::SrvTable || parameter.count != 1)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.texture.table"});
    }
    return Result<void>::success();
}
/// @brief 未対応の Root Binding や未設定の Table を使って GPU 実行しない
Result<void> DX12PipelineManager::validate_bindings(PipelineStateHandle a_handle,
                                                    std::span<const std::uint32_t> a_boundParameters) const
{
    auto record = find_record(m_state->pipelines, a_handle, m_state->id);
    if (!record)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12PipelineManager.bindings.handle"});
    }
    for (std::size_t index = 0; index < record->root->desc.parameters.size(); ++index)
    {
        const auto &parameter = record->root->desc.parameters[index];
        if (parameter.type != RootParameterType::SrvTable || parameter.count != 1 ||
            std::find(a_boundParameters.begin(), a_boundParameters.end(), index) == a_boundParameters.end())
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12PipelineManager.bindings.unbound"});
        }
    }
    return Result<void>::success();
}

/// @brief Native Command と Manager の生成 Device を照合する
ID3D12Device *DX12PipelineManager::device() const noexcept
{
    return m_state ? m_state->device.Get() : nullptr;
}
} // namespace cue::dx12
