#pragma once

#include "D3D12DeviceContext.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace cue::detail
{
/// @brief RTV Descriptor の割当を識別し、解放後の古い参照を拒否する
struct RtvSlot final
{
    UINT index = 0;
    std::uint64_t generation = 0;
};

/// @brief RTV Heap と Slot の割当状態を一意所有する
///
/// Slot の書換えと解放は GPU が旧 View を使い終えた後だけ行う
class D3D12ViewManager final
{
public:
    /// @brief 使用前の Heap Owner を作る
    D3D12ViewManager() = default;

    D3D12ViewManager(const D3D12ViewManager&) = delete;
    D3D12ViewManager& operator=(const D3D12ViewManager&) = delete;

    /// @brief 必要な RTV Slot 数で Heap を作る
    [[nodiscard]] static Result<std::unique_ptr<D3D12ViewManager>> create(D3D12DeviceContext& a_device,
                                                                            UINT a_rtvCapacity);

    /// @brief 未使用の RTV Slot を貸し出す
    [[nodiscard]] Result<RtvSlot> allocate_rtv();

    /// @brief 有効な Slot へ Resource の View を設定する
    [[nodiscard]] Result<void> write_rtv(ID3D12Device* a_device, RtvSlot a_slot, ID3D12Resource* a_resource);

    /// @brief 有効な Slot の CPU Descriptor Handle を借用する
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> cpu_handle(RtvSlot a_slot) const;

    /// @brief GPU 完了後に Slot を再利用可能にする
    [[nodiscard]] Result<void> release_rtv(RtvSlot a_slot);

    /// @brief 永続 Depth Surface の DSV を更新する。呼出前に旧 GPU 使用を完了させる
    [[nodiscard]] Result<void> write_dsv(ID3D12Device* a_device, ID3D12Resource* a_resource);

    /// @brief Depth Surface が存続する間の CPU Descriptor を借用する
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle() const noexcept;

private:
    struct SlotState final
    {
        std::uint64_t generation = 0;
        bool isAllocated = false;
    };

    /// @brief 世代と割当状態で失効した Slot を識別する
    [[nodiscard]] bool is_valid(RtvSlot a_slot) const noexcept;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    std::vector<SlotState> m_slots;
    UINT m_rtvStride = 0;
};
} // namespace cue::detail
