#include <DX12/DX12FrameGraphContext.h>

#include <algorithm>
#include <limits>
#include <new>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12FrameGraphFrames.h>

#include <DX12/DX12PipelineManager.h>

namespace cue::dx12
{
/// @brief Pass が参照する情報を記録期間へ限定する
DX12FrameGraphContext::DX12FrameGraphContext(const DX12FrameGraphRecordContext &a_record) noexcept
    : FrameGraphContext(a_record.width, a_record.height, a_record.frameIndex, a_record.command),
      m_command(&a_record.command), m_resources(&a_record.resources), m_plan(&a_record.plan), m_pass(&a_record.pass),
      m_frames(&a_record.frames), m_pipelines(&a_record.pipelines)
{
}

/// @brief 実行中 Pass の Resource 使用宣言だけを許可する
bool DX12FrameGraphContext::allows(FrameGraphResourceHandle a_handle, FrameGraphAccess a_access,
                                    FrameGraphResourceState a_state) const noexcept
{
    return std::any_of(m_pass->uses.begin(), m_pass->uses.end(),
                       [a_handle, a_access, a_state](const FrameGraphUse& a_use)
                       {
                           return a_use.resource.graphId == a_handle.graphId &&
                                  a_use.resource.index == a_handle.index && a_use.access == a_access &&
                                  a_use.state == a_state;
                       });
}

/// @brief 論理 Handle と枠の RTV の対応を検証して Clear を記録する
Result<void> DX12FrameGraphContext::clear_render_target(
    FrameGraphResourceHandle a_target, const std::array<float, 4>& a_color)
{
    if (!allows(a_target, FrameGraphAccess::Write, FrameGraphResourceState::RenderTarget) ||
        a_target.index >= m_plan->resources().size() || !resource(a_target) ||
        m_command->type() != QueueType::Graphics || m_command->state() != CommandState::Recording ||
        !m_command->command_list())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FrameGraphContext.clear_render_target"});
    }
    // Placed RT の最適化 Clear 値と異なる色は Debug Layer 上で記録を進めない
    if (m_plan->resources()[a_target.index].textureDesc.clearColor != a_color)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12FrameGraphContext.clear_render_target.color"});
    }
    auto rtvResult = m_frames->rtv(frame_index(), a_target);
    if (!rtvResult.has_value())
    {
        return Result<void>::failure(*rtvResult.try_error());
    }
    command_list().ClearRenderTargetView(*rtvResult.try_value(), a_color.data(), 0, nullptr);
    return Result<void>::success();
}

/// @brief Pass が宣言した任意の RTV を単一描画先として Bind する
Result<void> DX12FrameGraphContext::set_render_target(FrameGraphResourceHandle a_target)
{
    if (!allows(a_target, FrameGraphAccess::Write, FrameGraphResourceState::RenderTarget) ||
        !resource(a_target) || m_command->type() != QueueType::Graphics ||
        m_command->state() != CommandState::Recording || !m_command->command_list())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FrameGraphContext.set_render_target"});
    }
    auto rtvResult = m_frames->rtv(frame_index(), a_target);
    if (!rtvResult.has_value())
    {
        return Result<void>::failure(*rtvResult.try_error());
    }
    const auto targetFormat = m_plan->resources()[a_target.index].textureDesc.format;
    if (m_pipelineFormat && *m_pipelineFormat != targetFormat)
    {
        return Result<void>::failure(
            {ErrorCategory::InvalidArgument, "DX12FrameGraphContext.set_render_target.format"});
    }
    const auto rtv = *rtvResult.try_value();
    m_targetFormat = targetFormat;
    command_list().OMSetRenderTargets(1, &rtv, false, nullptr);
    return Result<void>::success();
}

/// @brief ShaderRead の SRV を現在の Graphics Root Signature に Bind する
Result<void> DX12FrameGraphContext::bind_texture2d(FrameGraphResourceHandle a_source,
                                                    std::uint32_t a_rootParameter)
{
    if (!allows(a_source, FrameGraphAccess::Read, FrameGraphResourceState::ShaderRead) ||
        !resource(a_source) || !m_frames->srv_heap() || m_command->type() != QueueType::Graphics ||
        m_command->state() != CommandState::Recording || !m_command->command_list())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FrameGraphContext.bind_texture2d"});
    }
    auto validation = m_pipelines->validate_texture_binding(m_pipeline, a_rootParameter);
    if (!validation.has_value())
    {
        return validation;
    }
    auto srvResult = m_frames->srv(frame_index(), a_source);
    if (!srvResult.has_value())
    {
        return Result<void>::failure(*srvResult.try_error());
    }
    if (std::find(m_boundParameters.begin(), m_boundParameters.end(), a_rootParameter) == m_boundParameters.end())
    {
        try
        {
            m_boundParameters.push_back(a_rootParameter);
        }
        catch (const std::bad_alloc &)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "DX12FrameGraphContext.binding.allocation"});
        }
    }
    if (!m_isSrvHeapBound)
    {
        ID3D12DescriptorHeap* heaps[] = {m_frames->srv_heap()};
        command_list().SetDescriptorHeaps(1, heaps);
        m_isSrvHeapBound = true;
    }
    command_list().SetGraphicsRootDescriptorTable(a_rootParameter, *srvResult.try_value());
    return Result<void>::success();
}

/// @brief Pipeline の Native 実体を Command に保持して Graphics Binding を初期化する
Result<void> DX12FrameGraphContext::set_graphics_pipeline(PipelineStateHandle a_pipeline)
{
    auto result = m_pipelines->bind_graphics(*m_command, a_pipeline);
    if (!result.has_value())
    {
        return Result<void>::failure(*result.try_error());
    }
    m_pipeline = a_pipeline;
    m_pipelineFormat = result.take_value();
    m_targetFormat.reset();
    m_boundParameters.clear();
    m_isComputeBound = false;
    return Result<void>::success();
}

/// @brief Graphics の Binding を引き継がず Compute Root と Pipeline を設定する
Result<void> DX12FrameGraphContext::set_compute_pipeline(PipelineStateHandle a_pipeline)
{
    auto result = m_pipelines->bind_compute(*m_command, a_pipeline);
    if (!result.has_value())
    {
        return result;
    }
    m_pipeline = a_pipeline;
    m_pipelineFormat.reset();
    m_targetFormat.reset();
    m_boundParameters.clear();
    m_isComputeBound = true;
    return Result<void>::success();
}

/// @brief Native Scissor の整数範囲と Graph の描画範囲を検証する
Result<void> DX12FrameGraphContext::set_viewport_scissor(std::uint32_t a_width, std::uint32_t a_height)
{
    if (a_width == 0 || a_height == 0 || a_width > width() || a_height > height() ||
        a_width > static_cast<std::uint32_t>((std::numeric_limits<LONG>::max)()) ||
        a_height > static_cast<std::uint32_t>((std::numeric_limits<LONG>::max)()) ||
        m_command->type() != QueueType::Graphics || m_command->state() != CommandState::Recording)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12FrameGraphContext.viewport"});
    }
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(a_width), static_cast<float>(a_height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height)};
    command_list().RSSetViewports(1, &viewport);
    command_list().RSSetScissorRects(1, &scissor);
    m_hasViewport = true;
    return Result<void>::success();
}

/// @brief Pipeline、Target、Viewport と Root Binding が揃ってから Draw を記録する
Result<void> DX12FrameGraphContext::draw_instanced(std::uint32_t a_vertexCount, std::uint32_t a_instanceCount,
                                                   std::uint32_t a_firstVertex, std::uint32_t a_firstInstance)
{
    if (!m_pipelineFormat || !m_targetFormat || m_pipelineFormat != m_targetFormat || !m_hasViewport ||
        a_vertexCount == 0 || a_instanceCount == 0 || m_command->type() != QueueType::Graphics ||
        m_command->state() != CommandState::Recording)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FrameGraphContext.draw"});
    }
    auto validation = m_pipelines->validate_bindings(m_pipeline, m_boundParameters);
    if (!validation.has_value())
    {
        return validation;
    }
    command_list().DrawInstanced(a_vertexCount, a_instanceCount, a_firstVertex, a_firstInstance);
    return Result<void>::success();
}

/// @brief 未設定 Root Binding と D3D12 の Group 上限超過を拒否する
Result<void> DX12FrameGraphContext::dispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
{
    if (!m_isComputeBound || a_x == 0 || a_y == 0 || a_z == 0 || a_x > 65535 || a_y > 65535 || a_z > 65535 ||
        m_command->type() == QueueType::Copy || m_command->state() != CommandState::Recording)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12FrameGraphContext.dispatch"});
    }
    auto validation = m_pipelines->validate_bindings(m_pipeline, {});
    if (!validation.has_value())
    {
        return validation;
    }
    command_list().Dispatch(a_x, a_y, a_z);
    return Result<void>::success();
}

/// @brief 同一形状で別の物理 Texture にだけ Copy を記録する
Result<void> DX12FrameGraphContext::copy_texture2d(
    FrameGraphResourceHandle a_source, FrameGraphResourceHandle a_destination)
{
    auto* source = resource(a_source);
    auto* destination = resource(a_destination);
    if (!allows(a_source, FrameGraphAccess::Read, FrameGraphResourceState::CopySource) ||
        !allows(a_destination, FrameGraphAccess::Write, FrameGraphResourceState::CopyDestination) ||
        !source || !destination || source == destination ||
        (m_command->type() != QueueType::Graphics && m_command->type() != QueueType::Copy) ||
        m_command->state() != CommandState::Recording ||
        !m_command->command_list())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FrameGraphContext.copy_texture2d"});
    }
    const auto sourceDesc = source->GetDesc();
    const auto destinationDesc = destination->GetDesc();
    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        destinationDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sourceDesc.Width != destinationDesc.Width || sourceDesc.Height != destinationDesc.Height ||
        sourceDesc.Format != destinationDesc.Format ||
        sourceDesc.DepthOrArraySize != destinationDesc.DepthOrArraySize ||
        sourceDesc.MipLevels != destinationDesc.MipLevels ||
        sourceDesc.SampleDesc.Count != destinationDesc.SampleDesc.Count ||
        sourceDesc.SampleDesc.Quality != destinationDesc.SampleDesc.Quality)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FrameGraphContext.copy_texture2d.shape"});
    }
    command_list().CopyResource(destination, source);
    return Result<void>::success();
}

/// @brief 記録中の List を返す
ID3D12GraphicsCommandList& DX12FrameGraphContext::command_list() const noexcept
{
    return *m_command->command_list();
}

/// @brief Native 記録の入口で Target を検証し、通常 Context の State Cache を引き継がない
Result<void> DX12FrameGraphContext::record_external_graphics(
    GpuTextureFormat a_format, const std::function<Result<void>(ID3D12GraphicsCommandList &)> &a_record)
{
    auto valid = validate_external_graphics(a_format);
    if (!a_record || !valid.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FrameGraphContext.external_graphics"});
    }
    // Callback が Context API を使っても、Native 状態を後から書き換えた Cache は残さない
    const auto invalidate = [&]()
    {
        m_pipeline = {};
        m_pipelineFormat.reset();
        m_targetFormat.reset();
        m_boundParameters.clear();
        m_isComputeBound = false;
        m_hasViewport = false;
        m_isSrvHeapBound = false;
    };
    invalidate();
    Result<void> result = Result<void>::success();
    try
    {
        result = a_record(command_list());
    }
    catch (...)
    {
        result = Result<void>::failure({ErrorCategory::PlatformFailure, "DX12FrameGraphContext.external_callback"});
    }
    invalidate();
    return result;
}

/// @brief 未設定 Target と不一致の Format を副作用なしで拒否する
Result<void> DX12FrameGraphContext::validate_external_graphics(GpuTextureFormat a_format) const
{
    if (m_command->type() != QueueType::Graphics || m_command->state() != CommandState::Recording ||
        !m_command->command_list() || !m_targetFormat || *m_targetFormat != a_format)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FrameGraphContext.external_graphics"});
    }
    return Result<void>::success();
}

/// @brief 検証済み対応表から Native Resource を返す
ID3D12Resource* DX12FrameGraphContext::resource(FrameGraphResourceHandle a_handle) const noexcept
{
    return m_resources->resource(a_handle);
}
} // namespace cue::dx12
