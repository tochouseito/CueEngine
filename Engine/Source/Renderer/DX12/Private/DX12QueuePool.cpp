#include "DX12QueuePool.h"

#include <utility>

namespace cue::detail
{
namespace
{
constexpr std::array<GpuQueueType, 3> k_queueTypes = {
    GpuQueueType::Graphics, GpuQueueType::Compute, GpuQueueType::Copy};
constexpr std::array<std::uint32_t, 3> k_queueCounts = {2, 4, 4};
} // namespace

/// @brief Device が生存する間に三種類の Queue を生成する
Result<std::unique_ptr<DX12QueuePool>> DX12QueuePool::create(DX12RenderDevice& a_device)
{
    using PoolResult = Result<std::unique_ptr<DX12QueuePool>>;
    auto pool = std::make_unique<DX12QueuePool>();
    for (const auto type : k_queueTypes)
    {
        const auto kind = static_cast<std::size_t>(type);
        for (std::uint32_t index = 0; index < k_queueCounts[kind]; ++index)
        {
            auto queueResult = DX12GpuCommandQueue::create(a_device, type);
            if (!queueResult.has_value())
            {
                return PoolResult::failure(*queueResult.try_error());
            }
            pool->m_contexts[kind].push_back(queueResult.take_value());
            pool->m_leases[kind].push_back({});
        }
    }
    return PoolResult::success(std::move(pool));
}

/// @brief Pool 内の Queue を借用する。呼出側は有効な GpuQueueType を渡す
DX12GpuCommandQueue& DX12QueuePool::context(GpuQueueType a_type) noexcept
{
    return *m_contexts[static_cast<std::size_t>(a_type)][0];
}

/// @brief 同じ Type の未使用 Queue から一つを貸す
Result<QueueContextLease> DX12QueuePool::acquire_context(GpuQueueType a_type)
{
    if (!is_valid(a_type))
    {
        return Result<QueueContextLease>::failure({ErrorCategory::InvalidArgument,
                                                    "DX12QueuePool.acquire_context.type"});
    }
    const auto kind = static_cast<std::size_t>(a_type);
    for (std::uint32_t index = a_type == GpuQueueType::Graphics ? 1u : 0u;
         index < m_leases[kind].size(); ++index)
    {
        auto& state = m_leases[kind][index];
        if (state.isLeased)
        {
            continue;
        }
        state.isLeased = true;
        ++state.generation;
        if (state.generation == 0)
        {
            ++state.generation;
        }
        return Result<QueueContextLease>::success({a_type, index, state.generation, this});
    }
    return Result<QueueContextLease>::failure({ErrorCategory::InvalidState,
                                                "DX12QueuePool.acquire_context.exhausted"});
}

/// @brief Owner と世代を確認して貸出 Queue を借用する
Result<IQueueContext*> DX12QueuePool::context(QueueContextLease a_lease)
{
    if (!owns(a_lease))
    {
        return Result<IQueueContext*>::failure({ErrorCategory::InvalidState, "DX12QueuePool.context.lease"});
    }
    return Result<IQueueContext*>::success(m_contexts[static_cast<std::size_t>(a_lease.type)][a_lease.index].get());
}

/// @brief Queue の先行作業を完了させてから再貸出可能にする
Result<void> DX12QueuePool::return_context(QueueContextLease a_lease)
{
    if (!owns(a_lease))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12QueuePool.return_context"});
    }
    auto result = m_contexts[static_cast<std::size_t>(a_lease.type)][a_lease.index]->wait_idle();
    if (result.has_value())
    {
        m_leases[static_cast<std::size_t>(a_lease.type)][a_lease.index].isLeased = false;
    }
    return result;
}

/// @brief Fence に Queue 種別と同型内 Index の両方を記録する
Result<GpuFencePoint> DX12QueuePool::signal_context(QueueContextLease a_lease)
{
    if (!owns(a_lease))
    {
        return Result<GpuFencePoint>::failure({ErrorCategory::InvalidState,
                                                "DX12QueuePool.signal_context"});
    }
    auto value = m_contexts[static_cast<std::size_t>(a_lease.type)][a_lease.index]->signal();
    if (!value.has_value())
    {
        return Result<GpuFencePoint>::failure(*value.try_error());
    }
    return Result<GpuFencePoint>::success({a_lease.type, value.take_value(), this, a_lease.index});
}

/// @brief Swap Chain と Back Buffer 描画に共用する Direct Queue を返す
IQueueContext& DX12QueuePool::present_context() noexcept
{
    return context(GpuQueueType::Graphics);
}

/// @brief Present 前後の同期に必要な Graphics 作業だけを待つ
Result<void> DX12QueuePool::wait_for_graphics_queue()
{
    return context(GpuQueueType::Graphics).wait_idle();
}

/// @brief 発行した Fence の Queue 種別と値を一体で返す
Result<GpuFencePoint> DX12QueuePool::signal(GpuQueueType a_queue)
{
    if (!is_valid(a_queue))
    {
        return Result<GpuFencePoint>::failure({ErrorCategory::InvalidArgument, "DX12QueuePool.signal"});
    }
    auto valueResult = context(a_queue).signal();
    if (!valueResult.has_value())
    {
        return Result<GpuFencePoint>::failure(*valueResult.try_error());
    }
    return Result<GpuFencePoint>::success({a_queue, valueResult.take_value(), this, 0});
}

/// @brief Fence 発行元の Event で CPU 完了を待つ
Result<void> DX12QueuePool::wait_cpu(GpuFencePoint a_point)
{
    if (!is_valid(a_point.queue) || a_point.owner != this ||
        a_point.queueIndex >= m_contexts[static_cast<std::size_t>(a_point.queue)].size())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12QueuePool.wait_cpu"});
    }
    return m_contexts[static_cast<std::size_t>(a_point.queue)][a_point.queueIndex]->wait_for(a_point.value);
}

/// @brief 発行元 Fence を指定 Queue の GPU 実行順に追加する
Result<void> DX12QueuePool::wait_gpu(GpuQueueType a_waitingQueue, GpuFencePoint a_point)
{
    if (!is_valid(a_waitingQueue) || !is_valid(a_point.queue) ||
        (a_waitingQueue == a_point.queue && a_point.queueIndex == 0) ||
        a_point.owner != this || a_point.queueIndex >= m_contexts[static_cast<std::size_t>(a_point.queue)].size())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12QueuePool.wait_gpu"});
    }
    return context(a_waitingQueue).wait_on(
        *m_contexts[static_cast<std::size_t>(a_point.queue)][a_point.queueIndex], a_point.value);
}

/// @brief 各 Queue の先行処理を順に CPU で待つ
Result<void> DX12QueuePool::wait_idle()
{
    for (const auto type : k_queueTypes)
    {
        for (auto& queue : m_contexts[static_cast<std::size_t>(type)])
        {
            auto result = queue->wait_idle();
            if (!result.has_value())
            {
                return result;
            }
        }
    }
    return Result<void>::success();
}

/// @brief 返却後や再貸出後の古い Lease を拒否する
bool DX12QueuePool::owns(QueueContextLease a_lease) const noexcept
{
    if (a_lease.owner != this || !is_valid(a_lease.type))
    {
        return false;
    }
    const auto kind = static_cast<std::size_t>(a_lease.type);
    return a_lease.index < m_leases[kind].size() && m_leases[kind][a_lease.index].isLeased &&
           m_leases[kind][a_lease.index].generation == a_lease.generation;
}

/// @brief enum 変換を介した境界外参照を防ぐ
bool DX12QueuePool::is_valid(GpuQueueType a_type) noexcept
{
    return static_cast<std::size_t>(a_type) < k_queueTypes.size();
}
} // namespace cue::detail
