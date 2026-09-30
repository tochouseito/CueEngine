#pragma once

#include "DX12PipelineManager.h"
#include "DX12ResourcePool.h"

#include <Cue/Renderer/RHI/GpuCommands.h>

namespace cue::detail
{
/// @brief 貸出中の DX12 List を借用し、RHI Command を記録する
class DX12CommandRecorder final : public IGpuCommandRecorder
{
public:
    /// @brief List と Owner はこの Recorder より長く生存させる
    DX12CommandRecorder(ID3D12GraphicsCommandList* a_list, GpuQueueType a_queue,
                         DX12ResourcePool& a_resources, DX12PipelineManager& a_pipelines) noexcept;

    [[nodiscard]] Result<void> transition(GpuResourceHandle a_resource, GpuResourceState a_before,
                                           GpuResourceState a_after) override;
    [[nodiscard]] Result<void> uav_barrier(GpuResourceHandle a_resource) override;
    [[nodiscard]] Result<void> copy_buffer(GpuResourceHandle a_destination, std::uint64_t a_destinationOffset,
                                           GpuResourceHandle a_source, std::uint64_t a_sourceOffset,
                                           std::uint64_t a_size) override;
    [[nodiscard]] Result<void> copy_texture(GpuResourceHandle a_destination,
                                            GpuResourceHandle a_source) override;
    [[nodiscard]] Result<void> copy_buffer_region(const GpuBufferCopyRegion& a_region) override;
    [[nodiscard]] Result<void> copy_texture_region_to_buffer(
        const GpuTextureToBufferCopyRegion& a_region) override;
    [[nodiscard]] Result<void> bind_pipeline(GpuPipelineHandle a_pipeline) override;
    [[nodiscard]] Result<void> bind_view(std::uint32_t a_parameter, GpuViewHandle a_view) override;
    [[nodiscard]] Result<void> set_32bit_constant(std::uint32_t a_parameter,
                                                   std::uint32_t a_value) override;
    [[nodiscard]] Result<void> set_root_buffer(std::uint32_t a_parameter,
        GpuResourceHandle a_buffer, GpuRootParameterType a_type, GpuMemory a_memory,
        std::uint32_t a_resourceIndex) override;
    [[nodiscard]] Result<void> set_graphics_texture_table(std::uint32_t a_parameter) override;
    [[nodiscard]] Result<void> set_render_targets(GpuViewHandle a_color,
                                                  std::optional<GpuViewHandle> a_depth) override;
    [[nodiscard]] Result<void> set_render_targets_many(std::span<const GpuViewHandle> a_colors,
                                                        std::optional<GpuViewHandle> a_depth) override;
    [[nodiscard]] Result<void> clear_color(GpuViewHandle a_color,
                                            const std::array<float, 4>& a_value) override;
    [[nodiscard]] Result<void> clear_depth(GpuViewHandle a_depth, float a_value) override;
    [[nodiscard]] Result<void> clear_depth_stencil(GpuViewHandle a_depth, float a_depthValue,
                                                    std::uint8_t a_stencilValue) override;
    [[nodiscard]] Result<void> clear_unordered_access_uint(GpuViewHandle a_view,
        const std::array<std::uint32_t, 4>& a_values) override;
    [[nodiscard]] Result<void> bind_vertex_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                   std::uint32_t a_stride, std::uint32_t a_size) override;
    [[nodiscard]] Result<void> bind_index_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                  std::uint32_t a_size) override;
    [[nodiscard]] Result<void> bind_index_buffer_format(GpuResourceHandle a_buffer,
        std::uint64_t a_offset, std::uint32_t a_size, GpuIndexFormat a_format) override;
    [[nodiscard]] Result<void> set_viewport(std::uint32_t a_width, std::uint32_t a_height) override;
    [[nodiscard]] Result<void> set_viewport_rect(std::uint32_t a_x, std::uint32_t a_y,
                                                 std::uint32_t a_width, std::uint32_t a_height) override;
    [[nodiscard]] Result<void> set_primitive_topology(GpuPrimitiveTopology a_topology) override;
    [[nodiscard]] Result<void> draw(std::uint32_t a_vertices, std::uint32_t a_instances) override;
    [[nodiscard]] Result<void> draw_indexed(std::uint32_t a_indices, std::uint32_t a_instances) override;
    [[nodiscard]] Result<void> draw_indexed_range(std::uint32_t a_indices, std::uint32_t a_instances,
        std::uint32_t a_startIndex, std::int32_t a_baseVertex,
        std::uint32_t a_startInstance) override;
    [[nodiscard]] Result<void> execute_indexed_indirect(GpuResourceHandle a_commands,
        GpuResourceHandle a_count, std::uint32_t a_maxCommandCount) override;
    [[nodiscard]] Result<void> dispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) override;

    /// @brief Swap Chain が所有する Back Buffer へ Resource 全体をコピーする Backend 内部入口
    [[nodiscard]] Result<void> copy_to_back_buffer(ID3D12Resource* a_backBuffer,
                                                    GpuResourceHandle a_source);

private:
    [[nodiscard]] static Result<D3D12_RESOURCE_STATES> resource_state(GpuResourceState a_state);
    [[nodiscard]] bool supports(GpuResourceState a_state) const noexcept;

    ID3D12GraphicsCommandList* m_list = nullptr;
    GpuQueueType m_queue = GpuQueueType::Graphics;
    DX12ResourcePool* m_resources = nullptr;
    DX12PipelineManager* m_pipelines = nullptr;
    GpuPipelineHandle m_pipeline{};
    bool m_hasPipeline = false;
    D3D_PRIMITIVE_TOPOLOGY m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};
} // namespace cue::detail
