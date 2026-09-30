#pragma once

#include <Cue/Renderer/FrameGraph/FrameGraphPass.h>

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace cue
{
enum class FrameGraphBindingKind : std::uint8_t
{
    ManagedResource,
    SurfaceColor,
    SurfaceDepth
};

/// @brief Graph の論理 Resource が実行時に借用する物理 Owner の種類
struct FrameGraphResourceBinding final
{
    FrameGraphBindingKind kind = FrameGraphBindingKind::ManagedResource;
    GpuResourceHandle resource{};
};

/// @brief 一 Frame の論理 Resource、Pass と依存計画を所有する
/// @details Pass の借用先は execute の終了まで生存させる。Backend の単一 Render Thread で構築・実行する
class FrameGraph final
{
public:
    FrameGraph() = default;

    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;

    /// @brief Pass に渡す論理 Resource と依存を宣言する Builder を貸す
    [[nodiscard]] FrameGraphBuilder& builder() noexcept;

    /// @brief Pass の setup を検証してから所有し、元の登録順を保つ
    [[nodiscard]] Result<GraphPassHandle> add_pass(std::unique_ptr<FrameGraphPass> a_pass);

    /// @brief 登録済み Pass の依存と Barrier を確定する
    [[nodiscard]] Result<CompiledFrameGraph> build() const;

    /// @brief Backend Manager が所有する Resource を Graph の論理 Handle に結び付ける
    [[nodiscard]] Result<void> bind_resource(GraphResourceHandle a_graphResource,
                                             GpuResourceHandle a_resource);

    /// @brief 実行時の現在 Frame Slot に対応する Color Surface を結び付ける
    [[nodiscard]] Result<void> bind_surface_color(GraphResourceHandle a_graphResource);

    /// @brief Resize 後の現行 Depth Surface を実行時に結び付ける
    [[nodiscard]] Result<void> bind_surface_depth(GraphResourceHandle a_graphResource);

    /// @brief 全論理 Resource の物理所有者が指定済みか検証して返す
    [[nodiscard]] Result<std::vector<FrameGraphResourceBinding>> resource_bindings() const;

    /// @brief 最後に成功した実行の CPU 計測 Snapshot を返す
    [[nodiscard]] FrameGraphExecutionStats execution_stats_copy() const;

    /// @brief Backend が成功した Graph 実行の計測値を保存する
    void update_execution_stats(FrameGraphExecutionStats a_stats);

    /// @brief 構築順の Pass を実行が終わるまで借用する
    [[nodiscard]] const std::vector<std::unique_ptr<FrameGraphPass>>& passes() const noexcept;

private:
    /// @brief Graph 所有 Handle と重複 Binding を検証して一つ登録する
    [[nodiscard]] Result<void> bind(GraphResourceHandle a_graphResource,
                                    FrameGraphResourceBinding a_binding);

    FrameGraphBuilder m_builder;
    std::vector<std::unique_ptr<FrameGraphPass>> m_passes;
    std::vector<std::optional<FrameGraphResourceBinding>> m_bindings;
    mutable std::mutex m_statsMutex;
    FrameGraphExecutionStats m_stats;
    bool m_isFaulted = false;
};
} // namespace cue
