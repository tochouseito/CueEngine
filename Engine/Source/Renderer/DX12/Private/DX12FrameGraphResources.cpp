#include <DX12/DX12FrameGraphResources.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <DX12/DX12GpuResource.h>
#include <DX12/DX12GpuResourcePool.h>
#include <DX12/DX12RenderDevice.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
/// @brief create の内部でのみ空の Resource 所有者を構築する
DX12FrameGraphResources::DX12FrameGraphResources(CreateToken) noexcept
{
}

/// @brief Plan の Alias Slot を実体化し、全 Placed Resource の Activation を計画する
Result<std::unique_ptr<DX12FrameGraphResources>> DX12FrameGraphResources::create(
    DX12RenderDevice& a_device, const FrameGraphPlan& a_plan)
{
    using GraphResult = Result<std::unique_ptr<DX12FrameGraphResources>>;
    // RenderTarget は明示された Texture だけに許可し、未対応の DS／UAV を拒否する
    for (const auto& pass : a_plan.passes())
    {
        for (const auto& use : pass.uses)
        {
            if (a_plan.resources()[use.resource.index].isImported)
            {
                continue;
            }
            const auto& resource = a_plan.resources()[use.resource.index];
            if ((use.state == FrameGraphResourceState::RenderTarget &&
                 (resource.kind != GpuResourceKind::Texture2D || !resource.textureDesc.isRenderTarget)) ||
                use.state == FrameGraphResourceState::DepthRead ||
                use.state == FrameGraphResourceState::DepthWrite ||
                use.state == FrameGraphResourceState::UnorderedAccess)
            {
                return GraphResult::failure({ErrorCategory::InvalidArgument,
                                             "DX12FrameGraphResources.create.unsupported_state"});
            }
        }
    }
    auto poolResult = DX12GpuResourcePool::create(a_device);
    if (!poolResult.has_value())
    {
        return GraphResult::failure(*poolResult.try_error());
    }
    try
    {
        auto graph = std::make_unique<DX12FrameGraphResources>(CreateToken{});
        graph->m_pool = poolResult.take_value();
        graph->m_poolResources.resize(a_plan.resources().size(), nullptr);
        graph->m_poolLeases.reserve(a_plan.resources().size());
        graph->m_barriers.resize(a_plan.passes().size());
        graph->m_planId = a_plan.id();
        if (!a_plan.resources().empty())
        {
            graph->m_graphId = a_plan.resources().front().handle.graphId;
        }

        for (const auto& slot : a_plan.alias_slots())
        {
            Result<std::vector<GpuResourceHandle>> createdResult =
                Result<std::vector<GpuResourceHandle>>::failure(
                    {ErrorCategory::InvalidState, "DX12FrameGraphResources.create.slot"});
            if (slot.kind == GpuResourceKind::Buffer)
            {
                std::vector<GpuBufferDesc> descs;
                descs.reserve(slot.resources.size());
                for (const auto handle : slot.resources)
                {
                    descs.push_back(a_plan.resources()[handle.index].bufferDesc);
                }
                createdResult = graph->m_pool->create_alias_buffers(descs);
            }
            else
            {
                std::vector<GpuTexture2DDesc> descs;
                descs.reserve(slot.resources.size());
                for (const auto handle : slot.resources)
                {
                    descs.push_back(a_plan.resources()[handle.index].textureDesc);
                }
                createdResult = graph->m_pool->create_alias_texture2ds(descs);
            }
            if (!createdResult.has_value() || createdResult.try_value()->size() != slot.resources.size())
            {
                return GraphResult::failure(createdResult.has_value()
                    ? Error{ErrorCategory::InvalidState, "DX12FrameGraphResources.create.group_size"}
                    : *createdResult.try_error());
            }
            auto created = createdResult.take_value();

            ID3D12Resource* previous = nullptr;
            for (std::size_t index = 0; index < slot.resources.size(); ++index)
            {
                const auto handle = slot.resources[index];
                const auto& planned = a_plan.resources()[handle.index];
                auto leaseResult = graph->m_pool->acquire(created[index], GpuResourceAccess::Write);
                if (!leaseResult.has_value())
                {
                    return GraphResult::failure(*leaseResult.try_error());
                }
                auto lease = leaseResult.take_value();
                auto* resource = dynamic_cast<DX12GpuResource*>(lease->resource());
                if (!resource || !resource->resource())
                {
                    return GraphResult::failure({ErrorCategory::InvalidState,
                                                 "DX12FrameGraphResources.create.pool_resource"});
                }
                graph->m_poolResources[handle.index] = resource;
                graph->m_poolLeases.push_back(std::move(lease));
                D3D12_RESOURCE_BARRIER activation{};
                activation.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                activation.Aliasing.pResourceBefore = previous;
                activation.Aliasing.pResourceAfter = resource->resource();
                graph->m_barriers[*planned.firstUse].push_back(activation);
                previous = activation.Aliasing.pResourceAfter;
            }
        }
        return GraphResult::success(std::move(graph));
    }
    catch (const std::bad_alloc&)
    {
        return GraphResult::failure({ErrorCategory::PlatformFailure, "DX12FrameGraphResources.create.allocation"});
    }
}

/// @brief GPU が参照中の Native Resource を先に解放しない
DX12FrameGraphResources::~DX12FrameGraphResources()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("DX12FrameGraphResources.shutdown", *result.try_error(), DiagnosticSeverity::Fatal);
        std::terminate();
    }
}

/// @brief Graph ID と Index を検証して所有する一時 Resource を貸す
DX12GpuResource* DX12FrameGraphResources::resource(FrameGraphResourceHandle a_handle) const noexcept
{
    if (m_isClosed || a_handle.graphId != m_graphId || a_handle.graphId == 0 ||
        a_handle.index >= m_poolResources.size())
    {
        return nullptr;
    }
    return m_poolResources[a_handle.index];
}

/// @brief Pass の実行前に必要な Activation を記録順のまま返す
std::span<const D3D12_RESOURCE_BARRIER> DX12FrameGraphResources::barriers_before_pass(
    std::size_t a_passIndex) const noexcept
{
    if (m_isClosed || a_passIndex >= m_barriers.size())
    {
        return {};
    }
    return m_barriers[a_passIndex];
}

/// @brief 別の Build 結果から作られた Alias Barrier の混入を防ぐ
bool DX12FrameGraphResources::matches_plan(const FrameGraphPlan& a_plan) const noexcept
{
    return !m_isClosed && a_plan.id() != 0 && a_plan.id() == m_planId;
}

/// @brief 同じ Queue への最後の Submit の完了点を保持する
Result<void> DX12FrameGraphResources::mark_submitted(std::shared_ptr<ICommandCompletion> a_completion)
{
    if (m_isClosed || !a_completion)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12FrameGraphResources.mark_submitted"});
    }
    m_completion = std::move(a_completion);
    return Result<void>::success();
}

/// @brief 登録済み Completion が終わってから Barrier の Pointer と Resource を解放する
Result<void> DX12FrameGraphResources::shutdown()
{
    if (m_isClosed)
    {
        return Result<void>::success();
    }
    if (m_completion)
    {
        auto result = m_completion->wait();
        if (!result.has_value())
        {
            return result;
        }
    }
    m_barriers.clear();
    m_poolResources.clear();
    m_poolLeases.clear();
    if (m_pool)
    {
        auto result = m_pool->shutdown();
        if (!result.has_value())
        {
            return result;
        }
        m_pool.reset();
    }
    m_completion.reset();
    m_isClosed = true;
    return Result<void>::success();
}
} // namespace cue::dx12
