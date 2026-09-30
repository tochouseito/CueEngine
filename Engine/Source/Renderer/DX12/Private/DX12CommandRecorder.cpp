#include "DX12CommandRecorder.h"

#include <limits>

namespace cue::detail
{
/// @brief 貸出中 List と依存 Owner を借用する
DX12CommandRecorder::DX12CommandRecorder(ID3D12GraphicsCommandList* a_list, GpuQueueType a_queue,
                                             DX12ResourcePool& a_resources,
                                             DX12PipelineManager& a_pipelines) noexcept
    : m_list(a_list), m_queue(a_queue), m_resources(&a_resources), m_pipelines(&a_pipelines)
{
}

/// @brief Queue が扱える状態だけを DX12 Barrier として記録する
Result<void> DX12CommandRecorder::transition(GpuResourceHandle a_resource, GpuResourceState a_before,
                                                GpuResourceState a_after)
{
    if (!m_list || !supports(a_before) || !supports(a_after) || a_before == a_after)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.transition"});
    }
    auto resourceResult = m_resources->resource(a_resource);
    auto beforeResult = resource_state(a_before);
    auto afterResult = resource_state(a_after);
    if (!resourceResult.has_value() || !beforeResult.has_value() || !afterResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.transition.resource"});
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
Result<void> DX12CommandRecorder::uav_barrier(GpuResourceHandle a_resource)
{
    if (!m_list || m_queue == GpuQueueType::Copy)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12CommandRecorder.uav_barrier"});
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
Result<void> DX12CommandRecorder::copy_buffer(GpuResourceHandle a_destination,
                                                std::uint64_t a_destinationOffset,
                                                GpuResourceHandle a_source, std::uint64_t a_sourceOffset,
                                                std::uint64_t a_size)
{
    auto destinationResult = m_resources->resource(a_destination);
    auto sourceResult = m_resources->resource(a_source);
    if (!m_list || !destinationResult.has_value() || !sourceResult.has_value() || a_size == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.copy_buffer"});
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
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.copy_buffer.range"});
    }
    m_list->CopyBufferRegion(destination, a_destinationOffset, source, a_sourceOffset, a_size);
    return Result<void>::success();
}

/// @brief 複数 Heap Slice の物理 Buffer を選択して範囲コピーする
Result<void> DX12CommandRecorder::copy_buffer_region(const GpuBufferCopyRegion& a_region)
{
    auto destinationResult = m_resources->resource(a_region.destination, a_region.destinationMemory,
                                                    a_region.destinationIndex);
    auto sourceResult = m_resources->resource(a_region.source, a_region.sourceMemory, a_region.sourceIndex);
    if (!m_list || !destinationResult.has_value() || !sourceResult.has_value() || a_region.size == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.copy_buffer_region"});
    }
    ID3D12Resource* destination = destinationResult.take_value();
    ID3D12Resource* source = sourceResult.take_value();
    const auto destinationSize = destination->GetDesc().Width;
    const auto sourceSize = source->GetDesc().Width;
    if (a_region.destinationOffset > destinationSize || a_region.sourceOffset > sourceSize ||
        a_region.size > destinationSize - a_region.destinationOffset ||
        a_region.size > sourceSize - a_region.sourceOffset)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.copy_buffer_region.range"});
    }
    m_list->CopyBufferRegion(destination, a_region.destinationOffset, source,
                             a_region.sourceOffset, a_region.size);
    return Result<void>::success();
}

/// @brief 2D Texture の RGBA／R32 矩形を整列済み Readback Slice へ転送する
Result<void> DX12CommandRecorder::copy_texture_region_to_buffer(
    const GpuTextureToBufferCopyRegion& a_region)
{
    auto sourceResult = m_resources->texture_resource(a_region.source, a_region.sourceIndex);
    auto destinationResult = m_resources->resource(a_region.destination, GpuMemory::Readback,
                                                   a_region.destinationIndex);
    if (!m_list || !sourceResult.has_value() || !destinationResult.has_value() ||
        a_region.width == 0 || a_region.height == 0 ||
        a_region.destinationOffset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.copy_texture_region_to_buffer"});
    }
    ID3D12Resource* source = sourceResult.take_value();
    ID3D12Resource* destination = destinationResult.take_value();
    const auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
        desc.SampleDesc.Count != 1 ||
        (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         desc.Format != DXGI_FORMAT_R32_UINT) ||
        a_region.sourceX > desc.Width || a_region.sourceY > desc.Height ||
        a_region.width > desc.Width - a_region.sourceX ||
        a_region.height > desc.Height - a_region.sourceY ||
        a_region.width > (std::numeric_limits<UINT>::max() - 255u) / 4u)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.copy_texture_region_to_buffer.shape"});
    }
    const UINT rowPitch = (a_region.width * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                          ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    const auto required = static_cast<std::uint64_t>(rowPitch) * (a_region.height - 1u) +
                          static_cast<std::uint64_t>(a_region.width) * 4u;
    if (a_region.destinationOffset > destination->GetDesc().Width ||
        required > destination->GetDesc().Width - a_region.destinationOffset)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.copy_texture_region_to_buffer.range"});
    }
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = destination;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint.Offset = a_region.destinationOffset;
    target.PlacedFootprint.Footprint = {desc.Format, a_region.width, a_region.height, 1, rowPitch};
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = source;
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{a_region.sourceX, a_region.sourceY, 0,
                         a_region.sourceX + a_region.width, a_region.sourceY + a_region.height, 1};
    m_list->CopyTextureRegion(&target, 0, 0, 0, &sourceLocation, &box);
    return Result<void>::success();
}

/// @brief 同じ寸法と Format の Texture 全体をコピーする
Result<void> DX12CommandRecorder::copy_texture(GpuResourceHandle a_destination,
                                                 GpuResourceHandle a_source)
{
    auto destinationResult = m_resources->resource(a_destination);
    auto sourceResult = m_resources->resource(a_source);
    if (!m_list || !destinationResult.has_value() || !sourceResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.copy_texture"});
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
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.copy_texture.shape"});
    }
    m_list->CopyResource(destination, source);
    return Result<void>::success();
}

/// @brief Presentation 所有の Back Buffer を借用して Copy Command を記録する
Result<void> DX12CommandRecorder::copy_to_back_buffer(ID3D12Resource* a_backBuffer,
                                                        GpuResourceHandle a_source)
{
    auto sourceResult = m_resources->resource(a_source);
    if (!m_list || m_queue != GpuQueueType::Graphics || !a_backBuffer || !sourceResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.copy_to_back_buffer"});
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
                                      "DX12CommandRecorder.copy_to_back_buffer.shape"});
    }
    m_list->CopyResource(a_backBuffer, source);
    return Result<void>::success();
}

/// @brief Queue と PSO の種類を照合して Root Signature も設定する
Result<void> DX12CommandRecorder::bind_pipeline(GpuPipelineHandle a_pipeline)
{
    if (!m_list || m_queue == GpuQueueType::Copy ||
        (m_queue == GpuQueueType::Compute && a_pipeline.kind != GpuPipelineKind::Compute))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12CommandRecorder.bind_pipeline"});
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
Result<void> DX12CommandRecorder::bind_view(std::uint32_t a_parameter, GpuViewHandle a_view)
{
    if (!m_list || !m_hasPipeline || !m_pipelines->accepts_view(m_pipeline, a_parameter, a_view.kind))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.bind_view"});
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

/// @brief Root Signature の定数 Parameter に 32 bit 値を設定する
Result<void> DX12CommandRecorder::set_32bit_constant(std::uint32_t a_parameter, std::uint32_t a_value)
{
    if (!m_list || !m_hasPipeline ||
        !m_pipelines->accepts_root_parameter(m_pipeline, a_parameter, GpuRootParameterType::Constants32))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.set_32bit_constant"});
    }
    if (m_pipeline.kind == GpuPipelineKind::Graphics)
    {
        m_list->SetGraphicsRoot32BitConstant(a_parameter, a_value, 0);
    }
    else
    {
        m_list->SetComputeRoot32BitConstant(a_parameter, a_value, 0);
    }
    return Result<void>::success();
}

/// @brief 指定 Heap Slice の GPU Address を Root Descriptor へ渡す
Result<void> DX12CommandRecorder::set_root_buffer(std::uint32_t a_parameter,
    GpuResourceHandle a_buffer, GpuRootParameterType a_type, GpuMemory a_memory,
    std::uint32_t a_resourceIndex)
{
    if (!m_list || !m_hasPipeline ||
        (a_type != GpuRootParameterType::Cbv && a_type != GpuRootParameterType::Srv &&
         a_type != GpuRootParameterType::Uav) ||
        !m_pipelines->accepts_root_parameter(m_pipeline, a_parameter, a_type))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.set_root_buffer"});
    }
    auto resourceResult = m_resources->resource(a_buffer, a_memory, a_resourceIndex);
    if (!resourceResult.has_value())
    {
        return Result<void>::failure(*resourceResult.try_error());
    }
    ID3D12Resource* resource = resourceResult.take_value();
    if (resource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.set_root_buffer.type"});
    }
    const auto address = resource->GetGPUVirtualAddress();
    if (m_pipeline.kind == GpuPipelineKind::Graphics)
    {
        switch (a_type)
        {
        case GpuRootParameterType::Cbv: m_list->SetGraphicsRootConstantBufferView(a_parameter, address); break;
        case GpuRootParameterType::Srv: m_list->SetGraphicsRootShaderResourceView(a_parameter, address); break;
        case GpuRootParameterType::Uav: m_list->SetGraphicsRootUnorderedAccessView(a_parameter, address); break;
        default: break;
        }
    }
    else
    {
        switch (a_type)
        {
        case GpuRootParameterType::Cbv: m_list->SetComputeRootConstantBufferView(a_parameter, address); break;
        case GpuRootParameterType::Srv: m_list->SetComputeRootShaderResourceView(a_parameter, address); break;
        case GpuRootParameterType::Uav: m_list->SetComputeRootUnorderedAccessView(a_parameter, address); break;
        default: break;
        }
    }
    return Result<void>::success();
}

/// @brief Texture Table に Shader Heap 全体の先頭を Bind する
Result<void> DX12CommandRecorder::set_graphics_texture_table(std::uint32_t a_parameter)
{
    if (!m_list || !m_hasPipeline || m_pipeline.kind != GpuPipelineKind::Graphics ||
        !m_pipelines->accepts_texture_table(m_pipeline, a_parameter,
                                            m_resources->texture_table_capacity()))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.set_graphics_texture_table"});
    }
    ID3D12DescriptorHeap* heap = m_resources->shader_heap();
    if (!heap)
    {
        return Result<void>::failure({ErrorCategory::InvalidState,
                                      "DX12CommandRecorder.set_graphics_texture_table.heap"});
    }
    m_list->SetDescriptorHeaps(1, &heap);
    m_list->SetGraphicsRootDescriptorTable(a_parameter, heap->GetGPUDescriptorHandleForHeapStart());
    return Result<void>::success();
}

/// @brief Graphics Queue の RTV と任意の DSV を Output Merger へ設定する
Result<void> DX12CommandRecorder::set_render_targets(GpuViewHandle a_color,
                                                       std::optional<GpuViewHandle> a_depth)
{
    return set_render_targets_many(std::span<const GpuViewHandle>(&a_color, 1), a_depth);
}

/// @brief 複数 RTV と任意 DSV を Output Merger へ設定する
Result<void> DX12CommandRecorder::set_render_targets_many(std::span<const GpuViewHandle> a_colors,
                                                            std::optional<GpuViewHandle> a_depth)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_colors.empty() || a_colors.size() > 8 ||
        (a_depth && a_depth->kind != GpuViewKind::DepthStencil))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.set_render_targets_many"});
    }
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> colors{};
    for (std::size_t index = 0; index < a_colors.size(); ++index)
    {
        if (a_colors[index].kind != GpuViewKind::RenderTarget)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument,
                                          "DX12CommandRecorder.set_render_targets_many.kind"});
        }
        auto colorResult = m_resources->cpu_handle(a_colors[index]);
        if (!colorResult.has_value())
        {
            return Result<void>::failure(*colorResult.try_error());
        }
        colors[index] = colorResult.take_value();
    }
    if (a_depth)
    {
        auto depthResult = m_resources->cpu_handle(*a_depth);
        if (!depthResult.has_value())
        {
            return Result<void>::failure(*depthResult.try_error());
        }
        const auto depth = depthResult.take_value();
        m_list->OMSetRenderTargets(static_cast<UINT>(a_colors.size()), colors.data(), FALSE, &depth);
    }
    else
    {
        m_list->OMSetRenderTargets(static_cast<UINT>(a_colors.size()), colors.data(), FALSE, nullptr);
    }
    return Result<void>::success();
}

/// @brief Graphics Queue の Color Target を消去する
Result<void> DX12CommandRecorder::clear_color(GpuViewHandle a_color,
                                                const std::array<float, 4>& a_value)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_color.kind != GpuViewKind::RenderTarget)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.clear_color"});
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
Result<void> DX12CommandRecorder::clear_depth(GpuViewHandle a_depth, float a_value)
{
    return clear_depth_stencil(a_depth, a_value, 0);
}

/// @brief D24 の Stencil を含め、View Format に応じた Clear Flag を設定する
Result<void> DX12CommandRecorder::clear_depth_stencil(GpuViewHandle a_depth, float a_depthValue,
                                                        std::uint8_t a_stencilValue)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_depth.kind != GpuViewKind::DepthStencil ||
        a_depthValue < 0.0f || a_depthValue > 1.0f)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.clear_depth_stencil"});
    }
    auto handleResult = m_resources->cpu_handle(a_depth);
    auto formatResult = m_resources->view_texture_format(a_depth);
    if (!handleResult.has_value() || !formatResult.has_value())
    {
        return Result<void>::failure(!handleResult.has_value() ? *handleResult.try_error()
                                                              : *formatResult.try_error());
    }
    const auto format = formatResult.take_value();
    if (format != GpuTextureFormat::Depth24Stencil8 && a_stencilValue != 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.clear_depth_stencil.format"});
    }
    const auto flags = format == GpuTextureFormat::Depth24Stencil8
        ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH;
    m_list->ClearDepthStencilView(handleResult.take_value(), flags, a_depthValue, a_stencilValue, 0, nullptr);
    return Result<void>::success();
}

/// @brief Shader-visible UAV の CPU/GPU Handle を揃えて整数値で消去する
Result<void> DX12CommandRecorder::clear_unordered_access_uint(GpuViewHandle a_view,
    const std::array<std::uint32_t, 4>& a_values)
{
    if (!m_list || m_queue == GpuQueueType::Copy || a_view.kind != GpuViewKind::UnorderedAccess)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.clear_unordered_access_uint"});
    }
    auto cpuResult = m_resources->cpu_handle(a_view);
    auto gpuResult = m_resources->gpu_handle(a_view);
    auto resourceResult = m_resources->view_resource(a_view);
    if (!cpuResult.has_value() || !gpuResult.has_value() || !resourceResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidState,
                                      "DX12CommandRecorder.clear_unordered_access_uint.view"});
    }
    ID3D12DescriptorHeap* heap = m_resources->shader_heap();
    m_list->SetDescriptorHeaps(1, &heap);
    m_list->ClearUnorderedAccessViewUint(gpuResult.take_value(), cpuResult.take_value(),
                                         resourceResult.take_value(), a_values.data(), 0, nullptr);
    return Result<void>::success();
}

/// @brief Buffer 範囲を Vertex Buffer View として設定する
Result<void> DX12CommandRecorder::bind_vertex_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                       std::uint32_t a_stride, std::uint32_t a_size)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_stride == 0 || a_size == 0 ||
        a_size % a_stride != 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.bind_vertex_buffer"});
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
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.vertexRange"});
    }
    D3D12_VERTEX_BUFFER_VIEW view{resource->GetGPUVirtualAddress() + a_offset, a_size, a_stride};
    m_list->IASetVertexBuffers(0, 1, &view);
    m_list->IASetPrimitiveTopology(m_topology);
    return Result<void>::success();
}

/// @brief 16 bit Index 範囲を検証して Input Assembler へ設定する
Result<void> DX12CommandRecorder::bind_index_buffer(GpuResourceHandle a_buffer, std::uint64_t a_offset,
                                                      std::uint32_t a_size)
{
    return bind_index_buffer_format(a_buffer, a_offset, a_size, GpuIndexFormat::Uint16);
}

/// @brief Index 要素幅を照合して 16 bit／32 bit の Buffer View を設定する
Result<void> DX12CommandRecorder::bind_index_buffer_format(GpuResourceHandle a_buffer,
    std::uint64_t a_offset, std::uint32_t a_size, GpuIndexFormat a_format)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_size == 0 ||
        (a_format != GpuIndexFormat::Uint16 && a_format != GpuIndexFormat::Uint32))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.bind_index_buffer"});
    }
    const auto elementSize = a_format == GpuIndexFormat::Uint16 ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
    auto resourceResult = m_resources->resource(a_buffer);
    if (!resourceResult.has_value())
    {
        return Result<void>::failure(*resourceResult.try_error());
    }
    ID3D12Resource* resource = resourceResult.take_value();
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || a_offset > desc.Width ||
        a_size > desc.Width - a_offset || a_offset % elementSize != 0 || a_size % elementSize != 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.indexRange"});
    }
    D3D12_INDEX_BUFFER_VIEW view{resource->GetGPUVirtualAddress() + a_offset, a_size,
        a_format == GpuIndexFormat::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
    m_list->IASetIndexBuffer(&view);
    return Result<void>::success();
}

/// @brief Surface 全体を描く Pass の Viewport と Scissor を設定する
Result<void> DX12CommandRecorder::set_viewport(std::uint32_t a_width, std::uint32_t a_height)
{
    return set_viewport_rect(0, 0, a_width, a_height);
}

/// @brief 正の矩形が LONG の Scissor 範囲に収まることを確認して設定する
Result<void> DX12CommandRecorder::set_viewport_rect(std::uint32_t a_x, std::uint32_t a_y,
                                                      std::uint32_t a_width, std::uint32_t a_height)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || a_width == 0 || a_height == 0 ||
        a_x > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
        a_y > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
        a_width > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) - a_x ||
        a_height > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) - a_y)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12CommandRecorder.set_viewport"});
    }
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = static_cast<float>(a_x);
    viewport.TopLeftY = static_cast<float>(a_y);
    viewport.Width = static_cast<float>(a_width);
    viewport.Height = static_cast<float>(a_height);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{static_cast<LONG>(a_x), static_cast<LONG>(a_y),
                             static_cast<LONG>(a_x + a_width), static_cast<LONG>(a_y + a_height)};
    m_list->RSSetViewports(1, &viewport);
    m_list->RSSetScissorRects(1, &scissor);
    return Result<void>::success();
}

/// @brief Graphics Queue の Input Assembler Topology を指定する
Result<void> DX12CommandRecorder::set_primitive_topology(GpuPrimitiveTopology a_topology)
{
    if (!m_list || m_queue != GpuQueueType::Graphics)
    {
        return Result<void>::failure({ErrorCategory::InvalidState,
                                      "DX12CommandRecorder.set_primitive_topology"});
    }
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    switch (a_topology)
    {
    case GpuPrimitiveTopology::Point: topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case GpuPrimitiveTopology::Line: topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case GpuPrimitiveTopology::Triangle: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    default: return Result<void>::failure({ErrorCategory::InvalidArgument,
                                           "DX12CommandRecorder.set_primitive_topology.type"});
    }
    m_topology = topology;
    m_list->IASetPrimitiveTopology(m_topology);
    return Result<void>::success();
}

/// @brief Graphics PSO 設定後だけ Draw Command を記録する
Result<void> DX12CommandRecorder::draw(std::uint32_t a_vertices, std::uint32_t a_instances)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || !m_hasPipeline ||
        m_pipeline.kind != GpuPipelineKind::Graphics || a_vertices == 0 || a_instances == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12CommandRecorder.draw"});
    }
    m_list->IASetPrimitiveTopology(m_topology);
    m_list->DrawInstanced(a_vertices, a_instances, 0, 0);
    return Result<void>::success();
}

/// @brief Graphics PSO と Index Buffer 設定後に DrawIndexed を記録する
Result<void> DX12CommandRecorder::draw_indexed(std::uint32_t a_indices, std::uint32_t a_instances)
{
    return draw_indexed_range(a_indices, a_instances, 0, 0, 0);
}

/// @brief Index と Vertex の開始位置を保って Mesh Slice を描画する
Result<void> DX12CommandRecorder::draw_indexed_range(std::uint32_t a_indices,
    std::uint32_t a_instances, std::uint32_t a_startIndex, std::int32_t a_baseVertex,
    std::uint32_t a_startInstance)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || !m_hasPipeline ||
        m_pipeline.kind != GpuPipelineKind::Graphics || a_indices == 0 || a_instances == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12CommandRecorder.draw_indexed"});
    }
    m_list->IASetPrimitiveTopology(m_topology);
    m_list->DrawIndexedInstanced(a_indices, a_instances, a_startIndex, a_baseVertex, a_startInstance);
    return Result<void>::success();
}

/// @brief Legacy と同じ 24 byte Stride で間接 Index 描画を記録する
Result<void> DX12CommandRecorder::execute_indexed_indirect(GpuResourceHandle a_commands,
    GpuResourceHandle a_count, std::uint32_t a_maxCommandCount)
{
    if (!m_list || m_queue != GpuQueueType::Graphics || !m_hasPipeline ||
        m_pipeline.kind != GpuPipelineKind::Graphics || a_maxCommandCount == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidState,
                                      "DX12CommandRecorder.execute_indexed_indirect"});
    }
    auto signatureResult = m_pipelines->indirect_signature(m_pipeline);
    auto commandsResult = m_resources->resource(a_commands, GpuMemory::Device, 0);
    auto countResult = m_resources->resource(a_count, GpuMemory::Device, 0);
    if (!signatureResult.has_value() || !commandsResult.has_value() || !countResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.execute_indexed_indirect.resources"});
    }
    ID3D12Resource* commands = commandsResult.take_value();
    ID3D12Resource* count = countResult.take_value();
    constexpr std::uint64_t k_commandStride = sizeof(std::uint32_t) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    if (commands->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        count->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        a_maxCommandCount > commands->GetDesc().Width / k_commandStride ||
        count->GetDesc().Width < sizeof(std::uint32_t))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12CommandRecorder.execute_indexed_indirect.range"});
    }
    m_list->IASetPrimitiveTopology(m_topology);
    m_list->ExecuteIndirect(signatureResult.take_value(), a_maxCommandCount, commands, 0, count, 0);
    return Result<void>::success();
}

/// @brief Compute PSO 設定後だけ Dispatch Command を記録する
Result<void> DX12CommandRecorder::dispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
{
    if (!m_list || m_queue == GpuQueueType::Copy || !m_hasPipeline ||
        m_pipeline.kind != GpuPipelineKind::Compute || a_x == 0 || a_y == 0 || a_z == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12CommandRecorder.dispatch"});
    }
    m_list->Dispatch(a_x, a_y, a_z);
    return Result<void>::success();
}

/// @brief RHI 状態を DX12 の完全な Shader 可視性へ対応付ける
Result<D3D12_RESOURCE_STATES> DX12CommandRecorder::resource_state(GpuResourceState a_state)
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
    case GpuResourceState::IndirectArgument:
        return Result<D3D12_RESOURCE_STATES>::success(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    default:
        return Result<D3D12_RESOURCE_STATES>::failure({ErrorCategory::InvalidArgument,
                                                       "DX12CommandRecorder.resource_state"});
    }
}

/// @brief Queue 固有の Barrier State 制限を確認する
bool DX12CommandRecorder::supports(GpuResourceState a_state) const noexcept
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
