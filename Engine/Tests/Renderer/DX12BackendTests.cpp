#include <memory>

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
    if (!device || !device->device() || !device->factory() || !device->adapter() || !backend->get_queue_pool())
    {
        return 2;
    }
    if (device->is_software_adapter() != device->is_warp())
    {
        return 3;
    }

    // 停止後に借用 Pointer を再取得できず、二重停止も安全に完了する
    if (!backend->shutdown().has_value() || backend->get_render_device() != nullptr ||
        backend->get_queue_pool() != nullptr ||
        !backend->shutdown().has_value())
    {
        return 4;
    }

    // Backend 停止後も貸出済み Queue を使用でき、最後の返却で安全に破棄する
    auto leasedBackendResult = cue::create_backend();
    if (!leasedBackendResult.has_value())
    {
        return 5;
    }
    auto leasedBackend = leasedBackendResult.take_value();
    auto leaseResult = leasedBackend->get_queue_pool()->acquire(cue::QueueType::Graphics);
    if (!leaseResult.has_value())
    {
        return 6;
    }
    auto lease = leaseResult.take_value();
    if (leasedBackend->shutdown().has_value() || leasedBackend->get_render_device() != nullptr ||
        leasedBackend->get_queue_pool() != nullptr)
    {
        return 7;
    }
    auto fenceResult = lease->signal();
    if (!fenceResult.has_value() || !lease->wait_for_fence(fenceResult.take_value()).has_value())
    {
        return 8;
    }
    lease.reset();
    return 0;
}
