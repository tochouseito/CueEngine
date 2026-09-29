#pragma once

#include "D3D12PipelineCache.h"
#include "D3D12StaticMeshPool.h"

#include <Cue/Renderer/FrameGraph/FrameGraph.h>

#include <array>

namespace cue::detail
{
/// @brief GPU 命令を RHI Recorder に記録する FrameGraph Pass の契約
class D3D12FrameGraphPass
{
public:
    /// @brief 借用資源を保持せずに Pass を破棄する
    virtual ~D3D12FrameGraphPass() = default;

    /// @brief Graph に登録する Pass 名を返す
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// @brief Graph に Resource 使用を宣言する
    [[nodiscard]] virtual Result<GraphPassHandle> setup(FrameGraphBuilder& a_builder) const = 0;

    /// @brief Graph の Barrier 適用後に描画命令を記録する
    [[nodiscard]] virtual Result<void> execute(IGpuCommandRecorder& a_commands) const = 0;
};

/// @brief 一 Frame の三角形 Pass が借用する資源と描画値
struct TrianglePassContext final
{
    D3D12PipelineCache& pipelines;
    D3D12StaticMeshPool& meshes;
    GraphResourceHandle color;
    GraphResourceHandle depth;
    StaticMeshHandle mesh;
    WindowSize size;
    UINT slot;
    GpuViewHandle colorView;
    GpuViewHandle depthView;
    std::array<float, 4> tint;
};

/// @brief 固定三角形の Resource 宣言と描画を一 Frame 分所有する
class D3D12TrianglePass final : public D3D12FrameGraphPass
{
public:
    /// @brief 呼出元の State が生存する一 Frame 中だけ資源を借用する
    explicit D3D12TrianglePass(TrianglePassContext a_context) noexcept;

    /// @brief Graph に表示する名前を返す
    [[nodiscard]] const char* name() const noexcept override;

    /// @brief Color と Depth の書き込みを宣言する
    [[nodiscard]] Result<GraphPassHandle> setup(FrameGraphBuilder& a_builder) const override;

    /// @brief PSO と Mesh を設定して固定三角形を描画する
    [[nodiscard]] Result<void> execute(IGpuCommandRecorder& a_commands) const override;

private:
    TrianglePassContext m_context;
};
} // namespace cue::detail
