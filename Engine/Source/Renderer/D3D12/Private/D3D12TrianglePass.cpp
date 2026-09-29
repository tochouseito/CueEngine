#include "D3D12TrianglePass.h"

namespace cue::detail
{
/// @brief 呼出元の State が生存する一 Frame 中だけ資源を借用する
D3D12TrianglePass::D3D12TrianglePass(TrianglePassContext a_context) noexcept
    : m_context(a_context)
{
}

/// @brief Graph に表示する名前を返す
const char* D3D12TrianglePass::name() const noexcept
{
    return "FixedMesh";
}

/// @brief Color と Depth の書き込みを宣言する
Result<GraphPassHandle> D3D12TrianglePass::setup(FrameGraphBuilder& a_builder) const
{
    return a_builder.add_pass(name(), {{m_context.color, GraphResourceState::RenderTarget, GraphAccess::Write},
                                       {m_context.depth, GraphResourceState::DepthWrite, GraphAccess::Write}});
}

/// @brief PSO と Mesh を設定して固定三角形を描画する
Result<void> D3D12TrianglePass::execute(IGpuCommandRecorder& a_commands) const
{
    auto bindResult = m_context.pipelines.bind(a_commands, m_context.slot, m_context.tint);
    if (!bindResult.has_value())
    {
        return bindResult;
    }
    auto viewportResult = a_commands.set_viewport(m_context.size.width, m_context.size.height);
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
