#pragma once

#include "D3D12DeviceContext.h"
#include "D3D12QueueContext.h"

#include <cstdint>
#include <memory>

namespace cue::detail
{
/// @brief Static Mesh の世代付き非所有識別子
struct StaticMeshHandle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
};

/// @brief 固定 Triangle の GPU Buffer を所有し、失効した Handle を拒否する
class D3D12StaticMeshPool final
{
public:
    /// @brief Upload 完了を Fence で確認してから Default Buffer を公開する
    [[nodiscard]] static Result<std::unique_ptr<D3D12StaticMeshPool>> create(D3D12DeviceContext& a_device,
                                                                              D3D12QueueContext& a_queue);

    /// @brief 現在登録されている Triangle を借用する Handle を返す
    [[nodiscard]] StaticMeshHandle triangle() const noexcept;

    /// @brief Render Pass 内で VB／IB を設定して固定 Mesh を描画する
    [[nodiscard]] Result<void> draw(ID3D12GraphicsCommandList* a_list, StaticMeshHandle a_handle) const;

    /// @brief GPU 完了後に Buffer と Handle を失効させる
    [[nodiscard]] Result<void> destroy(D3D12QueueContext& a_queue, StaticMeshHandle a_handle);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_geometry;
    std::uint64_t m_generation = 1;
    bool m_isAllocated = false;
};
} // namespace cue::detail
