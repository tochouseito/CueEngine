#include <DX12/DX12MainFrameGraph.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <DX12/DX12CommandPool.h>
#include <DX12/DX12FrameGraphContext.h>
#include <DX12/DX12FrameGraphExecutor.h>
#include <DX12/DX12FrameGraphFrames.h>
#include <DX12/DX12FrameGraphResources.h>
#include <DX12/DX12GpuResource.h>
#include <DX12/DX12PipelineManager.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <Passes/MainFrameGraph.h>
#include <Platform/Diagnostics.h>
#include <RHI/CommandCompletion.h>

namespace cue::dx12
{
/// @brief Factory が成功するまで所有状態を公開しない
DX12MainFrameGraph::DX12MainFrameGraph(CreateToken) noexcept
{
}

/// @brief 旧 Pass 契約に従って Clear、追加 Pass、表示 Pass を組み立てる
Result<std::unique_ptr<DX12MainFrameGraph>> DX12MainFrameGraph::create(const DX12ResourceContext &a_resources,
                                                                       DX12SwapChain &a_swapChain,
                                                                       DX12MainFrameGraphConfig a_config)
{
    using GraphResult = Result<std::unique_ptr<DX12MainFrameGraph>>;
    // Backend の生成基盤を借用し、描画に必要な Device、Queue と枠数を検証する
    auto &device = a_resources.get_render_device();
    // 0 番の Back Buffer は形状と Format の取得に使い、実際の描画先は Frame ごとに選ぶ
    auto *backBuffer = a_swapChain.back_buffer(0);
    if (!device.device() || !backBuffer || !a_swapChain.graphics_queue() || a_config.frameCount == 0)
    {
        return GraphResult::failure({ErrorCategory::InvalidArgument, "DX12MainFrameGraph.create"});
    }
    // 表示用 Pipeline と Resource 生成が対応する二次元 Color Texture に限定する
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
    // FinalColorTexture の生成仕様を表示先に合わせる。GPU Texture の実体化は後段で行う
    GpuTexture2DDesc colorDesc{static_cast<std::uint32_t>(nativeDesc.Width),
                               static_cast<std::uint32_t>(nativeDesc.Height)};
    colorDesc.format =
        nativeDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? GpuTextureFormat::Rgba8Unorm : GpuTextureFormat::Bgra8Unorm;
    colorDesc.isRenderTarget = true;
    colorDesc.clearColor = a_config.clearColor;
    // 抽象層で FinalColor と外部 BackBuffer を宣言し、Clear、追加 Pass、表示 Pass の Plan を構築する
    // 表示 Pass は Back Buffer への描画を担い、SwapChain の Present は Graph 提出後に Host が行う
    auto &pipelines = a_resources.get_pipeline_manager();
    if (pipelines.device() != device.device())
    {
        return GraphResult::failure({ErrorCategory::InvalidArgument, "DX12MainFrameGraph.create.pipeline_device"});
    }
    try
    {
        // Graph が消費する設定とは別に、Resize で再実行する Callback と Factory を保持する
        const bool hasOneShotDisplayPass = a_config.displayPass != nullptr;
        MainFrameGraphConfig compositionConfig;
        compositionConfig.configure = a_config.configure;
        compositionConfig.displayPass = std::move(a_config.displayPass);
        compositionConfig.displayPassFactory = a_config.displayPassFactory;
        auto compositionResult = create_main_frame_graph(colorDesc, {pipelines}, std::move(compositionConfig));
        if (!compositionResult.has_value())
        {
            return GraphResult::failure(*compositionResult.try_error());
        }
        auto composition = compositionResult.take_value();
        // Plan に従って枠ごとの一時 Resource と RTV／SRV を生成する
        // BackBuffer の RTV は SwapChain から借用するため、Graph 側で重複生成しない
        auto framesResult = DX12FrameGraphFrames::create(a_resources, *composition.graph->plan(),
                                                         {a_config.frameCount, composition.backBuffer});
        if (!framesResult.has_value())
        {
            return GraphResult::failure(*framesResult.try_error());
        }
        // 配列確保を先に完了させ、確保失敗時に不完全な管理状態へ Resource の所有権を移さない
        auto result = std::make_unique<DX12MainFrameGraph>(CreateToken{});
        result->m_poolLeases.resize(a_config.frameCount);
        result->m_externalBindings.resize(a_config.frameCount);
        result->m_isPrepared.resize(a_config.frameCount, false);
        // Graph は Build で生成した Pipeline の解放責任を持ち、Manager と SwapChain は非所有で参照する
        result->m_graph = std::move(composition.graph);
        result->m_backBuffer = composition.backBuffer;
        result->m_frames = framesResult.take_value();
        result->m_pipelineManager = &pipelines;
        result->m_swapChain = &a_swapChain;
        result->m_configure = std::move(a_config.configure);
        result->m_displayPassFactory = std::move(a_config.displayPassFactory);
        result->m_clearColor = a_config.clearColor;
        result->m_frameCount = a_config.frameCount;
        result->m_hasOneShotDisplayPass = hasOneShotDisplayPass;
        auto cacheResult = result->build_execution_cache();
        if (!cacheResult.has_value())
            return GraphResult::failure(*cacheResult.try_error());
        return GraphResult::success(std::move(result));
    }
    catch (const std::bad_alloc &)
    {
        // この生成段階のメモリ確保失敗を Result に変換し、途中生成物は所有者の破棄で回収する
        return GraphResult::failure({ErrorCategory::PlatformFailure, "DX12MainFrameGraph.create.allocation"});
    }
}

/// @brief 明示停止されなかった枠も GPU 完了を待って破棄する
DX12MainFrameGraph::~DX12MainFrameGraph()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_log_error("DX12MainFrameGraph.shutdown", *result.try_error(), LogLevel::Fatal);
        std::terminate();
    }
}

/// @brief Barrier 計画と各 Pass の execute を同じ Graphics List に記録する
Result<void> DX12MainFrameGraph::record(std::uint32_t a_frameIndex, DX12GpuCommandContext &a_context)
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
    if (!m_graph || !m_graph->plan() || !m_frames || !m_swapChain || a_frameIndex >= m_frames->frame_count() ||
        m_isPrepared[a_frameIndex] || !m_poolLeases[a_frameIndex].empty())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.prepare_frame"});
    }
    auto *backBuffer = m_swapChain->back_buffer(m_swapChain->current_index());
    if (!backBuffer || !m_frames->graph_resources(a_frameIndex))
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.prepare_frame.resources"});
    }
    const auto waitStart = std::chrono::steady_clock::now();
    auto beginResult = m_frames->begin_frame(a_frameIndex);
    {
        std::lock_guard lock(m_performanceMutex);
        m_frameWaitTimings.add(std::chrono::steady_clock::now() - waitStart);
        if (beginResult.has_value() && m_frames->has_gpu_sample(a_frameIndex))
        {
            try
            {
                const auto timings = m_frames->gpu_timings(a_frameIndex);
                m_gpuPasses.assign(timings.begin(), timings.end());
                ++m_completedGpuFrames;
            }
            catch (const std::bad_alloc &)
            {
                return Result<void>::failure(
                    {ErrorCategory::PlatformFailure, "DX12MainFrameGraph.performance.allocation"});
            }
        }
    }
    if (!beginResult.has_value())
    {
        return beginResult;
    }
    m_initialRecorded[a_frameIndex] = false;
    m_frameRecordDurations[a_frameIndex] = {};
    try
    {
        auto &external = m_externalBindings[a_frameIndex];
        auto &leases = m_poolLeases[a_frameIndex];
        external.clear();
        leases.clear();
        const auto fail = [this, a_frameIndex](Error a_error)
        {
            clear_frame(a_frameIndex);
            return Result<void>::failure(std::move(a_error));
        };
        external.push_back({m_backBuffer, backBuffer});
        for (const auto index : m_poolResourceIndices)
        {
            const auto &plannedResource = m_graph->plan()->resources()[index];
            const bool needsExclusive = plannedResource.isWritten || plannedResource.needsExclusiveStateAccess;
            auto leaseResult = plannedResource.pool->acquire_wait(
                plannedResource.poolHandle, needsExclusive ? GpuResourceAccess::Write : GpuResourceAccess::Read);
            if (!leaseResult.has_value())
            {
                return fail(*leaseResult.try_error());
            }
            auto lease = leaseResult.take_value();
            auto *resource = dynamic_cast<DX12GpuResource *>(lease->resource());
            if (!resource || !resource->resource())
            {
                return fail({ErrorCategory::InvalidState, "DX12MainFrameGraph.prepare_frame.pool_resource"});
            }
            external.push_back({plannedResource.handle, resource->resource()});
            leases.push_back(std::move(lease));
        }
        auto backRtvResult = m_swapChain->rtv(m_swapChain->current_index());
        if (!backRtvResult.has_value())
        {
            return fail(*backRtvResult.try_error());
        }
        auto viewsResult =
            m_frames->prepare_imported_views(a_frameIndex, *m_graph->plan(), external, *backRtvResult.try_value());
        if (!viewsResult.has_value())
        {
            return fail(*viewsResult.try_error());
        }
        m_isPrepared[a_frameIndex] = true;
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        clear_frame(a_frameIndex);
        return Result<void>::failure({ErrorCategory::PlatformFailure, "DX12MainFrameGraph.prepare_frame.allocation"});
    }
}

/// @brief 準備済み Binding を使い、各 Queue に割り当てた範囲だけを記録する
Result<void> DX12MainFrameGraph::record_range(std::uint32_t a_frameIndex, DX12GpuCommandContext &a_context,
                                              std::size_t a_firstPass, std::size_t a_passCount, bool a_includeFinal)
{
    if (!m_graph || !m_graph->plan() || !m_frames || a_frameIndex >= m_isPrepared.size() ||
        !m_isPrepared[a_frameIndex] || a_context.state() != CommandState::Recording ||
        a_firstPass > m_graph->plan()->passes().size() || a_passCount > m_graph->plan()->passes().size() - a_firstPass)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.record_range"});
    }
    auto *resources = m_frames->graph_resources(a_frameIndex);
    if (!resources)
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.record_range.resources"});
    const auto start = std::chrono::steady_clock::now();
    auto &prepared = m_prepared[a_frameIndex];
    if (prepared.planId == 0)
    {
        auto preparation = DX12FrameGraphExecutor::prepare(*m_graph->plan(), *resources,
                                                           m_externalBindings[a_frameIndex], a_context, prepared);
        if (!preparation.has_value())
            return preparation;
    }
    m_recordingContexts[a_frameIndex] = &a_context;
    const auto callbacks = std::span<const dx12FrameGraphPassCallback>(m_callbacks[a_frameIndex]);
    auto result = DX12FrameGraphExecutor::record_prepared(
        *m_graph->plan(), *resources, prepared, callbacks.subspan(a_firstPass, a_passCount), a_context, a_firstPass,
        a_passCount, a_includeFinal,
        !m_initialRecorded[a_frameIndex] && a_firstPass == 0 && a_context.type() == QueueType::Graphics);
    m_recordingContexts[a_frameIndex] = nullptr;
    if (result.has_value() && a_firstPass == 0 && a_context.type() == QueueType::Graphics)
        m_initialRecorded[a_frameIndex] = true;
    m_frameRecordDurations[a_frameIndex] += std::chrono::steady_clock::now() - start;
    return result;
}

/// @brief Build 済み Pass 対応と Callback を一度だけ構築する
Result<void> DX12MainFrameGraph::build_execution_cache()
{
    try
    {
        m_poolResourceIndices.clear();
        for (const auto &resource : m_graph->plan()->resources())
            if (resource.pool)
                m_poolResourceIndices.push_back(resource.handle.index);
        const auto &passes = m_graph->plan()->passes();
        m_prepared.resize(m_frameCount);
        m_callbacks.resize(m_frameCount);
        m_recordingContexts.resize(m_frameCount, nullptr);
        m_initialRecorded.resize(m_frameCount, false);
        m_frameRecordDurations.resize(m_frameCount);
        m_queueByPass.resize(m_frameCount);
        m_fenceByPass.resize(m_frameCount);
        for (std::uint32_t frame = 0; frame < m_frameCount; ++frame)
        {
            m_externalBindings[frame].reserve(m_poolResourceIndices.size() + 1);
            m_poolLeases[frame].reserve(m_poolResourceIndices.size());
            m_queueByPass[frame].resize(passes.size(), nullptr);
            m_fenceByPass[frame].resize(passes.size(), 0);
            auto &callbacks = m_callbacks[frame];
            callbacks.clear();
            callbacks.reserve(passes.size());
            for (std::size_t index = 0; index < passes.size(); ++index)
            {
                auto *pass = m_graph->pass(passes[index].handle);
                if (!pass)
                    return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.cache.pass"});
                callbacks.emplace_back(
                    [this, pass, frame, index](ID3D12GraphicsCommandList &a_list,
                                               const DX12FrameGraphPassContext &a_resources) -> Result<void>
                    {
                        // Callback が後続 Pass の状態を変えても、その Pass の記録前に拒否する
                        if (!pass->is_enabled())
                            return Result<void>::failure(
                                {ErrorCategory::InvalidState, "DX12MainFrameGraph.record.disabled_pass"});
                        DX12FrameGraphContext context({.width = m_graph->width(),
                                                       .height = m_graph->height(),
                                                       .frameIndex = frame,
                                                       .command = *m_recordingContexts[frame],
                                                       .resources = a_resources,
                                                       .plan = *m_graph->plan(),
                                                       .pass = m_graph->plan()->passes()[index],
                                                       .frames = *m_frames,
                                                       .pipelines = *m_pipelineManager});
                        m_frames->begin_pass(frame, index, a_list);
                        auto result = pass->execute(context);
                        m_frames->end_pass(frame, index, a_list);
                        return result;
                    });
            }
        }
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "DX12MainFrameGraph.cache.allocation"});
    }
}

/// @brief Render の集計を Lock 内で複写し、借用可変データを公開しない
MainFrameGraphPerformance DX12MainFrameGraph::performance() const
{
    std::lock_guard lock(m_performanceMutex);
    return {m_recordTimings.statistics(), m_frameWaitTimings.statistics(), {}, m_gpuPasses, m_completedGpuFrames};
}

/// @brief GPU が終わった枠の Pool Lease と Native Binding を返す
void DX12MainFrameGraph::clear_frame(std::uint32_t a_frameIndex) noexcept
{
    m_poolLeases[a_frameIndex].clear();
    m_externalBindings[a_frameIndex].clear();
    m_isPrepared[a_frameIndex] = false;
    m_prepared[a_frameIndex].planId = 0;
}

/// @brief Pool が返した基底 Context を DX12 記録器へ渡す
Result<void> DX12MainFrameGraph::record(std::uint32_t a_frameIndex, ICommandContext &a_context)
{
    auto *context = dynamic_cast<DX12GpuCommandContext *>(&a_context);
    if (!context)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12MainFrameGraph.record.context"});
    }
    return record(a_frameIndex, *context);
}

/// @brief Graph が Pool の Command と SwapChain の Graphics Queue を使って提出する
Result<bool> DX12MainFrameGraph::execute_graphics(std::uint32_t a_frameIndex, ICommandPool &a_commandPool,
                                                  std::function<bool()> a_shouldCancel)
{
    if (!m_graph || !m_swapChain || !m_swapChain->graphics_queue())
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.execute"});
    }
    auto frequency = m_swapChain->graphics_queue()->get_timestamp_frequency();
    if (!frequency.has_value())
        return Result<bool>::failure(*frequency.try_error());
    m_frames->set_timestamp_frequency(a_frameIndex, QueueType::Graphics, *frequency.try_value());
    auto result =
        m_graph->execute(a_frameIndex, a_commandPool, *m_swapChain->graphics_queue(), *this, std::move(a_shouldCancel));
    if (!result.has_value())
    {
        return Result<bool>::failure(*result.try_error());
    }
    return Result<bool>::success(static_cast<bool>(*result.try_value()));
}

/// @brief 異なる Queue の Pass を依存順に提出し、最終 Graphics 完了点へ集約する
Result<bool> DX12MainFrameGraph::execute(std::uint32_t a_frameIndex, const DX12ExecutionContext &a_execution,
                                         std::function<bool()> a_shouldCancel)
{
    auto &commandPool = a_execution.get_command_pool();
    auto &queuePool = a_execution.get_queue_pool();
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
    const auto &passes = m_graph->plan()->passes();
    if (std::all_of(passes.begin(), passes.end(),
                    [](const auto &a_pass) { return a_pass.queue == QueueType::Graphics; }))
    {
        return execute_graphics(a_frameIndex, commandPool, std::move(a_shouldCancel));
    }
    if (a_shouldCancel && a_shouldCancel())
    {
        return Result<bool>::success(false);
    }
    std::shared_ptr<SharedCommandCompletion> completion;
    std::vector<commandCompletion> submissions;
    auto &queueByPass = m_queueByPass[a_frameIndex];
    auto &fenceByPass = m_fenceByPass[a_frameIndex];
    std::fill(queueByPass.begin(), queueByPass.end(), nullptr);
    std::fill(fenceByPass.begin(), fenceByPass.end(), 0);
    try
    {
        completion = std::make_shared<SharedCommandCompletion>();
        submissions.reserve(passes.size() + 1);
    }
    catch (const std::bad_alloc &)
    {
        return Result<bool>::failure({ErrorCategory::PlatformFailure, "DX12MainFrameGraph.execute_queues.allocation"});
    }
    auto prepareResult = prepare_frame(a_frameIndex);
    if (!prepareResult.has_value())
    {
        return Result<bool>::failure(*prepareResult.try_error());
    }
    auto fail = [this, a_frameIndex, &submissions](Error a_error) -> Result<bool>
    {
        for (auto &submission : submissions)
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
    auto *graphicsQueue = m_swapChain->graphics_queue();
    auto graphicsFrequency = graphicsQueue->get_timestamp_frequency();
    if (!graphicsFrequency.has_value())
        return fail(*graphicsFrequency.try_error());
    m_frames->set_timestamp_frequency(a_frameIndex, QueueType::Graphics, *graphicsFrequency.try_value());
    std::uint64_t initialFence = 0;
    if (!m_graph->plan()->initial_barriers().empty())
    {
        auto initialCommandResult = commandPool.acquire(QueueType::Graphics);
        if (!initialCommandResult.has_value())
            return fail(*initialCommandResult.try_error());
        auto initialCommand = initialCommandResult.take_value();
        auto *initialContext = dynamic_cast<DX12GpuCommandContext *>(initialCommand.get());
        if (!initialContext)
            return fail({ErrorCategory::InvalidState, "DX12MainFrameGraph.initial_context"});
        auto recorded = record_range(a_frameIndex, *initialContext, 0, 0, false);
        if (!recorded.has_value())
            return fail(*recorded.try_error());
        auto closed = initialCommand->close();
        if (!closed.has_value())
            return fail(*closed.try_error());
        auto submitted = commandPool.submit(*graphicsQueue, *initialCommand);
        if (!submitted.has_value())
            return Result<bool>::failure(*submitted.try_error());
        auto token = submitted.take_value();
        initialFence = token->fence_value();
        const auto initialIdentity = token->queue_identity();
        submissions.push_back(std::move(token));
        if (initialFence == 0 || initialIdentity != graphicsQueue->identity())
            return fail({ErrorCategory::InvalidState, "DX12MainFrameGraph.initial_fence"});
    }
    for (std::size_t index = 0; index < passes.size();)
    {
        const auto type = passes[index].queue;
        IQueueContext *queue = graphicsQueue;
        if (type == QueueType::Compute)
        {
            if (!computeQueue)
            {
                auto queueResult = queuePool.acquire(type);
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
                auto queueResult = queuePool.acquire(type);
                if (!queueResult.has_value())
                {
                    return fail(*queueResult.try_error());
                }
                copyQueue = queueResult.take_value();
            }
            queue = copyQueue.get();
        }
        std::size_t end = index + 1;
        while (end < passes.size() && passes[end].queue == type)
            ++end;
        if (m_frames->timestamp_supported(type))
        {
            auto frequency = queue->get_timestamp_frequency();
            if (!frequency.has_value())
                return fail(*frequency.try_error());
            m_frames->set_timestamp_frequency(a_frameIndex, type, *frequency.try_value());
        }
        if (initialFence != 0 && queue != graphicsQueue)
        {
            auto waitResult = queue->wait_for_queue(*graphicsQueue, initialFence);
            if (!waitResult.has_value())
                return fail(*waitResult.try_error());
        }
        for (std::size_t passIndex = index; passIndex < end; ++passIndex)
            for (const auto dependency : passes[passIndex].dependencies)
            {
                const bool isInBatch =
                    std::any_of(passes.begin() + index, passes.begin() + passIndex,
                                [dependency](const auto &a_pass) { return a_pass.handle.index == dependency.index; });
                if (isInBatch)
                    continue;
                if (dependency.index >= queueByPass.size() || !queueByPass[dependency.index] ||
                    fenceByPass[dependency.index] == 0)
                {
                    return fail({ErrorCategory::InvalidState, "DX12MainFrameGraph.execute_queues.dependency"});
                }
                auto *producerQueue = queueByPass[dependency.index];
                if (producerQueue != queue)
                {
                    auto waitResult = queue->wait_for_queue(*producerQueue, fenceByPass[dependency.index]);
                    if (!waitResult.has_value())
                    {
                        return fail(*waitResult.try_error());
                    }
                }
            }
        auto commandResult = commandPool.acquire(type);
        if (!commandResult.has_value())
        {
            return fail(*commandResult.try_error());
        }
        auto command = commandResult.take_value();
        auto *context = dynamic_cast<DX12GpuCommandContext *>(command.get());
        if (!context)
        {
            return fail({ErrorCategory::InvalidState, "DX12MainFrameGraph.execute_queues.context"});
        }
        auto recordResult = record_range(a_frameIndex, *context, index, end - index, false);
        if (!recordResult.has_value())
        {
            return fail(*recordResult.try_error());
        }
        auto closeResult = command->close();
        if (!closeResult.has_value())
        {
            return fail(*closeResult.try_error());
        }
        auto submitResult = commandPool.submit(*queue, *command);
        if (!submitResult.has_value())
        {
            // 提出結果が不明な場合は Lease を保持し、GPU Resource の早期破棄を防ぐ
            return Result<bool>::failure(*submitResult.try_error());
        }
        auto token = submitResult.take_value();
        const auto fence = token->fence_value();
        if (fence == 0 || token->queue_identity() != queue->identity())
        {
            submissions.push_back(std::move(token));
            return fail({ErrorCategory::InvalidState, "DX12MainFrameGraph.submit_fence"});
        }
        submissions.push_back(std::move(token));
        for (std::size_t passIndex = index; passIndex < end; ++passIndex)
        {
            queueByPass[passes[passIndex].handle.index] = queue;
            fenceByPass[passes[passIndex].handle.index] = fence;
        }
        if (type == QueueType::Compute)
            latestComputeFence = fence;
        else if (type == QueueType::Copy)
            latestCopyFence = fence;
        index = end;
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
    auto finalCommandResult = commandPool.acquire(QueueType::Graphics);
    if (!finalCommandResult.has_value())
    {
        return fail(*finalCommandResult.try_error());
    }
    auto finalCommand = finalCommandResult.take_value();
    auto *finalContext = dynamic_cast<DX12GpuCommandContext *>(finalCommand.get());
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
    auto finalSubmitResult = commandPool.submit(*graphicsQueue, *finalCommand);
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
    for (auto &lease : m_poolLeases[a_frameIndex])
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
    {
        std::lock_guard lock(m_performanceMutex);
        m_recordTimings.add(m_frameRecordDurations[a_frameIndex]);
    }
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
const FrameGraphPlan &DX12MainFrameGraph::plan() const noexcept
{
    return *m_graph->plan();
}

/// @brief 旧 Graph の GPU 利用を終えてから BackBuffer を変更し、新しい Plan と物理枠を作る
Result<void> DX12MainFrameGraph::resize(const DX12ResourceContext &a_resources, std::uint32_t a_width,
                                        std::uint32_t a_height)
{
    if (a_width == 0 || a_height == 0 || !m_swapChain || m_frameCount == 0 ||
        (m_pipelineManager && m_pipelineManager != &a_resources.get_pipeline_manager()))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12MainFrameGraph.resize"});
    }
    if (m_graph && m_frames && m_graph->width() == a_width && m_graph->height() == a_height)
    {
        return Result<void>::success();
    }
    // 一回限りの表示 Pass は旧 Graph が所有する。Factory がなければ破棄前に拒否する
    if (m_hasOneShotDisplayPass && !m_displayPassFactory)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.resize.display_factory"});
    }
    for (std::size_t index = 0; index < m_poolLeases.size(); ++index)
    {
        if (m_isPrepared[index] || !m_poolLeases[index].empty())
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DX12MainFrameGraph.resize.pending_submission"});
        }
    }

    DX12MainFrameGraphConfig rebuildConfig;
    rebuildConfig.frameCount = m_frameCount;
    rebuildConfig.clearColor = m_clearColor;
    try
    {
        // Callback の複製失敗は旧 Graph と BackBuffer に手を触れる前に返す
        rebuildConfig.configure = m_configure;
        rebuildConfig.displayPassFactory = m_displayPassFactory;
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "DX12MainFrameGraph.resize.config_allocation"});
    }

    auto *swapChain = m_swapChain;
    if (m_frames)
    {
        auto stopped = shutdown();
        if (!stopped.has_value())
        {
            return stopped;
        }
        // 再構築が失敗しても同じ SwapChain に対する再試行入口を維持する
        m_swapChain = swapChain;
    }
    // SwapChain 自身が同一サイズの完成状態を省略し、途中取得失敗の部分状態は再試行する
    auto resized = swapChain->resize(a_width, a_height);
    if (!resized.has_value())
    {
        return resized;
    }
    auto rebuiltResult = create(a_resources, *swapChain, std::move(rebuildConfig));
    if (!rebuiltResult.has_value())
    {
        return Result<void>::failure(*rebuiltResult.try_error());
    }
    auto rebuilt = rebuiltResult.take_value();
    m_graph = std::move(rebuilt->m_graph);
    m_backBuffer = rebuilt->m_backBuffer;
    m_frames = std::move(rebuilt->m_frames);
    m_pipelineManager = rebuilt->m_pipelineManager;
    m_poolLeases = std::move(rebuilt->m_poolLeases);
    m_externalBindings = std::move(rebuilt->m_externalBindings);
    m_isPrepared = std::move(rebuilt->m_isPrepared);
    m_swapChain = swapChain;
    return build_execution_cache();
}

/// @brief GPU 完了後に枠の Resource を解放する
Result<void> DX12MainFrameGraph::shutdown()
{
    if (!m_frames)
    {
        m_swapChain = nullptr;
        return Result<void>::success();
    }
    for (std::size_t index = 0; index < m_poolLeases.size(); ++index)
    {
        if (m_isPrepared[index] || !m_poolLeases[index].empty())
        {
            return Result<void>::failure(
                {ErrorCategory::InvalidState, "DX12MainFrameGraph.shutdown.pending_submission"});
        }
    }
    auto result = m_frames->shutdown();
    if (!result.has_value())
    {
        return result;
    }
    m_frames.reset();
    m_pipelineManager = nullptr;
    m_poolLeases.clear();
    m_poolResourceIndices.clear();
    m_externalBindings.clear();
    m_isPrepared.clear();
    m_prepared.clear();
    m_callbacks.clear();
    m_recordingContexts.clear();
    m_initialRecorded.clear();
    m_frameRecordDurations.clear();
    m_queueByPass.clear();
    m_fenceByPass.clear();
    m_graph.reset();
    m_swapChain = nullptr;
    return Result<void>::success();
}
} // namespace cue::dx12
