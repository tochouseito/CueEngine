#include "DX12ViewManager.h"

#include <algorithm>
#include <utility>

#include "DX12ResourcePool.h"

namespace cue::detail
{
/// @brief 表示資源のGPU使用が終わった後にDSV Slotを返す
DX12ViewManager::~DX12ViewManager()
{
    if (m_allocator && m_dsvSlot.generation != 0)
    {
        [[maybe_unused]] auto result = m_allocator->release(m_dsvSlot);
    }
}

/// @brief 必要な RTV Slot 数で Heap を作る
Result<std::unique_ptr<DX12ViewManager>> DX12ViewManager::create(DX12RenderDevice& a_device,
                                                                     UINT a_rtvCapacity)
{
    using ManagerResult = Result<std::unique_ptr<DX12ViewManager>>;
    if (a_rtvCapacity == 0)
    {
        return ManagerResult::failure({ErrorCategory::InvalidArgument, "DX12ViewManager.rtvCapacity"});
    }
    auto allocatorResult = DescriptorAllocator::create(a_device, a_rtvCapacity, 1, 1);
    if (!allocatorResult.has_value())
    {
        return ManagerResult::failure(*allocatorResult.try_error());
    }
    auto manager = std::make_unique<DX12ViewManager>();
    manager->m_ownedAllocator = allocatorResult.take_value();
    manager->m_allocator = manager->m_ownedAllocator.get();
    auto dsvSlotResult = manager->m_allocator->allocate(DescriptorHeapKind::DepthStencil);
    if (!dsvSlotResult.has_value())
    {
        return ManagerResult::failure(*dsvSlotResult.try_error());
    }
    manager->m_dsvSlot = dsvSlotResult.take_value();
    auto dsvHandleResult = manager->m_allocator->cpu_handle(manager->m_dsvSlot);
    if (!dsvHandleResult.has_value())
    {
        return ManagerResult::failure(*dsvHandleResult.try_error());
    }
    manager->m_dsvHandle = dsvHandleResult.take_value();
    return ManagerResult::success(std::move(manager));
}

/// @brief 表示用 View と GPU Resource View が同じ Descriptor Heap を使う
Result<std::unique_ptr<DX12ViewManager>> DX12ViewManager::create(DX12RenderDevice& a_device,
                                                                     DescriptorAllocator& a_allocator)
{
    using ManagerResult = Result<std::unique_ptr<DX12ViewManager>>;
    if (!a_device.device())
    {
        return ManagerResult::failure({ErrorCategory::InvalidArgument, "DX12ViewManager.create.device"});
    }
    auto manager = std::make_unique<DX12ViewManager>();
    manager->m_allocator = &a_allocator;
    auto dsvSlotResult = a_allocator.allocate(DescriptorHeapKind::DepthStencil);
    if (!dsvSlotResult.has_value())
    {
        return ManagerResult::failure(*dsvSlotResult.try_error());
    }
    manager->m_dsvSlot = dsvSlotResult.take_value();
    auto dsvHandleResult = a_allocator.cpu_handle(manager->m_dsvSlot);
    if (!dsvHandleResult.has_value())
    {
        return ManagerResult::failure(*dsvHandleResult.try_error());
    }
    manager->m_dsvHandle = dsvHandleResult.take_value();
    return ManagerResult::success(std::move(manager));
}

/// @brief Resource View と表示用 View の Descriptor 所有を一つにする
Result<std::unique_ptr<DX12ViewManager>> DX12ViewManager::create(DX12RenderDevice& a_device,
                                                                   DescriptorAllocator& a_allocator,
                                                                   DX12ResourcePool& a_resources)
{
    auto result = create(a_device, a_allocator);
    if (!result.has_value())
    {
        return result;
    }
    auto manager = result.take_value();
    manager->m_resources = &a_resources;
    return Result<std::unique_ptr<DX12ViewManager>>::success(std::move(manager));
}

/// @brief View の実所有と世代検証は Resource Pool が行う
Result<GpuViewHandle> DX12ViewManager::create_view(GpuResourceHandle a_resource, GpuViewDesc a_desc)
{
    if (!m_resources)
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidState, "DX12ViewManager.create_view"});
    }
    if (!a_desc.name.empty() && m_namedViews.contains(a_desc.name))
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                               "DX12ViewManager.create_view.name"});
    }
    const std::string name = a_desc.name;
    auto result = m_resources->create_view(a_resource, std::move(a_desc));
    if (result.has_value() && !name.empty())
    {
        m_namedViews.emplace(name, *result.try_value());
    }
    return result;
}

/// @brief 名前付き View の現行世代を返す
Result<GpuViewHandle> DX12ViewManager::get_view(std::string_view a_name) const
{
    const auto it = m_namedViews.find(std::string(a_name));
    if (it == m_namedViews.end())
    {
        return Result<GpuViewHandle>::failure({ErrorCategory::InvalidArgument,
                                               "DX12ViewManager.get_view"});
    }
    return Result<GpuViewHandle>::success(it->second);
}

/// @brief GPU 完了待機と Descriptor Slot 返却は Resource Pool が行う
Result<void> DX12ViewManager::destroy_view(GpuViewHandle a_view)
{
    if (!m_resources)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12ViewManager.destroy_view"});
    }
    auto result = m_resources->destroy_view(a_view);
    if (result.has_value())
    {
        const auto it = std::find_if(m_namedViews.begin(), m_namedViews.end(),
                                     [a_view](const auto& a_entry) {
                                         const auto& handle = a_entry.second;
                                         return handle.owner == a_view.owner && handle.kind == a_view.kind &&
                                                handle.index == a_view.index && handle.generation == a_view.generation;
                                     });
        if (it != m_namedViews.end())
        {
            m_namedViews.erase(it);
        }
    }
    return result;
}

/// @brief 未使用の RTV Slot を貸し出す
Result<RtvSlot> DX12ViewManager::allocate_rtv()
{
    auto result = m_allocator->allocate(DescriptorHeapKind::RenderTarget);
    if (!result.has_value())
    {
        return Result<RtvSlot>::failure(*result.try_error());
    }
    const auto slot = result.take_value();
    return Result<RtvSlot>::success({slot.index, slot.generation, slot.ownerId});
}

/// @brief 有効な Slot へ Resource の View を設定する
Result<void> DX12ViewManager::write_rtv(ID3D12Device* a_device, RtvSlot a_slot, ID3D12Resource* a_resource)
{
    if (!a_device || !a_resource)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12ViewManager.write_rtv"});
    }
    auto handleResult = cpu_handle(a_slot);
    if (!handleResult.has_value())
    {
        return Result<void>::failure(*handleResult.try_error());
    }
    a_device->CreateRenderTargetView(a_resource, nullptr, handleResult.take_value());
    return Result<void>::success();
}

/// @brief 有効な Slot の CPU Descriptor Handle を借用する
Result<D3D12_CPU_DESCRIPTOR_HANDLE> DX12ViewManager::cpu_handle(RtvSlot a_slot) const
{
    return m_allocator->cpu_handle({DescriptorHeapKind::RenderTarget, a_slot.index, a_slot.generation,
                                    a_slot.ownerId});
}

/// @brief GPU 完了後に Slot を再利用可能にする
Result<void> DX12ViewManager::release_rtv(RtvSlot a_slot)
{
    return m_allocator->release({DescriptorHeapKind::RenderTarget, a_slot.index, a_slot.generation,
                                 a_slot.ownerId});
}

/// @brief 単一 Depth Descriptor を新しい Surface へ結び直す
Result<void> DX12ViewManager::write_dsv(ID3D12Device* a_device, ID3D12Resource* a_resource)
{
    if (!a_device || !a_resource)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12ViewManager.write_dsv"});
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    a_device->CreateDepthStencilView(a_resource, &desc, dsv_handle());
    return Result<void>::success();
}

/// @brief DSV Heap が存続する間の Handle を返す
D3D12_CPU_DESCRIPTOR_HANDLE DX12ViewManager::dsv_handle() const noexcept
{
    return m_dsvHandle;
}
} // namespace cue::detail
