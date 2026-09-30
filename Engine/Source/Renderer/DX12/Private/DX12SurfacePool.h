#pragma once

#include "DX12SwapChain.h"
#include "DX12ResourcePool.h"

#include <array>
#include <memory>
#include <vector>

namespace cue::detail
{
/// @brief Frame Slot ごとの一時 Color Surface と Resize 間で永続する Depth Surface を所有する
///
/// Surface の交換と破棄は全 Queue の完了後だけ許可する。SRV は現在の Copy Pass で不要
class DX12SurfacePool final
{
public:
    /// @brief View Manager を借用し、全 Surface より長く存続させる
    DX12SurfacePool(DX12ViewManager& a_views, DX12ResourcePool& a_resources) noexcept;

    /// @brief View Manager の Slot を借り、初回 Surface を生成する
    [[nodiscard]] static Result<std::unique_ptr<DX12SurfacePool>> create(DX12RenderDevice& a_device,
                                                                            DX12ViewManager& a_views,
                                                                            DX12ResourcePool& a_resources,
                                                                            WindowSize a_size);

    /// @brief GPU 完了後に RTV Slot を返す
    ~DX12SurfacePool();

    DX12SurfacePool(const DX12SurfacePool&) = delete;
    DX12SurfacePool& operator=(const DX12SurfacePool&) = delete;

    /// @brief GPU 完了後に Surface を置換し、旧 Resource を解放する
    [[nodiscard]] Result<void> resize(DX12RenderDevice& a_device, WindowSize a_size);

    /// @brief 対応 Frame の Fence が完了した後だけ借用する
    [[nodiscard]] ID3D12Resource* color(UINT a_slot) const noexcept;

    /// @brief 次の Resize まで借用する
    [[nodiscard]] ID3D12Resource* depth() const noexcept;

    /// @brief 対応 Frame の RTV を借用する
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> color_rtv(UINT a_slot) const;

    /// @brief 現行 Depth の DSV を借用する
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE depth_dsv() const noexcept;

    /// @brief 共通 Command Recorder に渡す Color View を借用する
    [[nodiscard]] GpuViewHandle color_view(UINT a_slot) const noexcept;

    /// @brief 共通 Command Recorder に渡す Depth View を借用する
    [[nodiscard]] GpuViewHandle depth_view() const noexcept;

    /// @brief Presentation への Copy 元となる Color Resource Handle を借用する
    [[nodiscard]] GpuResourceHandle color_resource(UINT a_slot) const noexcept;

private:
    struct RetiredSurface final
    {
        std::array<GpuResourceHandle, k_backBufferCount> color;
        GpuResourceHandle depth;
        std::array<GpuViewHandle, k_backBufferCount> colorViews;
        GpuViewHandle depthView;
    };

    /// @brief 旧 Surface の回収失敗時も Handle を維持して次回 Resize で再試行する
    [[nodiscard]] Result<void> collect_retired();

    DX12ViewManager& m_views;
    DX12ResourcePool& m_resources;
    std::array<GpuResourceHandle, k_backBufferCount> m_color;
    GpuResourceHandle m_depth;
    std::array<GpuViewHandle, k_backBufferCount> m_colorViews;
    GpuViewHandle m_depthView;
    std::array<RtvSlot, k_backBufferCount> m_colorSlots{};
    std::vector<RetiredSurface> m_retired;
};
} // namespace cue::detail
