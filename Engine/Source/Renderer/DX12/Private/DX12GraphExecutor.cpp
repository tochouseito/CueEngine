#include "DX12GraphExecutor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

namespace cue::detail
{
namespace
{
/// @brief Graph の状態を DX12 の状態へ変換する
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
    case GraphResourceState::IndirectArgument:
        return StateResult::success(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    default:
        return StateResult::failure({ErrorCategory::InvalidArgument, "DX12GraphExecutor.resource_state"});
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
Result<void> DX12GraphExecutor::record(const CompiledFrameGraph& a_graph,
                                        ID3D12GraphicsCommandList* a_list,
                                        const std::vector<ID3D12Resource*>& a_resources,
                                        const std::vector<GraphPassCallback>& a_callbacks)
{
    if (!a_list || std::ranges::find(a_resources, nullptr) != a_resources.end())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12GraphExecutor.record"});
    }
    for (std::size_t index = 0; index < a_resources.size(); ++index)
    {
        if (std::ranges::find(a_resources.begin(), a_resources.begin() + index, a_resources[index]) !=
            a_resources.begin() + index)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument,
                                          "DX12GraphExecutor.record.alias"});
        }
    }
    for (const auto& pass : a_graph.passes)
    {
        if (pass.queue != GpuQueueType::Graphics)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12GraphExecutor.record.queue"});
        }
        if (pass.sourceIndex >= a_callbacks.size() || !a_callbacks[pass.sourceIndex])
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12GraphExecutor.passCallback"});
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
Result<void> DX12GraphExecutor::execute(const CompiledFrameGraph& a_graph, DX12QueuePool& a_queues,
                                         const std::array<DX12CommandPool*, 3>& a_pools, UINT a_slot,
                                         const std::vector<ID3D12Resource*>& a_resources,
                                         const std::vector<GraphPassCallback>& a_callbacks,
                                         FrameGraphExecutionStats* a_stats)
{
    if (a_slot >= k_backBufferCount || !a_pools[0] || !a_pools[1] || !a_pools[2] ||
        std::ranges::find(a_resources, nullptr) != a_resources.end())
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12GraphExecutor.execute"});
    }
    for (std::size_t index = 0; index < a_resources.size(); ++index)
    {
        if (std::ranges::find(a_resources.begin(), a_resources.begin() + index, a_resources[index]) !=
            a_resources.begin() + index)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument,
                                          "DX12GraphExecutor.execute.alias"});
        }
    }
    const auto started = std::chrono::steady_clock::now();
    FrameGraphExecutionStats measured{};
    measured.passStats.resize(a_callbacks.size());
    std::vector<GpuFencePoint> completed(a_callbacks.size());
    std::vector<bool> pendingCommon(a_resources.size(), false);
    for (std::size_t passIndex = 0; passIndex < a_graph.passes.size(); ++passIndex)
    {
        const auto& pass = a_graph.passes[passIndex];
        const auto queueIndex = static_cast<std::size_t>(pass.queue);
        if (queueIndex >= a_pools.size() || pass.sourceIndex >= a_callbacks.size() ||
            !a_callbacks[pass.sourceIndex])
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12GraphExecutor.execute.pass"});
        }
        auto& queue = a_queues.context(pass.queue);
        for (const auto predecessor : pass.predecessors)
        {
            if (predecessor >= completed.size() || completed[predecessor].value == 0)
            {
                return Result<void>::failure({ErrorCategory::InvalidState, "DX12GraphExecutor.execute.dependency"});
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

        // 前の Queue が COMMON に戻した Resource は、Graph の論理状態から遷移し直さない
        std::vector<GraphBarrier> barriers = pass.barriers;
        for (const auto& use : pass.uses)
        {
            if (use.resource.index >= pendingCommon.size())
            {
                return Result<void>::failure({ErrorCategory::InvalidState,
                                              "DX12GraphExecutor.execute.resourceUse"});
            }
            if (!pendingCommon[use.resource.index])
            {
                continue;
            }
            bool hasBarrier = false;
            for (auto& barrier : barriers)
            {
                if (barrier.resource.index != use.resource.index)
                {
                    continue;
                }
                barrier.kind = GraphBarrierKind::Transition;
                barrier.before = GraphResourceState::Common;
                barrier.after = use.state;
                hasBarrier = true;
                break;
            }
            if (!hasBarrier && use.state != GraphResourceState::Common)
            {
                barriers.push_back({use.resource, GraphBarrierKind::Transition,
                                    GraphResourceState::Common, use.state});
            }
            pendingCommon[use.resource.index] = false;
        }

        // COPY Queue へ渡す前は DIRECT Queue 側で COMMON に戻す
        std::vector<GraphBarrier> graphicsBarriers;
        std::vector<bool> recordedOnGraphics(barriers.size(), false);
        for (std::size_t barrierIndex = 0; barrierIndex < barriers.size(); ++barrierIndex)
        {
            auto& barrier = barriers[barrierIndex];
            if (barrier.kind != GraphBarrierKind::Transition)
            {
                continue;
            }
            if (pass.queue == GpuQueueType::Copy && !supports_state(pass.queue, barrier.before))
            {
                if (!supports_state(pass.queue, barrier.after))
                {
                    return Result<void>::failure({ErrorCategory::InvalidState,
                                                  "DX12GraphExecutor.execute.copyState"});
                }
                graphicsBarriers.push_back({barrier.resource, GraphBarrierKind::Transition,
                                            barrier.before, GraphResourceState::Common});
                barrier.before = GraphResourceState::Common;
            }
            else if (!supports_state(pass.queue, barrier.before) ||
                     !supports_state(pass.queue, barrier.after))
            {
                graphicsBarriers.push_back(barrier);
                recordedOnGraphics[barrierIndex] = true;
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
            for (const auto& barrier : graphicsBarriers)
            {
                auto result = record_barrier(barrier, graphicsLease.list, a_resources);
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
        for (std::size_t barrierIndex = 0; barrierIndex < barriers.size(); ++barrierIndex)
        {
            const auto& barrier = barriers[barrierIndex];
            if (recordedOnGraphics[barrierIndex] ||
                (pass.queue == GpuQueueType::Copy && barrier.kind == GraphBarrierKind::Transition) ||
                (barrier.kind == GraphBarrierKind::Transition && barrier.before == barrier.after))
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
        const auto passStarted = std::chrono::steady_clock::now();
        auto recordResult = a_callbacks[pass.sourceIndex](lease.list);
        const auto passEnded = std::chrono::steady_clock::now();
        measured.passStats[pass.sourceIndex] = {
            pass.name, pass.queue,
            std::chrono::duration<double, std::milli>(passEnded - passStarted).count()};
        if (!recordResult.has_value())
        {
            [[maybe_unused]] auto abortResult = a_pools[queueIndex]->abort(lease);
            return recordResult;
        }
        // 次の使用が COPY と他 Queue をまたぐ場合は Submit 前に COMMON へ戻す
        for (const auto& use : pass.uses)
        {
            bool needsHandoff = false;
            bool hasNextUse = false;
            for (std::size_t nextIndex = passIndex + 1; nextIndex < a_graph.passes.size() && !hasNextUse;
                 ++nextIndex)
            {
                for (const auto& nextUse : a_graph.passes[nextIndex].uses)
                {
                    if (nextUse.resource.index == use.resource.index)
                    {
                        const auto nextQueue = a_graph.passes[nextIndex].queue;
                        needsHandoff = nextQueue != pass.queue &&
                            (nextQueue == GpuQueueType::Copy || pass.queue == GpuQueueType::Copy);
                        hasNextUse = true;
                        break;
                    }
                }
            }
            if (!hasNextUse && pass.queue == GpuQueueType::Copy)
            {
                for (const auto& finalBarrier : a_graph.finalBarriers)
                {
                    if (finalBarrier.resource.index == use.resource.index)
                    {
                        needsHandoff = true;
                        break;
                    }
                }
            }
            if (!needsHandoff)
            {
                continue;
            }
            // COPY Queue の使用状態は ExecuteCommandLists の完了時に COMMON へ減衰する
            if (pass.queue != GpuQueueType::Copy && use.state != GraphResourceState::Common)
            {
                auto result = record_barrier({use.resource, GraphBarrierKind::Transition,
                                               use.state, GraphResourceState::Common},
                                              lease.list, a_resources);
                if (!result.has_value())
                {
                    [[maybe_unused]] auto abortResult = a_pools[queueIndex]->abort(lease);
                    return result;
                }
            }
            pendingCommon[use.resource.index] = true;
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

    // COPY Queue が COMMON に戻した Resource はその状態を起点に最終遷移する
    std::vector<GraphBarrier> finalBarriers;
    for (auto barrier : a_graph.finalBarriers)
    {
        if (barrier.resource.index >= pendingCommon.size())
        {
            return Result<void>::failure({ErrorCategory::InvalidState,
                                          "DX12GraphExecutor.execute.finalResource"});
        }
        if (pendingCommon[barrier.resource.index])
        {
            barrier.before = GraphResourceState::Common;
        }
        if (barrier.kind != GraphBarrierKind::Transition || barrier.before != barrier.after)
        {
            finalBarriers.push_back(barrier);
        }
    }
    // Graph 終端の状態を Graphics Queue で確定し、別 Queue の書込み完了を GPU 上で待つ
    if (!finalBarriers.empty())
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
        for (const auto& barrier : finalBarriers)
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
    else
    {
        // 最終 Barrier がない Graph でも、後続の Graphics Copy は他 Queue の終了を待つ
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
    }
    if (a_stats)
    {
        measured.totalExecuteMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        *a_stats = std::move(measured);
    }
    return Result<void>::success();
}

/// @brief Resource Handle を物理 Resource へ対応付けて Barrier を記録する
Result<void> DX12GraphExecutor::record_barrier(const GraphBarrier& a_barrier,
                                                ID3D12GraphicsCommandList* a_list,
                                                const std::vector<ID3D12Resource*>& a_resources)
{
    if (a_barrier.resource.index >= a_resources.size() || !a_resources[a_barrier.resource.index])
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12GraphExecutor.resource"});
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
            return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12GraphExecutor.barrierState"});
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
