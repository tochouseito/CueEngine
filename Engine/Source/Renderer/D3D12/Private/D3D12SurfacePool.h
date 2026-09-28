#pragma once

#include "D3D12Presentation.h"
#include "D3D12ResourcePool.h"

#include <array>
#include <memory>

namespace cue::detail
{
/// @brief Frame Slot ごとの一時 Color Surface と Resize 間で永続する Depth Surface を所有する
///
/// Surface の交換と破棄は Direct Queue の完了後だけ許可する。SRV は現在の Copy Pass で不要
class D3D12SurfacePool final
{
public:
    /// @brief View Manager を借用し、全 Surface より長く存続させる
    D3D12SurfacePool(D3D12ViewManager& a_views, D3D12ResourcePool& a_resources) noexcept;

    /// @brief View Manager の Slot を借り、初回 Surface を生成する
    [[nodiscard]] static Result<std::unique_ptr<D3D12SurfacePool>> create(D3D12DeviceContext& a_device,
                                                                            D3D12ViewManager& a_views,
                                                                            D3D12ResourcePool& a_resources,
                                                                            WindowSize a_size);

    /// @brief GPU 完了後に RTV Slot を返す
    ~D3D12SurfacePool();

    D3D12SurfacePool(const D3D12SurfacePool&) = delete;
    D3D12SurfacePool& operator=(const D3D12SurfacePool&) = delete;

    /// @brief GPU 完了後に Surface を置換し、旧 Resource を解放する
    [[nodiscard]] Result<void> resize(D3D12DeviceContext& a_device, WindowSize a_size);

    /// @brief 対応 Frame の Fence が完了した後だけ借用する
    [[nodiscard]] ID3D12Resource* color(UINT a_slot) const noexcept;

    /// @brief 次の Resize まで借用する
    [[nodiscard]] ID3D12Resource* depth() const noexcept;

    /// @brief 対応 Frame の RTV を借用する
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> color_rtv(UINT a_slot) const;

    /// @brief 現行 Depth の DSV を借用する
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE depth_dsv() const noexcept;

private:
    D3D12ViewManager& m_views;
    D3D12ResourcePool& m_resources;
    std::array<GpuResourceHandle, k_backBufferCount> m_color;
    GpuResourceHandle m_depth;
    std::array<RtvSlot, k_backBufferCount> m_colorSlots{};
};
} // namespace cue::detail
