#include "DX12FramePasses.h"

namespace cue::detail
{
/// @brief View は Backend の Surface Owner から一 Frame の間だけ借用する
DX12ClearPass::DX12ClearPass(GraphResourceHandle a_color, GraphResourceHandle a_depth,
                             GpuViewHandle a_colorView, GpuViewHandle a_depthView) noexcept
    : m_color(a_color), m_depth(a_depth), m_colorView(a_colorView), m_depthView(a_depthView)
{
}

/// @brief Graph Plan と GPU Marker で同じ名前を使う
const char* DX12ClearPass::name() const noexcept
{
    return "Clear";
}

/// @brief 後続の描画 Pass と同じ Color／Depth Resource を宣言する
Result<GraphPassHandle> DX12ClearPass::setup(FrameGraphBuilder& a_builder) const
{
    return a_builder.add_pass(name(), {{m_color, GraphResourceState::RenderTarget, GraphAccess::Write},
                                       {m_depth, GraphResourceState::DepthWrite, GraphAccess::Write}});
}

/// @brief Clear を一つの Graphics Command Context に記録する
Result<void> DX12ClearPass::execute(FrameGraphContext& a_context) const
{
    auto& commands = a_context.commands();
    auto targetsResult = commands.set_render_targets(m_colorView, m_depthView);
    if (!targetsResult.has_value())
    {
        return targetsResult;
    }
    auto colorResult = commands.clear_color(m_colorView, {0.07f, 0.13f, 0.25f, 1.0f});
    if (!colorResult.has_value())
    {
        return colorResult;
    }
    return commands.clear_depth(m_depthView, 1.0f);
}

/// @brief Swap Chain の Back Buffer は Presentation が所有したまま借用する
DX12CopyToBackBufferPass::DX12CopyToBackBufferPass(GraphResourceHandle a_source,
                                                   GraphResourceHandle a_backBuffer,
                                                   GpuResourceHandle a_sourceResource,
                                                   ID3D12Resource* a_backBufferResource) noexcept
    : m_source(a_source), m_backBuffer(a_backBuffer), m_sourceResource(a_sourceResource),
      m_backBufferResource(a_backBufferResource)
{
}

/// @brief Graph Plan と GPU Marker で同じ名前を使う
const char* DX12CopyToBackBufferPass::name() const noexcept
{
    return "CopyToBackBuffer";
}

/// @brief Copy 後に Back Buffer は Graph の最終 Barrier で Present 状態へ戻す
Result<GraphPassHandle> DX12CopyToBackBufferPass::setup(FrameGraphBuilder& a_builder) const
{
    return a_builder.add_pass(name(), {{m_source, GraphResourceState::CopySource, GraphAccess::Read},
                                       {m_backBuffer, GraphResourceState::CopyDest, GraphAccess::Write}});
}

/// @brief Back Buffer を直接持つ DX12 Recorder の拡張命令を使う
Result<void> DX12CopyToBackBufferPass::execute(FrameGraphContext& a_context) const
{
    auto* recorder = dynamic_cast<DX12CommandRecorder*>(&a_context.commands());
    if (!recorder || !m_backBufferResource)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12CopyToBackBufferPass.execute"});
    }
    return recorder->copy_to_back_buffer(m_backBufferResource, m_sourceResource);
}
} // namespace cue::detail
