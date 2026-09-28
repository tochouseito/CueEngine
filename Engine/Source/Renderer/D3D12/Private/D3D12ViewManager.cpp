#include "D3D12ViewManager.h"

#include <utility>

namespace cue::detail
{
/// @brief 必要な RTV Slot 数で Heap を作る
Result<std::unique_ptr<D3D12ViewManager>> D3D12ViewManager::create(D3D12DeviceContext& a_device,
                                                                     UINT a_rtvCapacity)
{
    using ManagerResult = Result<std::unique_ptr<D3D12ViewManager>>;
    if (a_rtvCapacity == 0)
    {
        return ManagerResult::failure({ErrorCategory::InvalidArgument, "D3D12ViewManager.rtvCapacity"});
    }
    auto manager = std::make_unique<D3D12ViewManager>();
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = a_rtvCapacity;
    const HRESULT result = a_device.device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&manager->m_rtvHeap));
    if (FAILED(result))
    {
        return ManagerResult::failure(gpu_error("ID3D12Device.CreateDescriptorHeap.RTV", result));
    }
    const HRESULT rtvNameResult = manager->m_rtvHeap->SetName(L"CueEngine RTV Heap");
    if (FAILED(rtvNameResult))
    {
        return ManagerResult::failure(gpu_error("ID3D12DescriptorHeap.SetName.RTV", rtvNameResult));
    }
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heapDesc.NumDescriptors = 1;
    const HRESULT depthResult = a_device.device()->CreateDescriptorHeap(&heapDesc,
                                                                          IID_PPV_ARGS(&manager->m_dsvHeap));
    if (FAILED(depthResult))
    {
        return ManagerResult::failure(gpu_error("ID3D12Device.CreateDescriptorHeap.DSV", depthResult));
    }
    const HRESULT dsvNameResult = manager->m_dsvHeap->SetName(L"CueEngine DSV Heap");
    if (FAILED(dsvNameResult))
    {
        return ManagerResult::failure(gpu_error("ID3D12DescriptorHeap.SetName.DSV", dsvNameResult));
    }
    manager->m_rtvStride = a_device.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    manager->m_slots.resize(a_rtvCapacity);
    return ManagerResult::success(std::move(manager));
}

/// @brief 未使用の RTV Slot を貸し出す
Result<RtvSlot> D3D12ViewManager::allocate_rtv()
{
    for (UINT index = 0; index < m_slots.size(); ++index)
    {
        auto& slot = m_slots[index];
        if (slot.isAllocated)
        {
            continue;
        }
        slot.isAllocated = true;
        ++slot.generation;
        if (slot.generation == 0)
        {
            ++slot.generation;
        }
        return Result<RtvSlot>::success({index, slot.generation});
    }
    return Result<RtvSlot>::failure({ErrorCategory::InvalidState, "D3D12ViewManager.rtvCapacity"});
}

/// @brief 有効な Slot へ Resource の View を設定する
Result<void> D3D12ViewManager::write_rtv(ID3D12Device* a_device, RtvSlot a_slot, ID3D12Resource* a_resource)
{
    if (!is_valid(a_slot) || !a_device || !a_resource)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12ViewManager.write_rtv"});
    }
    auto handleResult = cpu_handle(a_slot);
    a_device->CreateRenderTargetView(a_resource, nullptr, handleResult.take_value());
    return Result<void>::success();
}

/// @brief 有効な Slot の CPU Descriptor Handle を借用する
Result<D3D12_CPU_DESCRIPTOR_HANDLE> D3D12ViewManager::cpu_handle(RtvSlot a_slot) const
{
    if (!is_valid(a_slot))
    {
        return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::failure({ErrorCategory::InvalidState,
                                                               "D3D12ViewManager.cpu_handle"});
    }
    auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(a_slot.index) * m_rtvStride;
    return Result<D3D12_CPU_DESCRIPTOR_HANDLE>::success(handle);
}

/// @brief GPU 完了後に Slot を再利用可能にする
Result<void> D3D12ViewManager::release_rtv(RtvSlot a_slot)
{
    if (!is_valid(a_slot))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12ViewManager.release_rtv"});
    }
    m_slots[a_slot.index].isAllocated = false;
    return Result<void>::success();
}

/// @brief 単一 Depth Descriptor を新しい Surface へ結び直す
Result<void> D3D12ViewManager::write_dsv(ID3D12Device* a_device, ID3D12Resource* a_resource)
{
    if (!a_device || !a_resource)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12ViewManager.write_dsv"});
    }
    a_device->CreateDepthStencilView(a_resource, nullptr, dsv_handle());
    return Result<void>::success();
}

/// @brief DSV Heap が存続する間の Handle を返す
D3D12_CPU_DESCRIPTOR_HANDLE D3D12ViewManager::dsv_handle() const noexcept
{
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

/// @brief 世代と割当状態で失効した Slot を識別する
bool D3D12ViewManager::is_valid(RtvSlot a_slot) const noexcept
{
    return a_slot.index < m_slots.size() && m_slots[a_slot.index].isAllocated &&
           m_slots[a_slot.index].generation == a_slot.generation;
}
} // namespace cue::detail
