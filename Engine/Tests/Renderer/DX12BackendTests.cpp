#include <array>
#include <memory>

#include <DX12/DX12Backend.h>
#include <DX12/DX12DescriptorAllocator.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12RenderDevice.h>
#include <RHI/BackendFactory.h>

/// @brief 共通 Factory から DX12 Device を所有し、停止で借用を失効させる
int main()
{
    auto backendResult = cue::create_backend();
    if (!backendResult.has_value())
    {
        return 1;
    }
    std::unique_ptr<cue::IBackend> backend = backendResult.take_value();
    auto* device = dynamic_cast<cue::dx12::DX12RenderDevice*>(backend->get_render_device());
    if (!device || !device->device() || !device->factory() || !device->adapter() ||
        !backend->get_queue_pool() || !backend->get_command_pool() || !backend->get_resource_pool())
    {
        return 2;
    }
    if (device->is_software_adapter() != device->is_warp())
    {
        return 3;
    }

    // Factory 経由の Backend が六種類の Heap を生成し、停止まで所有する
    auto* dx12Backend = dynamic_cast<cue::dx12::DX12Backend*>(backend.get());
    if (!dx12Backend)
    {
        return 13;
    }
    constexpr std::array roles = {
        cue::dx12::DX12DescriptorHeapRole::CpuView,
        cue::dx12::DX12DescriptorHeapRole::ShaderView,
        cue::dx12::DX12DescriptorHeapRole::CpuSampler,
        cue::dx12::DX12DescriptorHeapRole::ShaderSampler,
        cue::dx12::DX12DescriptorHeapRole::Rtv,
        cue::dx12::DX12DescriptorHeapRole::Dsv,
    };
    for (const auto role : roles)
    {
        auto* allocator = dx12Backend->get_descriptor_allocator(role);
        if (!allocator || !allocator->heap())
        {
            return 14;
        }
    }
    if (dx12Backend->get_descriptor_allocator(static_cast<cue::dx12::DX12DescriptorHeapRole>(255)) != nullptr ||
        dx12Backend->get_descriptor_allocator(cue::dx12::DX12DescriptorHeapRole::CpuView)->is_shader_visible() ||
        !dx12Backend->get_descriptor_allocator(cue::dx12::DX12DescriptorHeapRole::ShaderView)->is_shader_visible() ||
        dx12Backend->get_descriptor_allocator(cue::dx12::DX12DescriptorHeapRole::Rtv)->type() !=
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
    {
        return 15;
    }

    // Backend が所有する ResourcePool から生成し、停止前に破棄予約できる
    auto resourceResult = backend->get_resource_pool()->create_buffer({64, cue::GpuMemoryUsage::Upload});
    if (!resourceResult.has_value() ||
        !backend->get_resource_pool()->retire(resourceResult.take_value()).has_value())
    {
        return 25;
    }

    // 停止後に借用 Pointer を再取得できず、二重停止も安全に完了する
    if (!backend->shutdown().has_value() || backend->get_render_device() != nullptr ||
        backend->get_queue_pool() != nullptr || backend->get_command_pool() != nullptr ||
        backend->get_resource_pool() != nullptr ||
        !backend->shutdown().has_value())
    {
        return 4;
    }
    for (const auto role : roles)
    {
        if (dx12Backend->get_descriptor_allocator(role) != nullptr)
        {
            return 16;
        }
    }

    // 容量設定は生成前に検証し、指定した容量の Heap が Backend に反映される
    cue::dx12::DX12DescriptorHeapConfig invalidConfig{};
    invalidConfig.rtvCapacity = 0;
    if (cue::dx12::DX12Backend::create(invalidConfig).has_value())
    {
        return 17;
    }
    cue::dx12::DX12DescriptorHeapConfig smallConfig{};
    smallConfig.rtvCapacity = 1;
    auto configuredResult = cue::dx12::DX12Backend::create(smallConfig);
    if (!configuredResult.has_value())
    {
        return 18;
    }
    auto configured = configuredResult.take_value();
    auto* configuredRtv = configured->get_descriptor_allocator(cue::dx12::DX12DescriptorHeapRole::Rtv);
    if (!configuredRtv || configuredRtv->heap()->GetDesc().NumDescriptors != 1)
    {
        return 19;
    }
    auto configuredSlotResult = configuredRtv->allocate();
    if (!configuredSlotResult.has_value() || configuredRtv->allocate().has_value() ||
        !configuredRtv->release(configuredSlotResult.take_value()).has_value() ||
        !configured->shutdown().has_value() ||
        configured->get_descriptor_allocator(cue::dx12::DX12DescriptorHeapRole::Rtv) != nullptr)
    {
        return 20;
    }

    // Backend 停止後も貸出済み Queue を使用でき、最後の返却で安全に破棄する
    auto leasedBackendResult = cue::create_backend();
    if (!leasedBackendResult.has_value())
    {
        return 5;
    }
    auto leasedBackend = leasedBackendResult.take_value();
    auto* leasedDx12 = dynamic_cast<cue::dx12::DX12Backend*>(leasedBackend.get());
    auto* leasedDescriptors = leasedDx12
                                  ? leasedDx12->get_descriptor_allocator(cue::dx12::DX12DescriptorHeapRole::ShaderView)
                                  : nullptr;
    if (!leasedDescriptors)
    {
        return 21;
    }
    auto leaseResult = leasedBackend->get_queue_pool()->acquire(cue::QueueType::Compute);
    if (!leaseResult.has_value())
    {
        return 6;
    }
    auto lease = leaseResult.take_value();
    if (leasedBackend->shutdown().has_value() || leasedBackend->get_render_device() != nullptr ||
        leasedBackend->get_queue_pool() != nullptr || leasedBackend->get_command_pool() != nullptr)
    {
        return 7;
    }
    // Backend の参照を外しても Queue Lease が Shader 可視 Heap を保持する
    if (leasedDx12->get_descriptor_allocator(cue::dx12::DX12DescriptorHeapRole::ShaderView) != nullptr ||
        !leasedDescriptors->heap() ||
        leasedDescriptors->heap()->GetDesc().Flags != D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
    {
        return 22;
    }
    auto fenceResult = lease->signal();
    if (!fenceResult.has_value() || !lease->wait_for_fence(fenceResult.take_value()).has_value())
    {
        return 8;
    }
    lease.reset();

    // 借用中 Command を残した停止でも、Backend より長い Context の寿命を維持する
    auto commandBackendResult = cue::create_backend();
    if (!commandBackendResult.has_value())
    {
        return 9;
    }
    auto commandBackend = commandBackendResult.take_value();
    auto* commandDx12 = dynamic_cast<cue::dx12::DX12Backend*>(commandBackend.get());
    auto* commandDescriptors = commandDx12
                                   ? commandDx12->get_descriptor_allocator(cue::dx12::DX12DescriptorHeapRole::Rtv)
                                   : nullptr;
    if (!commandDescriptors)
    {
        return 23;
    }
    auto commandResult = commandBackend->get_command_pool()->acquire(cue::QueueType::Copy);
    if (!commandResult.has_value())
    {
        return 10;
    }
    auto command = commandResult.take_value();
    if (commandBackend->shutdown().has_value() || commandBackend->get_command_pool() != nullptr ||
        commandBackend->get_queue_pool() != nullptr || commandBackend->get_render_device() != nullptr)
    {
        return 11;
    }
    // Command Lease が残る間も記録に必要な Descriptor Heap の寿命を維持する
    if (commandDx12->get_descriptor_allocator(cue::dx12::DX12DescriptorHeapRole::Rtv) != nullptr ||
        !commandDescriptors->heap() ||
        commandDescriptors->heap()->GetDesc().Type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
    {
        return 24;
    }
    if (command->state() != cue::CommandState::Recording || !command->close().has_value())
    {
        return 12;
    }
    command.reset();

    // Backend 停止後も貸出中 Resource の Native 実体を Lease が維持する
    auto resourceBackendResult = cue::create_backend();
    if (!resourceBackendResult.has_value())
    {
        return 26;
    }
    auto resourceBackend = resourceBackendResult.take_value();
    auto resourceHandleResult = resourceBackend->get_resource_pool()->create_buffer({64, cue::GpuMemoryUsage::Upload});
    if (!resourceHandleResult.has_value())
    {
        return 27;
    }
    auto resourceLeaseResult = resourceBackend->get_resource_pool()->acquire(resourceHandleResult.take_value(),
                                                                             cue::GpuResourceAccess::Read);
    if (!resourceLeaseResult.has_value())
    {
        return 28;
    }
    auto resourceLease = resourceLeaseResult.take_value();
    if (resourceBackend->shutdown().has_value() || resourceBackend->get_resource_pool() != nullptr)
    {
        return 29;
    }
    resourceBackend.reset();
    auto* survivingResource = dynamic_cast<cue::dx12::DX12GpuResource*>(resourceLease->resource());
    if (!survivingResource || !survivingResource->resource() || survivingResource->buffer_size() != 64)
    {
        return 30;
    }
    resourceLease.reset();
    return 0;
}
