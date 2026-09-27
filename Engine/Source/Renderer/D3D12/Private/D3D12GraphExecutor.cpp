#include "D3D12GraphExecutor.h"

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
        return StateResult::success(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
