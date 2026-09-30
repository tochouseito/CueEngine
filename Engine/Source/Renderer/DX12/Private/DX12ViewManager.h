#pragma once

#include "DX12RenderDevice.h"
#include "DescriptorAllocator.h"

#include <Cue/Renderer/RHI/ViewManager.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cue::detail
{
class DX12ResourcePool;
/// @brief RTV Descriptor の割当を識別し、解放後の古い参照を拒否する
struct RtvSlot final
{
    UINT index = 0;
    std::uint64_t generation = 0;
    std::uint64_t ownerId = 0;
};

/// @brief RTV Heap と Slot の割当状態を一意所有する
///
/// Slot の書換えと解放は GPU が旧 View を使い終えた後だけ行う
class DX12ViewManager final : public IViewManager
{
public:
    /// @brief 使用前の Heap Owner を作る
    DX12ViewManager() = default;

    /// @brief GPU 完了後に表示用 DSV Slot を共通 Allocator へ返す
    ~DX12ViewManager();

    DX12ViewManager(const DX12ViewManager&) = delete;
    DX12ViewManager& operator=(const DX12ViewManager&) = delete;

    /// @brief 必要な RTV Slot 数で Heap を作る
    [[nodiscard]] static Result<std::unique_ptr<DX12ViewManager>> create(DX12RenderDevice& a_device,
                                                                            UINT a_rtvCapacity);

    /// @brief Backend 共通の DescriptorAllocator を借用して表示 View を管理する
    [[nodiscard]] static Result<std::unique_ptr<DX12ViewManager>> create(DX12RenderDevice& a_device,
                                                                            DescriptorAllocator& a_allocator);

    /// @brief Resource Pool の View 操作と表示用 View を同じ Descriptor Heap へ集める
    [[nodiscard]] static Result<std::unique_ptr<DX12ViewManager>> create(DX12RenderDevice& a_device,
                                                                           DescriptorAllocator& a_allocator,
                                                                           DX12ResourcePool& a_resources);

    /// @brief Resource Pool を通じて Resource View を作る
    [[nodiscard]] Result<GpuViewHandle> create_view(GpuResourceHandle a_resource, GpuViewDesc a_desc) override;

    /// @brief Manager が登録した名前から現行 View を探す
    [[nodiscard]] Result<GpuViewHandle> get_view(std::string_view a_name) const override;

    /// @brief Resource Pool を通じて Resource View を破棄する
    [[nodiscard]] Result<void> destroy_view(GpuViewHandle a_view) override;

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
    std::unique_ptr<DescriptorAllocator> m_ownedAllocator;
    DescriptorAllocator* m_allocator = nullptr;
    DX12ResourcePool* m_resources = nullptr;
    DescriptorSlot m_dsvSlot{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle{};
    std::unordered_map<std::string, GpuViewHandle> m_namedViews;
};
} // namespace cue::detail
