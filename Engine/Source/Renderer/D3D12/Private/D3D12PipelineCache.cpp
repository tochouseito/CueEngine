#include "D3D12PipelineCache.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "FixedMeshShaderPath.h"

namespace cue::detail
{
/// @brief 生成順の逆順で PSO、Root、Shader と View を Owner へ返す
D3D12PipelineCache::~D3D12PipelineCache()
{
    if (m_library)
    {
        if (m_pipeline.owner)
        {
            [[maybe_unused]] auto released = m_library->destroy_pipeline(m_pipeline);
        }
        if (m_root.owner)
        {
            [[maybe_unused]] auto released = m_library->destroy_root_signature(m_root);
        }
        if (m_pixel.owner)
        {
            [[maybe_unused]] auto released = m_library->destroy_shader(m_pixel);
        }
        if (m_vertex.owner)
        {
            [[maybe_unused]] auto released = m_library->destroy_shader(m_vertex);
        }
    }
    if (m_resources)
    {
        for (const auto view : m_cbvs)
        {
            if (view.owner)
            {
                [[maybe_unused]] auto released = m_resources->destroy_view(view);
            }
        }
        if (m_constants.owner)
        {
            [[maybe_unused]] auto released = m_resources->destroy(m_constants);
        }
    }
}

/// @brief 固定 Mesh を汎用 Pipeline Library と共通 Descriptor Heap の Client として登録する
Result<std::unique_ptr<D3D12PipelineCache>> D3D12PipelineCache::create(D3D12DeviceContext& a_device,
                                                                          D3D12ResourcePool& a_resources,
                                                                          D3D12PipelineLibrary& a_library)
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
    std::ifstream stream(std::filesystem::path{k_fixedMeshShaderPath}, std::ios::binary);
    if (!stream)
    {
        return CacheResult::failure({ErrorCategory::PlatformFailure, "FixedMeshShader.open"});
    }
    const std::string source(std::istreambuf_iterator<char>{stream}, {});
    if (source.empty() || stream.bad())
    {
        return CacheResult::failure({ErrorCategory::PlatformFailure, "FixedMeshShader.read"});
    }
    auto cache = std::make_unique<D3D12PipelineCache>();
    cache->m_library = &a_library;
    cache->m_resources = &a_resources;
    auto vertex = a_library.create_shader({source, "VSMain", GpuShaderStage::Vertex});
    if (!vertex.has_value())
    {
        return CacheResult::failure(*vertex.try_error());
    }
    cache->m_vertex = vertex.take_value();
    auto pixel = a_library.create_shader({source, "PSMain", GpuShaderStage::Pixel});
    if (!pixel.has_value())
    {
        return CacheResult::failure(*pixel.try_error());
    }
    cache->m_pixel = pixel.take_value();
    auto root = a_library.create_root_signature({{{GpuViewKind::ConstantBuffer, 0, 0}}});
    if (!root.has_value())
    {
        return CacheResult::failure(*root.try_error());
    }
    cache->m_root = root.take_value();
    GpuGraphicsPipelineDesc desc{};
    desc.rootSignature = cache->m_root;
    desc.vertexShader = cache->m_vertex;
    desc.pixelShader = cache->m_pixel;
    desc.vertexElements = {{"POSITION", 0, GpuVertexFormat::Float3, 0, 0},
                           {"COLOR", 0, GpuVertexFormat::Float3, 0, 12}};
    desc.hasDepth = true;
    desc.cullMode = GpuCullMode::None;
    auto pipeline = a_library.create_graphics_pipeline(std::move(desc));
    if (!pipeline.has_value())
    {
        return CacheResult::failure(*pipeline.try_error());
    }
    cache->m_pipeline = pipeline.take_value();

    // Buffer Slot ごとの定数は Fence 完了後だけ上書きする
    auto constants = a_resources.create_buffer({256 * k_backBufferCount, GpuMemory::Upload});
    if (!constants.has_value())
    {
        return CacheResult::failure(*constants.try_error());
    }
    cache->m_constants = constants.take_value();
    for (UINT index = 0; index < k_backBufferCount; ++index)
    {
        auto view = a_resources.create_view(cache->m_constants,
                                            {GpuViewKind::ConstantBuffer, 256 * index, 256});
        if (!view.has_value())
        {
            return CacheResult::failure(*view.try_error());
        }
        cache->m_cbvs[index] = view.take_value();
    }
    return CacheResult::success(std::move(cache));
}

/// @brief 汎用 Pipeline と共通 CBV／SRV／UAV Heap を固定 Mesh Pass へ Bind する
Result<void> D3D12PipelineCache::bind(IGpuCommandRecorder& a_commands, UINT a_slot,
                                      const std::array<float, 4>& a_tint)
{
    if (a_slot >= k_backBufferCount)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12PipelineCache.bind"});
    }
    auto written = m_resources->write_buffer(m_constants, 256 * a_slot, a_tint.data(),
                                             sizeof(float) * a_tint.size());
    if (!written.has_value())
    {
        return written;
    }
    auto pipeline = a_commands.bind_pipeline(m_pipeline);
    if (!pipeline.has_value())
    {
        return pipeline;
    }
    return a_commands.bind_view(0, m_cbvs[a_slot]);
}
} // namespace cue::detail
