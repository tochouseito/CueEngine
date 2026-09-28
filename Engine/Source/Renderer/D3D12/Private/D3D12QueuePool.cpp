#include "D3D12QueuePool.h"

#include <utility>

namespace cue::detail
{
namespace
{
constexpr std::array<GpuQueueType, 3> k_queueTypes = {
    GpuQueueType::Graphics, GpuQueueType::Compute, GpuQueueType::Copy};
} // namespace

/// @brief Device が生存する間に三種類の Queue を生成する
Result<std::unique_ptr<D3D12QueuePool>> D3D12QueuePool::create(D3D12DeviceContext& a_device)
{
    using PoolResult = Result<std::unique_ptr<D3D12QueuePool>>;
    auto pool = std::make_unique<D3D12QueuePool>();
    for (const auto type : k_queueTypes)
    {
        auto queueResult = D3D12QueueContext::create(a_device, type);
        if (!queueResult.has_value())
        {
            return PoolResult::failure(*queueResult.try_error());
        }
        pool->m_contexts[static_cast<std::size_t>(type)] = queueResult.take_value();
    }
    return PoolResult::success(std::move(pool));
}

/// @brief Pool 内の Queue を借用する。呼出側は有効な GpuQueueType を渡す
D3D12QueueContext& D3D12QueuePool::context(GpuQueueType a_type) noexcept
{
    return *m_contexts[static_cast<std::size_t>(a_type)];
}

/// @brief 発行した Fence の Queue 種別と値を一体で返す
Result<GpuFencePoint> D3D12QueuePool::signal(GpuQueueType a_queue)
{
    if (!is_valid(a_queue))
    {
        return Result<GpuFencePoint>::failure({ErrorCategory::InvalidArgument, "D3D12QueuePool.signal"});
    }
    auto valueResult = context(a_queue).signal();
    if (!valueResult.has_value())
    {
        return Result<GpuFencePoint>::failure(*valueResult.try_error());
    }
    return Result<GpuFencePoint>::success({a_queue, valueResult.take_value(), this});
}

/// @brief Fence 発行元の Event で CPU 完了を待つ
Result<void> D3D12QueuePool::wait_cpu(GpuFencePoint a_point)
{
    if (!is_valid(a_point.queue) || a_point.owner != this)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12QueuePool.wait_cpu"});
    }
    return context(a_point.queue).wait_for(a_point.value);
}

/// @brief 発行元 Fence を指定 Queue の GPU 実行順に追加する
Result<void> D3D12QueuePool::wait_gpu(GpuQueueType a_waitingQueue, GpuFencePoint a_point)
{
    if (!is_valid(a_waitingQueue) || !is_valid(a_point.queue) || a_waitingQueue == a_point.queue ||
        a_point.owner != this)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12QueuePool.wait_gpu"});
    }
    return context(a_waitingQueue).wait_on(context(a_point.queue), a_point.value);
}

/// @brief 各 Queue の先行処理を順に CPU で待つ
Result<void> D3D12QueuePool::wait_idle()
{
    for (const auto type : k_queueTypes)
    {
        auto result = context(type).wait_idle();
        if (!result.has_value())
        {
            return result;
        }
    }
    return Result<void>::success();
}

/// @brief enum 変換を介した境界外参照を防ぐ
bool D3D12QueuePool::is_valid(GpuQueueType a_type) noexcept
{
    return static_cast<std::size_t>(a_type) < k_queueTypes.size();
}
} // namespace cue::detail
