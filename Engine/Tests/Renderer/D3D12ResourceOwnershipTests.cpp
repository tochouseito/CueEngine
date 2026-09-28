#include "D3D12CommandPool.h"
#include "D3D12QueueContext.h"
#include "D3D12ViewManager.h"

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
    return 0;
}
