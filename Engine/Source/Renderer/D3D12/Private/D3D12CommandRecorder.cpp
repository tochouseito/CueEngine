#include "D3D12CommandRecorder.h"

#include <limits>

namespace cue::detail
{
/// @brief 貸出中 List と依存 Owner を借用する
D3D12CommandRecorder::D3D12CommandRecorder(ID3D12GraphicsCommandList* a_list, GpuQueueType a_queue,
                                             D3D12ResourcePool& a_resources,
                                             D3D12PipelineLibrary& a_pipelines) noexcept
    : m_list(a_list), m_queue(a_queue), m_resources(&a_resources), m_pipelines(&a_pipelines)
{
}

/// @brief Queue が扱える状態だけを D3D12 Barrier として記録する
Result<void> D3D12CommandRecorder::transition(GpuResourceHandle a_resource, GpuResourceState a_before,
                                                GpuResourceState a_after)
{
    if (!m_list || !supports(a_before) || !supports(a_after) || a_before == a_after)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.transition"});
    }
    auto resourceResult = m_resources->resource(a_resource);
    auto beforeResult = resource_state(a_before);
    auto afterResult = resource_state(a_after);
    if (!resourceResult.has_value() || !beforeResult.has_value() || !afterResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.transition.resource"});
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resourceResult.take_value();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = beforeResult.take_value();
    barrier.Transition.StateAfter = afterResult.take_value();
    m_list->ResourceBarrier(1, &barrier);
    return Result<void>::success();
}

/// @brief UAV の先行書込みを後続 Command より前に確定する
Result<void> D3D12CommandRecorder::uav_barrier(GpuResourceHandle a_resource)
{
    if (!m_list || m_queue == GpuQueueType::Copy)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12CommandRecorder.uav_barrier"});
    }
    auto resourceResult = m_resources->resource(a_resource);
    if (!resourceResult.has_value())
    {
        return Result<void>::failure(*resourceResult.try_error());
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resourceResult.take_value();
    m_list->ResourceBarrier(1, &barrier);
    return Result<void>::success();
}

/// @brief 二つの Buffer の範囲を検証して Copy Command を記録する
Result<void> D3D12CommandRecorder::copy_buffer(GpuResourceHandle a_destination,
                                                std::uint64_t a_destinationOffset,
                                                GpuResourceHandle a_source, std::uint64_t a_sourceOffset,
                                                std::uint64_t a_size)
{
    auto destinationResult = m_resources->resource(a_destination);
    auto sourceResult = m_resources->resource(a_source);
    if (!m_list || !destinationResult.has_value() || !sourceResult.has_value() || a_size == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.copy_buffer"});
    }
    ID3D12Resource* destination = destinationResult.take_value();
    ID3D12Resource* source = sourceResult.take_value();
    const auto destinationDesc = destination->GetDesc();
    const auto sourceDesc = source->GetDesc();
    if (destinationDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        a_destinationOffset > destinationDesc.Width || a_sourceOffset > sourceDesc.Width ||
        a_size > destinationDesc.Width - a_destinationOffset || a_size > sourceDesc.Width - a_sourceOffset)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.copy_buffer.range"});
    }
    m_list->CopyBufferRegion(destination, a_destinationOffset, source, a_sourceOffset, a_size);
    return Result<void>::success();
}

/// @brief 同じ寸法と Format の Texture 全体をコピーする
Result<void> D3D12CommandRecorder::copy_texture(GpuResourceHandle a_destination,
                                                 GpuResourceHandle a_source)
{
    auto destinationResult = m_resources->resource(a_destination);
    auto sourceResult = m_resources->resource(a_source);
    if (!m_list || !destinationResult.has_value() || !sourceResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.copy_texture"});
    }
    ID3D12Resource* destination = destinationResult.take_value();
    ID3D12Resource* source = sourceResult.take_value();
    const auto destinationDesc = destination->GetDesc();
    const auto sourceDesc = source->GetDesc();
    if (destinationDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        destinationDesc.Width != sourceDesc.Width || destinationDesc.Height != sourceDesc.Height ||
        destinationDesc.Format != sourceDesc.Format || destinationDesc.MipLevels != sourceDesc.MipLevels)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.copy_texture.shape"});
    }
    m_list->CopyResource(destination, source);
    return Result<void>::success();
}

/// @brief Presentation 所有の Back Buffer を借用して Copy Command を記録する
Result<void> D3D12CommandRecorder::copy_to_back_buffer(ID3D12Resource* a_backBuffer,
                                                        GpuResourceHandle a_source)
{
    auto sourceResult = m_resources->resource(a_source);
    if (!m_list || m_queue != GpuQueueType::Graphics || !a_backBuffer || !sourceResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.copy_to_back_buffer"});
    }
    ID3D12Resource* source = sourceResult.take_value();
    const auto destinationDesc = a_backBuffer->GetDesc();
    const auto sourceDesc = source->GetDesc();
    if (destinationDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        destinationDesc.Width != sourceDesc.Width || destinationDesc.Height != sourceDesc.Height ||
        destinationDesc.Format != sourceDesc.Format)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "D3D12CommandRecorder.copy_to_back_buffer.shape"});
    }
    m_list->CopyResource(a_backBuffer, source);
    return Result<void>::success();
}

/// @brief Queue と PSO の種類を照合して Root Signature も設定する
Result<void> D3D12CommandRecorder::bind_pipeline(GpuPipelineHandle a_pipeline)
{
    if (!m_list || m_queue == GpuQueueType::Copy ||
        (m_queue == GpuQueueType::Compute && a_pipeline.kind != GpuPipelineKind::Compute))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12CommandRecorder.bind_pipeline"});
    }
    auto result = a_pipeline.kind == GpuPipelineKind::Graphics
                      ? m_pipelines->bind_graphics(m_list, a_pipeline)
                      : m_pipelines->bind_compute(m_list, a_pipeline);
    if (result.has_value())
    {
        m_pipeline = a_pipeline;
        m_hasPipeline = true;
    }
    return result;
}

/// @brief Root Table と View の種類を照合して Shader Heap を設定する
Result<void> D3D12CommandRecorder::bind_view(std::uint32_t a_parameter, GpuViewHandle a_view)
{
    if (!m_list || !m_hasPipeline || !m_pipelines->accepts_view(m_pipeline, a_parameter, a_view.kind))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.bind_view"});
    }
    auto handleResult = m_resources->gpu_handle(a_view);
    if (!handleResult.has_value())
    {
        return Result<void>::failure(*handleResult.try_error());
    }
    ID3D12DescriptorHeap* heap = m_resources->shader_heap();
    m_list->SetDescriptorHeaps(1, &heap);
    const auto handle = handleResult.take_value();
    if (m_pipeline.kind == GpuPipelineKind::Graphics)
    {
        m_list->SetGraphicsRootDescriptorTable(a_parameter, handle);
    }
    else
    {
        m_list->SetComputeRootDescriptorTable(a_parameter, handle);
    }
    return Result<void>::success();
}

/// @brief Graphics Queue の RTV と任意の DSV を Output Merger へ設定する
Result<void> D3D12CommandRecorder::set_render_targets(GpuViewHandle a_color,
                                                       std::optional<GpuViewHandle> a_depth)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_color.kind != GpuViewKind::RenderTarget ||
        (a_depth && a_depth->kind != GpuViewKind::DepthStencil))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.set_render_targets"});
    }
    auto colorResult = m_resources->cpu_handle(a_color);
    if (!colorResult.has_value())
    {
        return Result<void>::failure(*colorResult.try_error());
    }
    const auto color = colorResult.take_value();
    if (a_depth)
    {
        auto depthResult = m_resources->cpu_handle(*a_depth);
        if (!depthResult.has_value())
        {
            return Result<void>::failure(*depthResult.try_error());
        }
        const auto depth = depthResult.take_value();
        m_list->OMSetRenderTargets(1, &color, FALSE, &depth);
    }
    else
    {
        m_list->OMSetRenderTargets(1, &color, FALSE, nullptr);
    }
    return Result<void>::success();
}

/// @brief Graphics Queue の Color Target を消去する
Result<void> D3D12CommandRecorder::clear_color(GpuViewHandle a_color,
                                                const std::array<float, 4>& a_value)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_color.kind != GpuViewKind::RenderTarget)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.clear_color"});
    }
    auto handleResult = m_resources->cpu_handle(a_color);
    if (!handleResult.has_value())
    {
        return Result<void>::failure(*handleResult.try_error());
    }
    m_list->ClearRenderTargetView(handleResult.take_value(), a_value.data(), 0, nullptr);
    return Result<void>::success();
}

/// @brief Graphics Queue の Depth Target を消去する
Result<void> D3D12CommandRecorder::clear_depth(GpuViewHandle a_depth, float a_value)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_depth.kind != GpuViewKind::DepthStencil ||
        a_value < 0.0f || a_value > 1.0f)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.clear_depth"});
    }
    auto handleResult = m_resources->cpu_handle(a_depth);
    if (!handleResult.has_value())
    {
        return Result<void>::failure(*handleResult.try_error());
    }
    m_list->ClearDepthStencilView(handleResult.take_value(), D3D12_CLEAR_FLAG_DEPTH, a_value, 0, 0, nullptr);
    return Result<void>::success();
}

/// @brief Buffer 範囲を Vertex Buffer View として設定する
Result<void> D3D12CommandRecorder::bind_vertex_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                       std::uint32_t a_stride, std::uint32_t a_size)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_stride == 0 || a_size == 0 ||
        a_size % a_stride != 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.bind_vertex_buffer"});
    }
    auto resourceResult = m_resources->resource(a_buffer);
    if (!resourceResult.has_value())
    {
        return Result<void>::failure(*resourceResult.try_error());
    }
    ID3D12Resource* resource = resourceResult.take_value();
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || a_offset > desc.Width ||
        a_size > desc.Width - a_offset)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.vertexRange"});
    }
    D3D12_VERTEX_BUFFER_VIEW view{resource->GetGPUVirtualAddress() + a_offset, a_size, a_stride};
    m_list->IASetVertexBuffers(0, 1, &view);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    return Result<void>::success();
}

/// @brief 16 bit Index 範囲を検証して Input Assembler へ設定する
Result<void> D3D12CommandRecorder::bind_index_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                      std::uint32_t a_size)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_size == 0 || a_size % sizeof(std::uint16_t) != 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.bind_index_buffer"});
    }
    auto resourceResult = m_resources->resource(a_buffer);
    if (!resourceResult.has_value())
    {
        return Result<void>::failure(*resourceResult.try_error());
    }
    ID3D12Resource* resource = resourceResult.take_value();
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || a_offset > desc.Width ||
        a_size > desc.Width - a_offset || a_offset % sizeof(std::uint16_t) != 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.indexRange"});
    }
    D3D12_INDEX_BUFFER_VIEW view{resource->GetGPUVirtualAddress() + a_offset, a_size, DXGI_FORMAT_R16_UINT};
    m_list->IASetIndexBuffer(&view);
    return Result<void>::success();
}

/// @brief Surface 全体を描く Pass の Viewport と Scissor を設定する
Result<void> D3D12CommandRecorder::set_viewport(std::uint32_t a_width, std::uint32_t a_height)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_width == 0 || a_height == 0 ||
        a_width > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
        a_height > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12CommandRecorder.set_viewport"});
    }
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(a_width);
    viewport.Height = static_cast<float>(a_height);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height)};
    m_list->RSSetViewports(1, &viewport);
    m_list->RSSetScissorRects(1, &scissor);
    return Result<void>::success();
}

/// @brief Graphics PSO 設定後だけ Draw Command を記録する
Result<void> D3D12CommandRecorder::draw(std::uint32_t a_vertices, std::uint32_t a_instances)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || !m_hasPipeline ||
        m_pipeline.kind != GpuPipelineKind::Graphics || a_vertices == 0 || a_instances == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12CommandRecorder.draw"});
    }
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_list->DrawInstanced(a_vertices, a_instances, 0, 0);
    return Result<void>::success();
}

/// @brief Graphics PSO と Index Buffer 設定後に DrawIndexed を記録する
Result<void> D3D12CommandRecorder::draw_indexed(std::uint32_t a_indices, std::uint32_t a_instances)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || !m_hasPipeline ||
        m_pipeline.kind != GpuPipelineKind::Graphics || a_indices == 0 || a_instances == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12CommandRecorder.draw_indexed"});
    }
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_list->DrawIndexedInstanced(a_indices, a_instances, 0, 0, 0);
    return Result<void>::success();
}

/// @brief Compute PSO 設定後だけ Dispatch Command を記録する
Result<void> D3D12CommandRecorder::dispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
{
    if (!m_list || m_queue == GpuQueueType::Copy || !m_hasPipeline ||
        m_pipeline.kind != GpuPipelineKind::Compute || a_x == 0 || a_y == 0 || a_z == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12CommandRecorder.dispatch"});
    }
    m_list->Dispatch(a_x, a_y, a_z);
    return Result<void>::success();
}

/// @brief RHI 状態を D3D12 の完全な Shader 可視性へ対応付ける
Result<D3D12_RESOURCE_STATES> D3D12CommandRecorder::resource_state(GpuResourceState a_state)
{
    switch (a_state)
    {
    case GpuResourceState::Common:
        return Result<D3D12_RESOURCE_STATES>::success(D3D12_RESOURCE_STATE_COMMON);
    case GpuResourceState::RenderTarget:
        return Result<D3D12_RESOURCE_STATES>::success(D3D12_RESOURCE_STATE_RENDER_TARGET);
    case GpuResourceState::DepthWrite:
        return Result<D3D12_RESOURCE_STATES>::success(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    case GpuResourceState::ShaderResource:
        return Result<D3D12_RESOURCE_STATES>::success(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    case GpuResourceState::ComputeShaderResource:
        return Result<D3D12_RESOURCE_STATES>::success(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    case GpuResourceState::CopySource:
        return Result<D3D12_RESOURCE_STATES>::success(D3D12_RESOURCE_STATE_COPY_SOURCE);
    case GpuResourceState::CopyDest:
        return Result<D3D12_RESOURCE_STATES>::success(D3D12_RESOURCE_STATE_COPY_DEST);
    case GpuResourceState::UnorderedAccess:
        return Result<D3D12_RESOURCE_STATES>::success(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    case GpuResourceState::VertexIndex:
        return Result<D3D12_RESOURCE_STATES>::success(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                                      D3D12_RESOURCE_STATE_INDEX_BUFFER);
    default:
        return Result<D3D12_RESOURCE_STATES>::failure({ErrorCategory::InvalidArgument,
                                                       "D3D12CommandRecorder.resource_state"});
    }
}

/// @brief Queue 固有の Barrier State 制限を確認する
bool D3D12CommandRecorder::supports(GpuResourceState a_state) const noexcept
{
    if (m_queue == GpuQueueType::Graphics)
    {
        return true;
    }
    if (m_queue == GpuQueueType::Copy)
    {
        return a_state == GpuResourceState::Common || a_state == GpuResourceState::CopySource ||
               a_state == GpuResourceState::CopyDest;
    }
    return a_state != GpuResourceState::RenderTarget && a_state != GpuResourceState::DepthWrite &&
           a_state != GpuResourceState::ShaderResource && a_state != GpuResourceState::VertexIndex;
}
} // namespace cue::detail
