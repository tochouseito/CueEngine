#include "D3D12GraphExecutor.h"

#include <array>
#include <vector>

namespace cue::detail
{
namespace
{
/// @brief Graph の状態を D3D12 の状態へ変換する
Result<D3D12_RESOURCE_STATES> resource_state(GraphResourceState a_state)
{
    using StateResult = Result<D3D12_RESOURCE_STATES>;
    switch (a_state)
    {
    case GraphResourceState::Common:
        return StateResult::success(D3D12_RESOURCE_STATE_COMMON);
    case GraphResourceState::Present:
        return StateResult::success(D3D12_RESOURCE_STATE_PRESENT);
    case GraphResourceState::RenderTarget:
        return StateResult::success(D3D12_RESOURCE_STATE_RENDER_TARGET);
    case GraphResourceState::DepthWrite:
        return StateResult::success(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    case GraphResourceState::ShaderResource:
        return StateResult::success(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    case GraphResourceState::ComputeShaderResource:
        return StateResult::success(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    case GraphResourceState::CopySource:
        return StateResult::success(D3D12_RESOURCE_STATE_COPY_SOURCE);
    case GraphResourceState::CopyDest:
        return StateResult::success(D3D12_RESOURCE_STATE_COPY_DEST);
    case GraphResourceState::UnorderedAccess:
        return StateResult::success(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    default:
        return StateResult::failure({ErrorCategory::InvalidArgument, "D3D12GraphExecutor.resource_state"});
    }
}

/// @brief 制限のある Queue で Resource Barrier を記録できる状態か確認する
bool supports_state(GpuQueueType a_queue, GraphResourceState a_state)
{
    if (a_queue == GpuQueueType::Graphics)
    {
        return true;
    }
    if (a_queue == GpuQueueType::Copy)
    {
        return a_state == GraphResourceState::Common || a_state == GraphResourceState::Present ||
               a_state == GraphResourceState::CopySource || a_state == GraphResourceState::CopyDest;
    }
    return a_state != GraphResourceState::RenderTarget && a_state != GraphResourceState::DepthWrite &&
           a_state != GraphResourceState::ShaderResource;
}
} // namespace

/// @brief Graph の順序で Barrier と Pass を記録し、Submit は呼出側に任せる
Result<void> D3D12GraphExecutor::record(const CompiledFrameGraph& a_graph,
                                        ID3D12GraphicsCommandList* a_list,
                                        const std::vector<ID3D12Resource*>& a_resources,
                                        const std::vector<GraphPassCallback>& a_callbacks)
{
    if (!a_list)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12GraphExecutor.record"});
    }
    for (const auto& pass : a_graph.passes)
    {
        if (pass.queue != GpuQueueType::Graphics)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12GraphExecutor.record.queue"});
        }
        if (pass.sourceIndex >= a_callbacks.size() || !a_callbacks[pass.sourceIndex])
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "D3D12GraphExecutor.passCallback"});
        }
        for (const auto& barrier : pass.barriers)
        {
            auto barrierResult = record_barrier(barrier, a_list, a_resources);
            if (!barrierResult.has_value())
            {
                return barrierResult;
            }
        }
        auto passResult = a_callbacks[pass.sourceIndex](a_list);
        if (!passResult.has_value())
        {
            return passResult;
        }
    }
    for (const auto& barrier : a_graph.finalBarriers)
    {
        auto barrierResult = record_barrier(barrier, a_list, a_resources);
        if (!barrierResult.has_value())
        {
            return barrierResult;
        }
    }
    return Result<void>::success();
}

/// @brief 各 Pass を専用 Context へ記録し、依存先 Queue の Fence を GPU 側で待つ
Result<void> D3D12GraphExecutor::execute(const CompiledFrameGraph& a_graph, D3D12QueuePool& a_queues,
                                         const std::array<D3D12CommandPool*, 3>& a_pools, UINT a_slot,
                                         const std::vector<ID3D12Resource*>& a_resources,
                                         const std::vector<GraphPassCallback>& a_callbacks)
{
    if (a_slot >= k_backBufferCount || !a_pools[0] || !a_pools[1] || !a_pools[2])
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12GraphExecutor.execute"});
    }
    std::vector<GpuFencePoint> completed(a_callbacks.size());
    for (const auto& pass : a_graph.passes)
    {
        const auto queueIndex = static_cast<std::size_t>(pass.queue);
        if (queueIndex >= a_pools.size() || pass.sourceIndex >= a_callbacks.size() ||
            !a_callbacks[pass.sourceIndex])
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "D3D12GraphExecutor.execute.pass"});
        }
        auto& queue = a_queues.context(pass.queue);
        for (const auto predecessor : pass.predecessors)
        {
            if (predecessor >= completed.size() || completed[predecessor].value == 0)
            {
                return Result<void>::failure({ErrorCategory::InvalidState, "D3D12GraphExecutor.execute.dependency"});
            }
            if (completed[predecessor].queue != pass.queue)
            {
                auto waitResult = a_queues.wait_gpu(pass.queue, completed[predecessor]);
                if (!waitResult.has_value())
                {
                    return waitResult;
                }
            }
        }

        // Copy／Compute Queue が扱えない遷移だけ Graphics Queue で先に記録する
        std::vector<const GraphBarrier*> graphicsBarriers;
        for (const auto& barrier : pass.barriers)
        {
            if (barrier.kind == GraphBarrierKind::Transition &&
                (!supports_state(pass.queue, barrier.before) || !supports_state(pass.queue, barrier.after)))
            {
                graphicsBarriers.push_back(&barrier);
            }
        }
        if (!graphicsBarriers.empty())
        {
            auto& graphics = a_queues.context(GpuQueueType::Graphics);
            for (const auto predecessor : pass.predecessors)
            {
                if (completed[predecessor].queue != GpuQueueType::Graphics)
                {
                    auto waitResult = a_queues.wait_gpu(GpuQueueType::Graphics, completed[predecessor]);
                    if (!waitResult.has_value())
                    {
                        return waitResult;
                    }
                }
            }
            auto graphicsLeaseResult = a_pools[0]->acquire_batch(graphics, a_slot);
            if (!graphicsLeaseResult.has_value())
            {
                return Result<void>::failure(*graphicsLeaseResult.try_error());
            }
            const auto graphicsLease = graphicsLeaseResult.take_value();
            for (const auto* barrier : graphicsBarriers)
            {
                auto result = record_barrier(*barrier, graphicsLease.list, a_resources);
                if (!result.has_value())
                {
                    [[maybe_unused]] auto abortResult = a_pools[0]->abort(graphicsLease);
                    return result;
                }
            }
            auto submitResult = a_pools[0]->submit(graphics, graphicsLease);
            if (!submitResult.has_value())
            {
                [[maybe_unused]] auto abortResult = a_pools[0]->abort(graphicsLease);
                return submitResult;
            }
            auto fenceResult = a_pools[0]->retire_fence(graphics, graphicsLease);
            if (!fenceResult.has_value())
            {
                return Result<void>::failure(*fenceResult.try_error());
            }
            if (pass.queue != GpuQueueType::Graphics)
            {
                auto waitResult = queue.wait_on(graphics, fenceResult.take_value());
                if (!waitResult.has_value())
                {
                    return waitResult;
                }
            }
        }

        auto leaseResult = a_pools[queueIndex]->acquire_batch(queue, a_slot);
        if (!leaseResult.has_value())
        {
            return Result<void>::failure(*leaseResult.try_error());
        }
        const auto lease = leaseResult.take_value();
        for (const auto& barrier : pass.barriers)
        {
            if (barrier.kind == GraphBarrierKind::Transition &&
                (!supports_state(pass.queue, barrier.before) || !supports_state(pass.queue, barrier.after)))
            {
                continue;
            }
            auto result = record_barrier(barrier, lease.list, a_resources);
            if (!result.has_value())
            {
                [[maybe_unused]] auto abortResult = a_pools[queueIndex]->abort(lease);
                return result;
            }
        }
        auto recordResult = a_callbacks[pass.sourceIndex](lease.list);
        if (!recordResult.has_value())
        {
            [[maybe_unused]] auto abortResult = a_pools[queueIndex]->abort(lease);
            return recordResult;
        }
        auto submitResult = a_pools[queueIndex]->submit(queue, lease);
        if (!submitResult.has_value())
        {
            [[maybe_unused]] auto abortResult = a_pools[queueIndex]->abort(lease);
            return submitResult;
        }
        auto fenceResult = a_pools[queueIndex]->retire_fence(queue, lease);
        if (!fenceResult.has_value())
        {
            return Result<void>::failure(*fenceResult.try_error());
        }
        completed[pass.sourceIndex] = {pass.queue, fenceResult.take_value(), &a_queues};
    }

    // Graph 終端の状態を Graphics Queue で確定し、別 Queue の書込み完了を GPU 上で待つ
    if (!a_graph.finalBarriers.empty())
    {
        auto& graphics = a_queues.context(GpuQueueType::Graphics);
        for (const auto& fence : completed)
        {
            if (fence.value != 0 && fence.queue != GpuQueueType::Graphics)
            {
                auto waitResult = a_queues.wait_gpu(GpuQueueType::Graphics, fence);
                if (!waitResult.has_value())
                {
                    return waitResult;
                }
            }
        }
        auto leaseResult = a_pools[0]->acquire_batch(graphics, a_slot);
        if (!leaseResult.has_value())
        {
            return Result<void>::failure(*leaseResult.try_error());
        }
        const auto lease = leaseResult.take_value();
        for (const auto& barrier : a_graph.finalBarriers)
        {
            auto result = record_barrier(barrier, lease.list, a_resources);
            if (!result.has_value())
            {
                [[maybe_unused]] auto abortResult = a_pools[0]->abort(lease);
                return result;
            }
        }
        auto submitResult = a_pools[0]->submit(graphics, lease);
        if (!submitResult.has_value())
        {
            [[maybe_unused]] auto abortResult = a_pools[0]->abort(lease);
            return submitResult;
        }
        auto retireResult = a_pools[0]->retire(graphics, lease);
        if (!retireResult.has_value())
        {
            return retireResult;
        }
    }
    return Result<void>::success();
}

/// @brief Resource Handle を物理 Resource へ対応付けて Barrier を記録する
Result<void> D3D12GraphExecutor::record_barrier(const GraphBarrier& a_barrier,
                                                ID3D12GraphicsCommandList* a_list,
                                                const std::vector<ID3D12Resource*>& a_resources)
{
    if (a_barrier.resource.index >= a_resources.size() || !a_resources[a_barrier.resource.index])
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12GraphExecutor.resource"});
    }
    D3D12_RESOURCE_BARRIER barrier{};
    if (a_barrier.kind == GraphBarrierKind::UnorderedAccess)
    {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = a_resources[a_barrier.resource.index];
    }
    else
    {
        auto beforeResult = resource_state(a_barrier.before);
        auto afterResult = resource_state(a_barrier.after);
        if (!beforeResult.has_value() || !afterResult.has_value())
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument, "D3D12GraphExecutor.barrierState"});
        }
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = a_resources[a_barrier.resource.index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = beforeResult.take_value();
        barrier.Transition.StateAfter = afterResult.take_value();
    }
    a_list->ResourceBarrier(1, &barrier);
    return Result<void>::success();
}
} // namespace cue::detail
