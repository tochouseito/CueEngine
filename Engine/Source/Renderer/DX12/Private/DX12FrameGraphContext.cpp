#include <DX12/DX12FrameGraphContext.h>

#include <algorithm>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12FrameGraphFrames.h>

#include "DX12FullscreenTriangle.h"

namespace cue::dx12
{
/// @brief Pass が参照する情報を記録期間へ限定する
DX12FrameGraphContext::DX12FrameGraphContext(const DX12FrameGraphRecordContext &a_record) noexcept
    : FrameGraphContext(a_record.width, a_record.height, a_record.frameIndex, a_record.command),
      m_command(&a_record.command), m_resources(&a_record.resources), m_plan(&a_record.plan), m_pass(&a_record.pass),
      m_frames(&a_record.frames), m_fullscreenTriangle(&a_record.fullscreenTriangle)
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
    const auto rtv = *rtvResult.try_value();
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
    auto srvResult = m_frames->srv(frame_index(), a_source);
    if (!srvResult.has_value())
    {
        return Result<void>::failure(*srvResult.try_error());
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

/// @brief 宣言済み Texture と RTV を検証し、固定 Pipeline に全画面描画を記録する
Result<void> DX12FrameGraphContext::draw_fullscreen_texture(
    FrameGraphResourceHandle a_source, FrameGraphResourceHandle a_target)
{
    auto* source = resource(a_source);
    auto* target = resource(a_target);
    if (!allows(a_source, FrameGraphAccess::Read, FrameGraphResourceState::ShaderRead) ||
        !allows(a_target, FrameGraphAccess::Write, FrameGraphResourceState::RenderTarget) ||
        !source || !target || source == target || !m_frames->srv_heap() || !m_fullscreenTriangle ||
        m_command->type() != QueueType::Graphics || m_command->state() != CommandState::Recording ||
        !m_command->command_list())
    {
        return Result<void>::failure({ErrorCategory::InvalidState,
                                      "DX12FrameGraphContext.draw_fullscreen_texture"});
    }
    const auto sourceDesc = source->GetDesc();
    const auto targetDesc = target->GetDesc();
    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        targetDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sourceDesc.Width != targetDesc.Width || sourceDesc.Height != targetDesc.Height ||
        sourceDesc.Format != targetDesc.Format || sourceDesc.SampleDesc.Count != 1 ||
        targetDesc.SampleDesc.Count != 1)
    {
        return Result<void>::failure({ErrorCategory::InvalidState,
                                      "DX12FrameGraphContext.draw_fullscreen_texture.shape"});
    }
    auto srvResult = m_frames->srv(frame_index(), a_source);
    if (!srvResult.has_value())
    {
        return Result<void>::failure(*srvResult.try_error());
    }
    auto rtvResult = m_frames->rtv(frame_index(), a_target);
    if (!rtvResult.has_value())
    {
        return Result<void>::failure(*rtvResult.try_error());
    }
    // Descriptor の実体は記録前の prepare_frame で確定済みである
    m_fullscreenTriangle->draw(command_list(), *m_frames->srv_heap(), *srvResult.try_value(),
                               *rtvResult.try_value(), width(), height());
    m_isSrvHeapBound = true;
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

/// @brief 検証済み対応表から Native Resource を返す
ID3D12Resource* DX12FrameGraphContext::resource(FrameGraphResourceHandle a_handle) const noexcept
{
    return m_resources->resource(a_handle);
}
} // namespace cue::dx12
