#pragma once

#include "DX12PipelineCache.h"
#include "DX12StaticMeshPool.h"

#include <Cue/Renderer/FrameGraph/FrameGraphPass.h>

#include <array>

namespace cue::detail
{
/// @brief 一 Frame の三角形 Pass が借用する資源と描画値
struct TrianglePassContext final
{
    DX12PipelineCache& pipelines;
    DX12StaticMeshPool& meshes;
    GraphResourceHandle color;
    GraphResourceHandle depth;
    StaticMeshHandle mesh;
    GpuViewHandle colorView;
    GpuViewHandle depthView;
    std::array<float, 4> tint;
};

/// @brief 固定三角形の Resource 宣言と描画を一 Frame 分所有する
class DX12TrianglePass final : public FrameGraphPass
{
public:
    /// @brief 呼出元の State が生存する一 Frame 中だけ資源を借用する
    explicit DX12TrianglePass(TrianglePassContext a_context) noexcept;

    /// @brief Graph に表示する名前を返す
    [[nodiscard]] const char* name() const noexcept override;

    /// @brief Color と Depth の書き込みを宣言する
    [[nodiscard]] Result<GraphPassHandle> setup(FrameGraphBuilder& a_builder) const override;

    /// @brief PSO と Mesh を設定して固定三角形を描画する
    [[nodiscard]] Result<void> execute(FrameGraphContext& a_context) const override;

private:
    TrianglePassContext m_context;
};
} // namespace cue::detail
