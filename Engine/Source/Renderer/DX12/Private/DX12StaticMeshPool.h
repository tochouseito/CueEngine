#pragma once

#include "DX12RenderDevice.h"
#include "DX12GpuCommandQueue.h"
#include "DX12ResourcePool.h"

#include <Cue/Renderer/RHI/GpuCommands.h>

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
class DX12StaticMeshPool final
{
public:
    /// @brief Upload 完了を Fence で確認してから Default Buffer を公開する
    [[nodiscard]] static Result<std::unique_ptr<DX12StaticMeshPool>> create(DX12RenderDevice& a_device,
                                                                              DX12ResourcePool& a_resources);

    /// @brief GPU 完了後に幾何 Buffer を Resource Pool へ返す
    ~DX12StaticMeshPool();

    /// @brief 現在登録されている Triangle を借用する Handle を返す
    [[nodiscard]] StaticMeshHandle triangle() const noexcept;

    /// @brief Render Pass 内で VB／IB を設定して固定 Mesh を描画する
    [[nodiscard]] Result<void> draw(IGpuCommandRecorder& a_commands, StaticMeshHandle a_handle) const;

    /// @brief GPU 完了後に Buffer と Handle を失効させる
    [[nodiscard]] Result<void> destroy(StaticMeshHandle a_handle);

private:
    DX12ResourcePool* m_resources = nullptr;
    GpuResourceHandle m_geometry;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_upload;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_uploadAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_uploadList;
    std::uint64_t m_generation = 1;
    bool m_isAllocated = false;
};
} // namespace cue::detail
