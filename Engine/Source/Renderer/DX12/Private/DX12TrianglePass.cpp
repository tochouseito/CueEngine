#include "DX12TrianglePass.h"

namespace cue::detail
{
/// @brief 呼出元の State が生存する一 Frame 中だけ資源を借用する
DX12TrianglePass::DX12TrianglePass(TrianglePassContext a_context) noexcept
    : m_context(a_context)
{
}

/// @brief Graph に表示する名前を返す
const char* DX12TrianglePass::name() const noexcept
{
    return "FixedMesh";
}

/// @brief Color と Depth の書き込みを宣言する
Result<GraphPassHandle> DX12TrianglePass::setup(FrameGraphBuilder& a_builder) const
{
    return a_builder.add_pass(name(), {{m_context.color, GraphResourceState::RenderTarget, GraphAccess::Write},
                                       {m_context.depth, GraphResourceState::DepthWrite, GraphAccess::Write}});
}

/// @brief PSO と Mesh を設定して固定三角形を描画する
Result<void> DX12TrianglePass::execute(FrameGraphContext& a_context) const
{
    IGpuCommandRecorder& a_commands = a_context.commands();
    auto bindResult = m_context.pipelines.bind(a_commands, a_context.frame_index(), m_context.tint);
    if (!bindResult.has_value())
    {
        return bindResult;
    }
    auto viewportResult = a_commands.set_viewport(a_context.width(), a_context.height());
    if (!viewportResult.has_value())
    {
        return viewportResult;
    }
    auto targetsResult = a_commands.set_render_targets(m_context.colorView, m_context.depthView);
    if (!targetsResult.has_value())
    {
        return targetsResult;
    }
    return m_context.meshes.draw(a_commands, m_context.mesh);
}
} // namespace cue::detail
