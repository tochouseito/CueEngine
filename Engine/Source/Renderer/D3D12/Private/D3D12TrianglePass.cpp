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
Result<void> D3D12TrianglePass::execute(ID3D12GraphicsCommandList* a_list) const
{
    if (!a_list)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12TrianglePass.execute"});
    }
    auto bindResult = m_context.pipelines.bind(a_list, m_context.slot, m_context.tint);
    if (!bindResult.has_value())
    {
        return bindResult;
    }
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_context.size.width);
    viewport.Height = static_cast<float>(m_context.size.height);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_context.size.width),
                             static_cast<LONG>(m_context.size.height)};
    a_list->RSSetViewports(1, &viewport);
    a_list->RSSetScissorRects(1, &scissor);
    a_list->OMSetRenderTargets(1, &m_context.rtv, false, &m_context.dsv);
    return m_context.meshes.draw(a_list, m_context.mesh);
}
} // namespace cue::detail
