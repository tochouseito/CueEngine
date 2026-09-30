#pragma once

#include "DX12CommandRecorder.h"

#include <Cue/Renderer/FrameGraph/FrameGraphPass.h>

namespace cue::detail
{
/// @brief Offscreen Color と Depth の初期状態を作る Pass
class DX12ClearPass final : public FrameGraphPass
{
public:
    /// @brief Frame 中だけ有効な View と論理 Resource を借用する
    DX12ClearPass(GraphResourceHandle a_color, GraphResourceHandle a_depth,
                  GpuViewHandle a_colorView, GpuViewHandle a_depthView) noexcept;

    /// @brief FrameGraph 上の Pass 名を返す
    [[nodiscard]] const char* name() const noexcept override;

    /// @brief RTV／DSV への書込を宣言する
    [[nodiscard]] Result<GraphPassHandle> setup(FrameGraphBuilder& a_builder) const override;

    /// @brief Barrier 適用後に出力先と消去値を記録する
    [[nodiscard]] Result<void> execute(FrameGraphContext& a_context) const override;

private:
    GraphResourceHandle m_color;
    GraphResourceHandle m_depth;
    GpuViewHandle m_colorView;
    GpuViewHandle m_depthView;
};

/// @brief Offscreen Color を Back Buffer へコピーする Pass
class DX12CopyToBackBufferPass final : public FrameGraphPass
{
public:
    /// @brief Back Buffer と Source は Pass 実行まで生存させる
    DX12CopyToBackBufferPass(GraphResourceHandle a_source, GraphResourceHandle a_backBuffer,
                             GpuResourceHandle a_sourceResource, ID3D12Resource* a_backBufferResource) noexcept;

    /// @brief FrameGraph 上の Pass 名を返す
    [[nodiscard]] const char* name() const noexcept override;

    /// @brief Copy Source／Destination の状態を宣言する
    [[nodiscard]] Result<GraphPassHandle> setup(FrameGraphBuilder& a_builder) const override;

    /// @brief DX12 Back Buffer のコピーを記録する
    [[nodiscard]] Result<void> execute(FrameGraphContext& a_context) const override;

private:
    GraphResourceHandle m_source;
    GraphResourceHandle m_backBuffer;
    GpuResourceHandle m_sourceResource;
    ID3D12Resource* m_backBufferResource = nullptr;
};
} // namespace cue::detail
