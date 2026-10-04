#include <DX12/DX12MainFrameGraph.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12FrameGraphExecutor.h>
#include <DX12/DX12FrameGraphContext.h>
#include <DX12/DX12FrameGraphFrames.h>
#include <DX12/DX12FrameGraphResources.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include "DX12FullscreenTriangle.h"
#include <Passes/MainFrameGraph.h>
#include <Platform/Diagnostics.h>

namespace cue::dx12
{
namespace
{
/// @brief 最終提出より前に共有所有を確保する完了 Token
class BatchCompletion final : public ICommandCompletion
{
public:
    /// @brief 提出結果の一意所有を共有 Control Block へ移す
    void set(commandCompletion a_completion) noexcept
    {
        m_completion = std::move(a_completion);
    }

    /// @brief 最終提出の Queue 種類を返す
    [[nodiscard]] QueueType type() const noexcept override
    {
        return m_completion ? m_completion->type() : QueueType::Graphics;
    }

    /// @brief 最終提出の GPU 完了を返す
    [[nodiscard]] bool is_complete() const noexcept override
    {
        return m_completion && m_completion->is_complete();
    }

    /// @brief 最終提出の GPU 完了を待つ
    [[nodiscard]] Result<void> wait() override
    {
        return m_completion ? m_completion->wait() :
            Result<void>::failure({ErrorCategory::InvalidState, "BatchCompletion.wait"});
    }

private:
    commandCompletion m_completion;
};
} // namespace

/// @brief Graph と枠ごとの Texture の所有権を受け取る
DX12MainFrameGraph::DX12MainFrameGraph(CreateToken, std::unique_ptr<FrameGraph> a_graph,
                                       FrameGraphResourceHandle a_backBuffer,
                                       std::unique_ptr<DX12FrameGraphFrames> a_frames,
                                       std::unique_ptr<DX12FullscreenTriangle> a_fullscreenTriangle,
                                       DX12SwapChain& a_swapChain)
    : m_graph(std::move(a_graph)), m_backBuffer(a_backBuffer),
      m_frames(std::move(a_frames)), m_fullscreenTriangle(std::move(a_fullscreenTriangle)),
      m_poolLeases(m_frames->frame_count()),
      m_externalBindings(m_frames->frame_count()), m_isPrepared(m_frames->frame_count(), false),
      m_swapChain(&a_swapChain)
{
}

/// @brief 旧 Pass 契約に従って Clear、追加 Pass、表示 Pass を組み立てる
Result<std::unique_ptr<DX12MainFrameGraph>> DX12MainFrameGraph::create(
    DX12RenderDevice& a_device, DX12SwapChain& a_swapChain, std::uint32_t a_frameCount,
    DX12DescriptorAllocator& a_rtvAllocator, DX12DescriptorAllocator& a_srvAllocator,
    std::array<float, 4> a_clearColor, frameGraphConfigure a_configure)
{
    using GraphResult = Result<std::unique_ptr<DX12MainFrameGraph>>;
    auto* backBuffer = a_swapChain.back_buffer(0);
    if (!a_device.device() || !backBuffer || !a_swapChain.graphics_queue() || a_frameCount == 0)
    {
        return GraphResult::failure({ErrorCategory::InvalidArgument, "DX12MainFrameGraph.create"});
    }
    const auto nativeDesc = backBuffer->GetDesc();
    if (nativeDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || nativeDesc.Width == 0 ||
        nativeDesc.Width > (std::numeric_limits<std::uint32_t>::max)() || nativeDesc.Height == 0 ||
        nativeDesc.Height > (std::numeric_limits<std::uint32_t>::max)() || nativeDesc.MipLevels != 1 ||
        nativeDesc.DepthOrArraySize != 1 || nativeDesc.SampleDesc.Count != 1 ||
        (nativeDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
        (nativeDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && nativeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM))
    {
        return GraphResult::failure({ErrorCategory::InvalidArgument, "DX12MainFrameGraph.create.back_buffer"});
    }
    GpuTexture2DDesc colorDesc{static_cast<std::uint32_t>(nativeDesc.Width),
                               static_cast<std::uint32_t>(nativeDesc.Height)};
    colorDesc.format = nativeDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ?
        GpuTextureFormat::Rgba8Unorm : GpuTextureFormat::Bgra8Unorm;
    colorDesc.isRenderTarget = true;
    colorDesc.clearColor = a_clearColor;
    auto compositionResult = create_main_frame_graph(colorDesc, std::move(a_configure));
    if (!compositionResult.has_value())
    {
        return GraphResult::failure(*compositionResult.try_error());
    }
    auto composition = compositionResult.take_value();
    try
    {
        auto fullscreenResult = DX12FullscreenTriangle::create(a_device, nativeDesc.Format);
        if (!fullscreenResult.has_value())
        {
            return GraphResult::failure(*fullscreenResult.try_error());
        }
        auto framesResult = DX12FrameGraphFrames::create(a_device, *composition.graph->plan(), a_frameCount,
                                                       a_rtvAllocator, a_srvAllocator, composition.backBuffer);
        if (!framesResult.has_value())
        {
            return GraphResult::failure(*framesResult.try_error());
        }
        return GraphResult::success(std::make_unique<DX12MainFrameGraph>(
            CreateToken{}, std::move(composition.graph), composition.backBuffer,
            framesResult.take_value(), fullscreenResult.take_value(), a_swapChain));
    }
    catch (const std::bad_alloc&)
    {
        return GraphResult::failure({ErrorCategory::PlatformFailure, "DX12MainFrameGraph.create.allocation"});
    }
}

/// @brief 明示停止されなかった枠も GPU 完了を待って破棄する
DX12MainFrameGraph::~DX12MainFrameGraph()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("DX12MainFrameGraph.shutdown", *result.try_error(), DiagnosticSeverity::Fatal);
        std::terminate();
    }
}

/// @brief Barrier 計画と各 Pass の execute を同じ Graphics List に記録する
Result<void> DX12MainFrameGraph::record(std::uint32_t a_frameIndex, DX12GpuCommandContext& a_context)
{
    if (!m_graph || !m_graph->plan() || a_context.type() != QueueType::Graphics ||
        a_context.state() != CommandState::Recording)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.record"});
    }
    auto enabledResult = m_graph->validate_enabled();
    if (!enabledResult.has_value())
    {
        return enabledResult;
    }
    auto prepareResult = prepare_frame(a_frameIndex);
    if (!prepareResult.has_value())
    {
        return prepareResult;
    }
    auto recordResult = record_range(a_frameIndex, a_context, 0, m_graph->plan()->passes().size(), true);
    if (!recordResult.has_value())
    {
        discard_unsubmitted(a_frameIndex);
    }
    return recordResult;
}

/// @brief 複数 Queue へ分ける前に枠の物理 Resource を一度だけ借用する
Result<void> DX12MainFrameGraph::prepare_frame(std::uint32_t a_frameIndex)
{
    if (!m_graph || !m_graph->plan() || !m_frames || !m_swapChain ||
        a_frameIndex >= m_frames->frame_count() || m_isPrepared[a_frameIndex] ||
        !m_poolLeases[a_frameIndex].empty())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.prepare_frame"});
    }
    auto* backBuffer = m_swapChain->back_buffer(m_swapChain->current_index());
    if (!backBuffer || !m_frames->graph_resources(a_frameIndex))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.prepare_frame.resources"});
    }
    auto beginResult = m_frames->begin_frame(a_frameIndex);
    if (!beginResult.has_value())
    {
        return beginResult;
    }
    try
    {
        std::vector<DX12FrameGraphExternalResource> external;
        std::vector<gpuResourceLease> leases;
        external.reserve(m_graph->plan()->resources().size() + 1);
        external.push_back({m_backBuffer, backBuffer});
        for (const auto& plannedResource : m_graph->plan()->resources())
        {
            if (!plannedResource.pool)
            {
                continue;
            }
            bool isWritten = false;
            for (const auto& pass : m_graph->plan()->passes())
            {
                for (const auto& use : pass.uses)
                {
                    if (use.resource.index == plannedResource.handle.index &&
                        use.access == FrameGraphAccess::Write)
                    {
                        isWritten = true;
                    }
                }
            }
            auto leaseResult = plannedResource.pool->acquire(
                plannedResource.poolHandle, isWritten ? GpuResourceAccess::Write : GpuResourceAccess::Read);
            if (!leaseResult.has_value())
            {
                return Result<void>::failure(*leaseResult.try_error());
            }
            auto lease = leaseResult.take_value();
            auto* resource = dynamic_cast<DX12GpuResource*>(lease->resource());
            if (!resource || !resource->resource())
            {
                return Result<void>::failure({ErrorCategory::InvalidState,
                                              "DX12MainFrameGraph.prepare_frame.pool_resource"});
            }
            external.push_back({plannedResource.handle, resource->resource()});
            leases.push_back(std::move(lease));
        }
        auto backRtvResult = m_swapChain->rtv(m_swapChain->current_index());
        if (!backRtvResult.has_value())
        {
            return Result<void>::failure(*backRtvResult.try_error());
        }
        auto viewsResult = m_frames->prepare_imported_views(a_frameIndex, *m_graph->plan(), external,
                                                             *backRtvResult.try_value());
        if (!viewsResult.has_value())
        {
            return viewsResult;
        }
        m_externalBindings[a_frameIndex] = std::move(external);
        m_poolLeases[a_frameIndex] = std::move(leases);
        m_isPrepared[a_frameIndex] = true;
        return Result<void>::success();
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure,
                                      "DX12MainFrameGraph.prepare_frame.allocation"});
    }
}

/// @brief 準備済み Binding を使い、各 Queue に割り当てた範囲だけを記録する
Result<void> DX12MainFrameGraph::record_range(
    std::uint32_t a_frameIndex, DX12GpuCommandContext& a_context,
    std::size_t a_firstPass, std::size_t a_passCount, bool a_includeFinal)
{
    if (!m_graph || !m_graph->plan() || !m_frames || a_frameIndex >= m_isPrepared.size() ||
        !m_isPrepared[a_frameIndex] || a_context.state() != CommandState::Recording)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.record_range"});
    }
    auto enabledResult = m_graph->validate_enabled();
    if (!enabledResult.has_value())
    {
        return enabledResult;
    }
    auto* resources = m_frames->graph_resources(a_frameIndex);
    if (!resources)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.record_range.resources"});
    }
    try
    {
        std::vector<dx12FrameGraphPassCallback> callbacks;
        callbacks.reserve(m_graph->plan()->passes().size());
        for (const auto& planned : m_graph->plan()->passes())
        {
            auto* pass = m_graph->pass(planned.handle);
            if (!pass)
            {
                return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.record_range.pass"});
            }
            callbacks.push_back([this, pass, a_frameIndex, &a_context, plannedPass = &planned]
                                (ID3D12GraphicsCommandList&, const DX12FrameGraphPassContext& a_resources)
                                -> Result<void>
            {
                DX12FrameGraphContext context(m_graph->width(), m_graph->height(), a_frameIndex,
                                              a_context, a_resources, *m_graph->plan(), *plannedPass,
                                              *m_frames, *m_fullscreenTriangle);
                return pass->execute(context);
            });
        }
        return DX12FrameGraphExecutor::record_range(
            *m_graph->plan(), *resources, m_externalBindings[a_frameIndex], callbacks,
            a_context, a_firstPass, a_passCount, a_includeFinal);
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure,
                                      "DX12MainFrameGraph.record_range.allocation"});
    }
}

/// @brief GPU が終わった枠の Pool Lease と Native Binding を返す
void DX12MainFrameGraph::clear_frame(std::uint32_t a_frameIndex) noexcept
{
    m_poolLeases[a_frameIndex].clear();
    m_externalBindings[a_frameIndex].clear();
    m_isPrepared[a_frameIndex] = false;
}

/// @brief Pool が返した基底 Context を DX12 記録器へ渡す
Result<void> DX12MainFrameGraph::record(std::uint32_t a_frameIndex, ICommandContext& a_context)
{
    auto* context = dynamic_cast<DX12GpuCommandContext*>(&a_context);
    if (!context)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12MainFrameGraph.record.context"});
    }
    return record(a_frameIndex, *context);
}

/// @brief Graph が Pool の Command と SwapChain の Graphics Queue を使って提出する
Result<bool> DX12MainFrameGraph::execute(std::uint32_t a_frameIndex, ICommandPool& a_commandPool,
                                         std::function<bool()> a_shouldCancel)
{
    if (!m_graph || !m_swapChain || !m_swapChain->graphics_queue())
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.execute"});
    }
    auto result = m_graph->execute(a_frameIndex, a_commandPool, *m_swapChain->graphics_queue(), *this,
                                   std::move(a_shouldCancel));
    if (!result.has_value())
    {
        return Result<bool>::failure(*result.try_error());
    }
    return Result<bool>::success(static_cast<bool>(*result.try_value()));
}

/// @brief 異なる Queue の Pass を依存順に提出し、最終 Graphics 完了点へ集約する
Result<bool> DX12MainFrameGraph::execute(std::uint32_t a_frameIndex, ICommandPool& a_commandPool,
                                         IQueuePool& a_queuePool, std::function<bool()> a_shouldCancel)
{
    if (!m_graph || !m_graph->plan() || !m_swapChain || !m_swapChain->graphics_queue() ||
        a_frameIndex >= m_poolLeases.size())
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.execute_queues"});
    }
    auto enabledResult = m_graph->validate_enabled();
    if (!enabledResult.has_value())
    {
        return Result<bool>::failure(*enabledResult.try_error());
    }
    const auto& passes = m_graph->plan()->passes();
    if (std::all_of(passes.begin(), passes.end(), [](const auto& a_pass)
                    { return a_pass.queue == QueueType::Graphics; }))
    {
        return execute(a_frameIndex, a_commandPool, std::move(a_shouldCancel));
    }
    if (a_shouldCancel && a_shouldCancel())
    {
        return Result<bool>::success(false);
    }
    std::shared_ptr<BatchCompletion> completion;
    std::vector<commandCompletion> submissions;
    std::vector<IQueueContext*> queueByPass;
    std::vector<std::uint64_t> fenceByPass;
    try
    {
        completion = std::make_shared<BatchCompletion>();
        submissions.reserve(passes.size());
        queueByPass.resize(passes.size(), nullptr);
        fenceByPass.resize(passes.size(), 0);
    }
    catch (const std::bad_alloc&)
    {
        return Result<bool>::failure({ErrorCategory::PlatformFailure,
                                      "DX12MainFrameGraph.execute_queues.allocation"});
    }
    auto prepareResult = prepare_frame(a_frameIndex);
    if (!prepareResult.has_value())
    {
        return Result<bool>::failure(*prepareResult.try_error());
    }
    auto fail = [this, a_frameIndex, &submissions](Error a_error) -> Result<bool>
    {
        for (auto& submission : submissions)
        {
            auto waitResult = submission->wait();
            if (!waitResult.has_value())
            {
                return Result<bool>::failure(*waitResult.try_error());
            }
        }
        clear_frame(a_frameIndex);
        return Result<bool>::failure(std::move(a_error));
    };
    queueLease computeQueue;
    queueLease copyQueue;
    std::uint64_t latestComputeFence = 0;
    std::uint64_t latestCopyFence = 0;
    auto* graphicsQueue = m_swapChain->graphics_queue();
    for (std::size_t index = 0; index < passes.size(); ++index)
    {
        const auto type = passes[index].queue;
        IQueueContext* queue = graphicsQueue;
        if (type == QueueType::Compute)
        {
            if (!computeQueue)
            {
                auto queueResult = a_queuePool.acquire(type);
                if (!queueResult.has_value())
                {
                    return fail(*queueResult.try_error());
                }
                computeQueue = queueResult.take_value();
            }
            queue = computeQueue.get();
        }
        else if (type == QueueType::Copy)
        {
            if (!copyQueue)
            {
                auto queueResult = a_queuePool.acquire(type);
                if (!queueResult.has_value())
                {
                    return fail(*queueResult.try_error());
                }
                copyQueue = queueResult.take_value();
            }
            queue = copyQueue.get();
        }
        for (const auto dependency : passes[index].dependencies)
        {
            if (dependency.index >= queueByPass.size() || !queueByPass[dependency.index] ||
                fenceByPass[dependency.index] == 0)
            {
                return fail({ErrorCategory::InvalidState, "DX12MainFrameGraph.execute_queues.dependency"});
            }
            auto* producerQueue = queueByPass[dependency.index];
            if (producerQueue != queue)
            {
                auto waitResult = queue->wait_for_queue(*producerQueue, fenceByPass[dependency.index]);
                if (!waitResult.has_value())
                {
                    return fail(*waitResult.try_error());
                }
            }
        }
        auto commandResult = a_commandPool.acquire(type);
        if (!commandResult.has_value())
        {
            return fail(*commandResult.try_error());
        }
        auto command = commandResult.take_value();
        auto* context = dynamic_cast<DX12GpuCommandContext*>(command.get());
        if (!context)
        {
            return fail({ErrorCategory::InvalidState, "DX12MainFrameGraph.execute_queues.context"});
        }
        auto recordResult = record_range(a_frameIndex, *context, index, 1, false);
        if (!recordResult.has_value())
        {
            return fail(*recordResult.try_error());
        }
        auto closeResult = command->close();
        if (!closeResult.has_value())
        {
            return fail(*closeResult.try_error());
        }
        auto submitResult = a_commandPool.submit(*queue, *command);
        if (!submitResult.has_value())
        {
            // 提出結果が不明な場合は Lease を保持し、GPU Resource の早期破棄を防ぐ
            return Result<bool>::failure(*submitResult.try_error());
        }
        submissions.push_back(submitResult.take_value());
        auto signalResult = queue->signal();
        if (!signalResult.has_value())
        {
            return fail(*signalResult.try_error());
        }
        queueByPass[passes[index].handle.index] = queue;
        fenceByPass[passes[index].handle.index] = signalResult.take_value();
        if (type == QueueType::Compute)
        {
            latestComputeFence = fenceByPass[passes[index].handle.index];
        }
        else if (type == QueueType::Copy)
        {
            latestCopyFence = fenceByPass[passes[index].handle.index];
        }
    }
    if (latestComputeFence != 0)
    {
        auto waitResult = graphicsQueue->wait_for_queue(*computeQueue, latestComputeFence);
        if (!waitResult.has_value())
        {
            return fail(*waitResult.try_error());
        }
    }
    if (latestCopyFence != 0)
    {
        auto waitResult = graphicsQueue->wait_for_queue(*copyQueue, latestCopyFence);
        if (!waitResult.has_value())
        {
            return fail(*waitResult.try_error());
        }
    }
    auto finalCommandResult = a_commandPool.acquire(QueueType::Graphics);
    if (!finalCommandResult.has_value())
    {
        return fail(*finalCommandResult.try_error());
    }
    auto finalCommand = finalCommandResult.take_value();
    auto* finalContext = dynamic_cast<DX12GpuCommandContext*>(finalCommand.get());
    if (!finalContext)
    {
        return fail({ErrorCategory::InvalidState, "DX12MainFrameGraph.execute_queues.final_context"});
    }
    auto finalRecordResult = record_range(a_frameIndex, *finalContext, passes.size(), 0, true);
    if (!finalRecordResult.has_value())
    {
        return fail(*finalRecordResult.try_error());
    }
    auto finalCloseResult = finalCommand->close();
    if (!finalCloseResult.has_value())
    {
        return fail(*finalCloseResult.try_error());
    }
    auto finalSubmitResult = a_commandPool.submit(*graphicsQueue, *finalCommand);
    if (!finalSubmitResult.has_value())
    {
        return Result<bool>::failure(*finalSubmitResult.try_error());
    }
    completion->set(finalSubmitResult.take_value());
    auto markResult = mark_submitted(a_frameIndex, completion);
    if (!markResult.has_value())
    {
        return Result<bool>::failure(*markResult.try_error());
    }
    return Result<bool>::success(true);
}

/// @brief 枠の再利用と停止の待機に使う完了点を登録する
Result<void> DX12MainFrameGraph::mark_submitted(std::uint32_t a_frameIndex,
                                                 std::shared_ptr<ICommandCompletion> a_completion)
{
    if (!m_frames || a_frameIndex >= m_poolLeases.size() || !a_completion)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.mark_submitted"});
    }
    auto frameResult = m_frames->mark_submitted(a_frameIndex, a_completion);
    if (!frameResult.has_value())
    {
        auto waitResult = a_completion->wait();
        if (!waitResult.has_value())
        {
            return waitResult;
        }
        clear_frame(a_frameIndex);
        return frameResult;
    }
    for (auto& lease : m_poolLeases[a_frameIndex])
    {
        auto markResult = lease->mark_submitted(a_completion);
        if (!markResult.has_value())
        {
            auto waitResult = a_completion->wait();
            if (!waitResult.has_value())
            {
                return waitResult;
            }
            clear_frame(a_frameIndex);
            return markResult;
        }
    }
    clear_frame(a_frameIndex);
    return Result<void>::success();
}

/// @brief Command List の未提出が確定した枠を再記録可能にする
void DX12MainFrameGraph::discard_unsubmitted(std::uint32_t a_frameIndex) noexcept
{
    if (a_frameIndex < m_poolLeases.size())
    {
        clear_frame(a_frameIndex);
    }
}

/// @brief 検証済み Plan を返す
const FrameGraphPlan& DX12MainFrameGraph::plan() const noexcept
{
    return *m_graph->plan();
}

/// @brief GPU 完了後に枠の Resource を解放する
Result<void> DX12MainFrameGraph::shutdown()
{
    if (!m_frames)
    {
        return Result<void>::success();
    }
    for (std::size_t index = 0; index < m_poolLeases.size(); ++index)
    {
        if (m_isPrepared[index] || !m_poolLeases[index].empty())
        {
            return Result<void>::failure({ErrorCategory::InvalidState,
                                          "DX12MainFrameGraph.shutdown.pending_submission"});
        }
    }
    auto result = m_frames->shutdown();
    if (!result.has_value())
    {
        return result;
    }
    m_frames.reset();
    m_fullscreenTriangle.reset();
    m_poolLeases.clear();
    m_externalBindings.clear();
    m_isPrepared.clear();
    m_graph.reset();
    m_swapChain = nullptr;
    return Result<void>::success();
}
} // namespace cue::dx12
