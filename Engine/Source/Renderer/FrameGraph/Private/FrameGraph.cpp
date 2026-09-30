#include <Cue/Renderer/FrameGraph/FrameGraph.h>

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

namespace cue
{
namespace
{
std::atomic<std::uint64_t> s_nextGraphId = 1;

/// @brief 状態と Access の組合せが一つの Pass で有効か確認する
bool is_valid_use(const GraphResourceUse& a_use)
{
    switch (a_use.state)
    {
    case GraphResourceState::RenderTarget:
    case GraphResourceState::DepthWrite:
    case GraphResourceState::CopyDest:
        return a_use.access == GraphAccess::Write;
    case GraphResourceState::ShaderResource:
    case GraphResourceState::ComputeShaderResource:
    case GraphResourceState::CopySource:
    case GraphResourceState::IndirectArgument:
        return a_use.access == GraphAccess::Read;
    case GraphResourceState::UnorderedAccess:
        return true;
    default:
        return false;
    }
}

/// @brief 後続 Pass が先行 Pass の読書きを追い越せない条件を返す
bool has_hazard(GraphAccess a_before, GraphAccess a_after)
{
    return a_before != GraphAccess::Read || a_after != GraphAccess::Read;
}
} // namespace

/// @brief 他の Builder と Handle を混同しない識別子を割り当てる
FrameGraphBuilder::FrameGraphBuilder()
    : m_graphId(s_nextGraphId.fetch_add(1, std::memory_order_relaxed))
{
}

/// @brief 外部 Owner の Resource を初期状態と終了状態付きで取り込む
Result<GraphResourceHandle> FrameGraphBuilder::import_resource(std::string a_name,
                                                               GraphResourceState a_initial,
                                                               GraphResourceState a_final)
{
    using HandleResult = Result<GraphResourceHandle>;
    if (a_name.empty() || a_initial == GraphResourceState::Undefined ||
        a_final == GraphResourceState::Undefined)
    {
        return HandleResult::failure({ErrorCategory::InvalidArgument, "FrameGraph.import_resource"});
    }
    const auto index = static_cast<std::uint32_t>(m_resources.size());
    m_resources.push_back({std::move(a_name), GraphResourceLifetime::Imported, a_initial, a_final});
    return HandleResult::success({index, m_graphId});
}

/// @brief Backend が所有する永続または一時 Resource を宣言する
Result<GraphResourceHandle> FrameGraphBuilder::create_resource(std::string a_name,
                                                               GraphResourceLifetime a_lifetime,
                                                               GraphResourceState a_initial,
                                                               GraphResourceState a_final)
{
    using HandleResult = Result<GraphResourceHandle>;
    if (a_name.empty() || a_lifetime == GraphResourceLifetime::Imported ||
        a_initial == GraphResourceState::Undefined)
    {
        return HandleResult::failure({ErrorCategory::InvalidArgument, "FrameGraph.create_resource"});
    }
    const auto index = static_cast<std::uint32_t>(m_resources.size());
    m_resources.push_back({std::move(a_name), a_lifetime, a_initial, a_final});
    return HandleResult::success({index, m_graphId});
}

/// @brief Resource 使用を明示した Pass を登録する
Result<GraphPassHandle> FrameGraphBuilder::add_pass(std::string a_name, std::vector<GraphResourceUse> a_uses)
{
    return add_pass(std::move(a_name), std::move(a_uses), GpuQueueType::Graphics);
}

/// @brief Queue が扱える状態と Resource 使用を検証して Pass を登録する
Result<GraphPassHandle> FrameGraphBuilder::add_pass(std::string a_name, std::vector<GraphResourceUse> a_uses,
                                                     GpuQueueType a_queue)
{
    using HandleResult = Result<GraphPassHandle>;
    if (a_name.empty() || a_uses.empty() || static_cast<std::uint32_t>(a_queue) >
        static_cast<std::uint32_t>(GpuQueueType::Copy))
    {
        return HandleResult::failure({ErrorCategory::InvalidArgument, "FrameGraph.add_pass"});
    }
    for (std::size_t index = 0; index < a_uses.size(); ++index)
    {
        if (!owns(a_uses[index].resource) || !is_valid_use(a_uses[index]) ||
            (a_queue == GpuQueueType::Copy && a_uses[index].state != GraphResourceState::CopySource &&
             a_uses[index].state != GraphResourceState::CopyDest) ||
            (a_queue == GpuQueueType::Compute &&
             (a_uses[index].state == GraphResourceState::RenderTarget ||
              a_uses[index].state == GraphResourceState::DepthWrite ||
              a_uses[index].state == GraphResourceState::ShaderResource)))
        {
            return HandleResult::failure({ErrorCategory::InvalidArgument, "FrameGraph.passUse"});
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier)
        {
            if (a_uses[earlier].resource.index == a_uses[index].resource.index)
            {
                return HandleResult::failure({ErrorCategory::InvalidState, "FrameGraph.conflictingPassUse"});
            }
        }
    }
    const auto index = static_cast<std::uint32_t>(m_passes.size());
    m_passes.push_back({std::move(a_name), std::move(a_uses), a_queue});
    return HandleResult::success({index, m_graphId});
}

/// @brief 先行 Pass を明示し、循環は compile で拒否する
Result<void> FrameGraphBuilder::add_dependency(GraphPassHandle a_before, GraphPassHandle a_after)
{
    if (!owns(a_before) || !owns(a_after) || a_before.index == a_after.index)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "FrameGraph.add_dependency"});
    }
    m_dependencies.emplace_back(a_before.index, a_after.index);
    return Result<void>::success();
}

/// @brief 依存順、不正使用、Barrier を確定し、失敗時は Builder を変更しない
Result<CompiledFrameGraph> FrameGraphBuilder::compile() const
{
    using GraphResult = Result<CompiledFrameGraph>;
    const std::size_t passCount = m_passes.size();
    std::vector<std::vector<bool>> edges(passCount, std::vector<bool>(passCount, false));
    for (const auto& [before, after] : m_dependencies)
    {
        edges[before][after] = true;
    }

    // Queue をまたぐ同一 Resource の読み取りも状態遷移と所有順を確定させる
    for (std::size_t before = 0; before < passCount; ++before)
    {
        for (std::size_t after = before + 1; after < passCount; ++after)
        {
            if (m_passes[before].queue == m_passes[after].queue)
            {
                continue;
            }
            for (const auto& earlierUse : m_passes[before].uses)
            {
                for (const auto& laterUse : m_passes[after].uses)
                {
                    if (earlierUse.resource.index == laterUse.resource.index)
                    {
                        edges[before][after] = true;
                    }
                }
            }
        }
    }

    // 登録順の Resource Hazard を明示依存へ加え、複数 Reader の後の Writer も追い越させない
    for (std::size_t resourceIndex = 0; resourceIndex < m_resources.size(); ++resourceIndex)
    {
        for (std::size_t before = 0; before < passCount; ++before)
        {
            for (const auto& earlierUse : m_passes[before].uses)
            {
                if (earlierUse.resource.index != resourceIndex)
                {
                    continue;
                }
                for (std::size_t after = before + 1; after < passCount; ++after)
                {
                    for (const auto& laterUse : m_passes[after].uses)
                    {
                        if (laterUse.resource.index == resourceIndex &&
                            has_hazard(earlierUse.access, laterUse.access))
                        {
                            edges[before][after] = true;
                        }
                    }
                }
            }
        }
    }

    std::vector<std::uint32_t> order;
    std::vector<bool> scheduled(passCount, false);
    for (std::size_t step = 0; step < passCount; ++step)
    {
        bool found = false;
        for (std::size_t candidate = 0; candidate < passCount; ++candidate)
        {
            if (scheduled[candidate])
            {
                continue;
            }
            bool isBlocked = false;
            for (std::size_t predecessor = 0; predecessor < passCount; ++predecessor)
            {
                isBlocked |= !scheduled[predecessor] && edges[predecessor][candidate];
            }
            if (!isBlocked)
            {
                scheduled[candidate] = true;
                order.push_back(static_cast<std::uint32_t>(candidate));
                found = true;
                break;
            }
        }
        if (!found)
        {
            return GraphResult::failure({ErrorCategory::InvalidState, "FrameGraph.cycle"});
        }
    }

    CompiledFrameGraph graph;
    graph.passes.reserve(passCount);
    std::vector<GraphResourceState> states;
    std::vector<bool> produced;
    std::vector<bool> previousUavWrite(m_resources.size(), false);
    for (const auto& resource : m_resources)
    {
        states.push_back(resource.initial);
        produced.push_back(resource.lifetime != GraphResourceLifetime::Transient);
    }
    for (const std::uint32_t index : order)
    {
        GraphPassPlan plan{index, m_passes[index].name, {}};
        plan.queue = m_passes[index].queue;
        plan.uses = m_passes[index].uses;
        for (std::size_t predecessor = 0; predecessor < passCount; ++predecessor)
        {
            if (edges[predecessor][index])
            {
                plan.predecessors.push_back(static_cast<std::uint32_t>(predecessor));
            }
        }
        for (const auto& use : m_passes[index].uses)
        {
            const std::size_t resource = use.resource.index;
            if (!produced[resource] && use.access != GraphAccess::Write)
            {
                return GraphResult::failure({ErrorCategory::InvalidState, "FrameGraph.readBeforeWrite"});
            }
            if (states[resource] != use.state)
            {
                plan.barriers.push_back({use.resource, GraphBarrierKind::Transition,
                                         states[resource], use.state});
            }
            else if (use.state == GraphResourceState::UnorderedAccess && previousUavWrite[resource])
            {
                plan.barriers.push_back({use.resource, GraphBarrierKind::UnorderedAccess,
                                         use.state, use.state});
            }
            states[resource] = use.state;
            if (use.access != GraphAccess::Read)
            {
                produced[resource] = true;
            }
            previousUavWrite[resource] = use.state == GraphResourceState::UnorderedAccess &&
                                          use.access != GraphAccess::Read;
        }
        graph.passes.push_back(std::move(plan));
    }
    for (std::size_t index = 0; index < m_resources.size(); ++index)
    {
        const auto final = m_resources[index].final;
        if (final != GraphResourceState::Undefined && states[index] != final)
        {
            graph.finalBarriers.push_back({{static_cast<std::uint32_t>(index), m_graphId},
                                           GraphBarrierKind::Transition, states[index], final});
        }
    }
    return GraphResult::success(std::move(graph));
}

/// @brief Resource Handle がこの Graph の現行 Index を指すか確認する
bool FrameGraphBuilder::owns(GraphResourceHandle a_handle) const noexcept
{
    return a_handle.graphId == m_graphId && a_handle.index < m_resources.size();
}

/// @brief 外部 Graph の Handle を拒否して宣言済みの最終状態を返す
Result<GraphResourceState> FrameGraphBuilder::final_state(GraphResourceHandle a_handle) const
{
    if (!owns(a_handle))
    {
        return Result<GraphResourceState>::failure({ErrorCategory::InvalidArgument,
                                                     "FrameGraph.final_state"});
    }
    return Result<GraphResourceState>::success(m_resources[a_handle.index].final);
}

/// @brief 前 Frame が COMMON へ戻した共有 Surface の開始状態と照合する
Result<GraphResourceState> FrameGraphBuilder::initial_state(GraphResourceHandle a_handle) const
{
    if (!owns(a_handle))
    {
        return Result<GraphResourceState>::failure({ErrorCategory::InvalidArgument,
                                                     "FrameGraph.initial_state"});
    }
    return Result<GraphResourceState>::success(m_resources[a_handle.index].initial);
}

/// @brief Pass Handle がこの Graph の現行 Index を指すか確認する
bool FrameGraphBuilder::owns(GraphPassHandle a_handle) const noexcept
{
    return a_handle.graphId == m_graphId && a_handle.index < m_passes.size();
}
} // namespace cue
