#include "D3D12CommandPool.h"
#include "D3D12QueueContext.h"
#include "D3D12QueuePool.h"
#include "D3D12ViewManager.h"

#include <cstdint>
#include <cstring>

/// @brief RTV Slot の失効と Command Context の貸出・GPU 完了条件を利用可能な Adapter で確認する
int main()
{
    auto deviceResult = cue::detail::D3D12DeviceContext::create();
    if (!deviceResult.has_value())
    {
        return 1;
    }
    auto device = deviceResult.take_value();
    auto queueResult = cue::detail::D3D12QueueContext::create(*device);
    if (!queueResult.has_value())
    {
        return 15;
    }
    auto queue = queueResult.take_value();

    auto viewsResult = cue::detail::D3D12ViewManager::create(*device, 2);
    if (!viewsResult.has_value())
    {
        return 2;
    }
    auto views = viewsResult.take_value();
    auto firstView = views->allocate_rtv();
    auto secondView = views->allocate_rtv();
    auto exhausted = views->allocate_rtv();
    if (!firstView.has_value() || !secondView.has_value() || exhausted.has_value())
    {
        return 3;
    }
    const cue::detail::RtvSlot oldSlot = firstView.take_value();
    if (!views->cpu_handle(oldSlot).has_value() || !views->release_rtv(oldSlot).has_value())
    {
        return 4;
    }
    if (views->cpu_handle(oldSlot).has_value() || views->release_rtv(oldSlot).has_value())
    {
        return 5;
    }
    auto replacement = views->allocate_rtv();
    if (!replacement.has_value() || replacement.try_value()->index != oldSlot.index ||
        replacement.try_value()->generation == oldSlot.generation)
    {
        return 6;
    }

    auto poolResult = cue::detail::D3D12CommandPool::create(*device);
    if (!poolResult.has_value())
    {
        return 7;
    }
    auto pool = poolResult.take_value();
    auto firstLease = pool->acquire(*queue, 0);
    if (!firstLease.has_value() || pool->acquire(*queue, 0).has_value())
    {
        return 8;
    }
    const cue::detail::CommandLease lease = firstLease.take_value();
    if (!pool->submit(*queue, lease).has_value() || pool->submit(*queue, lease).has_value() ||
        !pool->retire(*queue, lease).has_value())
    {
        return 9;
    }

    // 同じ Slot の再貸出は記録した Fence の完了後だけ許す
    auto reusedLease = pool->acquire(*queue, 0);
    if (!reusedLease.has_value())
    {
        return 10;
    }
    const cue::detail::CommandLease reused = reusedLease.take_value();
    if (pool->submit(*queue, lease).has_value() || !pool->submit(*queue, reused).has_value() ||
        !pool->retire(*queue, reused).has_value())
    {
        return 10;
    }
    if (!queue->wait_idle().has_value())
    {
        return 11;
    }
    pool->release_for_resize();
    if (!pool->recreate_lists(*device).has_value())
    {
        return 12;
    }
    auto otherLease = pool->acquire(*queue, 1);
    if (!otherLease.has_value())
    {
        return 13;
    }
    const cue::detail::CommandLease other = otherLease.take_value();
    if (!pool->submit(*queue, other).has_value() || !pool->retire(*queue, other).has_value() ||
        !queue->wait_idle().has_value())
    {
        return 14;
    }
    pool->mark_idle();

    // Copy の実転送と Copy → Compute → Graphics の GPU 待機を確認する
    auto queuesResult = cue::detail::D3D12QueuePool::create(*device);
    if (!queuesResult.has_value())
    {
        return 16;
    }
    auto queues = queuesResult.take_value();
    cue::IGpuExecution& execution = *queues;
    if (execution.wait_cpu({cue::GpuQueueType::Copy, 1}).has_value())
    {
        return 17;
    }
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(std::uint32_t);
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
    if (FAILED(device->device()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer))) ||
        FAILED(device->device()->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuffer))))
    {
        return 18;
    }
    constexpr std::uint32_t k_expected = 0x12345678;
    void* mapped = nullptr;
    if (FAILED(uploadBuffer->Map(0, nullptr, &mapped)))
    {
        return 19;
    }
    std::memcpy(mapped, &k_expected, sizeof(k_expected));
    uploadBuffer->Unmap(0, nullptr);
    auto copyPoolResult = cue::detail::D3D12CommandPool::create(*device, cue::GpuQueueType::Copy);
    auto computePoolResult = cue::detail::D3D12CommandPool::create(*device, cue::GpuQueueType::Compute);
    if (!copyPoolResult.has_value() || !computePoolResult.has_value())
    {
        return 20;
    }
    auto copyPool = copyPoolResult.take_value();
    auto computePool = computePoolResult.take_value();
    auto& copyQueue = queues->context(cue::GpuQueueType::Copy);
    auto& computeQueue = queues->context(cue::GpuQueueType::Compute);
    auto copyLeaseResult = copyPool->acquire(copyQueue, 0);
    if (!copyLeaseResult.has_value() || computePool->acquire(copyQueue, 0).has_value())
    {
        return 21;
    }
    const auto copyLease = copyLeaseResult.take_value();
    copyLease.list->CopyBufferRegion(readbackBuffer.Get(), 0, uploadBuffer.Get(), 0, sizeof(k_expected));
    if (!copyPool->submit(copyQueue, copyLease).has_value() ||
        !copyPool->retire(copyQueue, copyLease).has_value())
    {
        return 22;
    }
    auto copied = execution.signal(cue::GpuQueueType::Copy);
    if (!copied.has_value() ||
        !execution.wait_gpu(cue::GpuQueueType::Compute, copied.take_value()).has_value())
    {
        return 23;
    }
    auto computeLeaseResult = computePool->acquire(computeQueue, 0);
    if (!computeLeaseResult.has_value())
    {
        return 24;
    }
    const auto computeLease = computeLeaseResult.take_value();
    if (!computePool->submit(computeQueue, computeLease).has_value() ||
        !computePool->retire(computeQueue, computeLease).has_value())
    {
        return 25;
    }
    auto computed = execution.signal(cue::GpuQueueType::Compute);
    if (!computed.has_value() ||
        !execution.wait_gpu(cue::GpuQueueType::Graphics, computed.take_value()).has_value())
    {
        return 26;
    }
    auto graphics = execution.signal(cue::GpuQueueType::Graphics);
    if (!graphics.has_value() || !execution.wait_cpu(graphics.take_value()).has_value())
    {
        return 27;
    }
    D3D12_RANGE readRange{0, sizeof(k_expected)};
    if (FAILED(readbackBuffer->Map(0, &readRange, &mapped)))
    {
        return 28;
    }
    std::uint32_t actual = 0;
    std::memcpy(&actual, mapped, sizeof(actual));
    readbackBuffer->Unmap(0, nullptr);
    if (actual != k_expected || !execution.wait_idle().has_value())
    {
        return 29;
    }
    return 0;
}
