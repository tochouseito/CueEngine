#include <Cue/Renderer/DX12/DX12Backend.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <Cue/Renderer/FrameGraph/FrameGraphRuntime.h>

#include "DX12RenderDevice.h"
#include "DescriptorAllocator.h"
#include "DX12BufferManager.h"
#include "DX12CommandPool.h"
#include "DX12CommandRecorder.h"
#include "DX12FramePasses.h"
#include "DX12GraphExecutor.h"
#include "DX12GraphResourceBindings.h"
#include "DX12PipelineCache.h"
#include "DX12PipelineManager.h"
#include "DX12SwapChain.h"
#include "DX12GpuCommandQueue.h"
#include "DX12QueuePool.h"
#include "DX12ResourcePool.h"
#include "DX12StaticMeshPool.h"
#include "DX12SurfacePool.h"
#include "DX12TrianglePass.h"
#include "DX12TextureManager.h"
#include "DX12ViewManager.h"
#include "ResourceLeakChecker.h"

namespace cue
{
class DX12Backend::State final
{
public:
    /// @brief GPU Submit の残りがあれば Owner の破棄前に完了を待つ
    ~State()
    {
        if (queues)
        {
            [[maybe_unused]] auto result = queues->wait_idle();
        }
    }

    /// @brief GPU 完了後に旧 Back Buffer 参照を外して Surface を更新する
    [[nodiscard]] Result<void> resize(WindowSize a_size)
    {
        if (!resizePending && a_size.width == presentation->size().width &&
            a_size.height == presentation->size().height)
        {
            return Result<void>::success();
        }

        // Frame Context の List と Presentation の Buffer は GPU 完了前に解放しない
        auto idleResult = queues->wait_idle();
        if (!idleResult.has_value())
        {
            return idleResult;
        }
        // 途中失敗後も同じ Size で再試行し、Command List が欠けた状態を成功扱いしない
        resizePending = true;
        commands->release_for_resize();
        computeCommands->release_for_resize();
        copyCommands->release_for_resize();
        auto resizeResult = presentation->resize(*device, a_size);
        if (!resizeResult.has_value())
        {
            return resizeResult;
        }
        auto surfaceResult = surfaces->resize(*device, a_size);
        if (!surfaceResult.has_value())
        {
            return surfaceResult;
        }
        auto listResult = commands->recreate_lists(*device);
        if (!listResult.has_value())
        {
            return listResult;
        }
        auto computeListResult = computeCommands->recreate_lists(*device);
        if (!computeListResult.has_value())
        {
            return computeListResult;
        }
        auto copyListResult = copyCommands->recreate_lists(*device);
        if (!copyListResult.has_value())
        {
            return copyListResult;
        }
        resizePending = false;
        {
            std::lock_guard lock(surfaceMutex);
            progress.surfaceSize = a_size;
        }
        return Result<void>::success();
    }

    // 最初に宣言し、他の COM Owner を全て破棄した最後に Leak を診断する
    detail::ResourceLeakChecker leakChecker;
    // 宣言順を Owner の寿命順にする。Queue Pool は Device より先に破棄される
    std::unique_ptr<detail::DX12RenderDevice> device;
    std::unique_ptr<detail::DX12QueuePool> queues;
    detail::DX12GpuCommandQueue* queue = nullptr;
    std::unique_ptr<detail::DescriptorAllocator> descriptors;
    std::unique_ptr<detail::DX12ResourcePool> resources;
    std::unique_ptr<detail::DX12BufferManager> buffers;
    std::unique_ptr<detail::DX12TextureManager> textures;
    std::unique_ptr<detail::DX12ViewManager> views;
    std::unique_ptr<detail::DX12SwapChain> presentation;
    std::unique_ptr<detail::DX12SurfacePool> surfaces;
    std::unique_ptr<detail::DX12PipelineManager> pipelineLibrary;
    std::unique_ptr<detail::DX12PipelineCache> pipelines;
    std::unique_ptr<detail::DX12StaticMeshPool> meshes;
    std::unique_ptr<detail::DX12CommandPool> commands;
    std::unique_ptr<detail::DX12CommandPool> computeCommands;
    std::unique_ptr<detail::DX12CommandPool> copyCommands;
    std::mutex surfaceMutex;
    WindowSize requestedSize{};
    BackendProgress progress{};
    bool isMinimized = false;
    bool resizePending = false;
    std::thread::id renderThreadId;
    std::uint64_t lastFrame = 0;
    bool hasRendered = false;
    bool isFaulted = false;
};

/// @brief 初期化済み GPU State を受け取る
DX12Backend::DX12Backend(std::unique_ptr<State> a_state) noexcept
    : m_state(std::move(a_state))
{
}

/// @brief 呼出側が明示停止を忘れても GPU 完了後に所有資源を破棄する
DX12Backend::~DX12Backend()
{
    [[maybe_unused]] auto result = shutdown();
}

/// @brief Hardware を優先し、対応 Adapter がなければ WARP で表示資源を生成する
Result<std::unique_ptr<DX12Backend>> DX12Backend::create(void* a_nativeWindow, WindowSize a_clientSize)
{
    using RendererResult = Result<std::unique_ptr<DX12Backend>>;

    // Window 生成後の有効な Client Area だけで Swap Chain を作る
    if (!a_nativeWindow || a_clientSize.width == 0 || a_clientSize.height == 0)
    {
        return RendererResult::failure({ErrorCategory::InvalidArgument, "DX12Backend.create"});
    }
    auto state = std::make_unique<State>();
    state->requestedSize = a_clientSize;
    state->progress.surfaceSize = a_clientSize;

    // 借用元の Device と Queue を先に生成してから表示と Command Context を作る
    auto deviceResult = detail::DX12RenderDevice::create();
    if (!deviceResult.has_value())
    {
        return RendererResult::failure(*deviceResult.try_error());
    }
    state->device = deviceResult.take_value();

    auto queueResult = detail::DX12QueuePool::create(*state->device);
    if (!queueResult.has_value())
    {
        return RendererResult::failure(*queueResult.try_error());
    }
    state->queues = queueResult.take_value();
    state->queue = &state->queues->context(GpuQueueType::Graphics);

    auto descriptorResult = detail::DescriptorAllocator::create(*state->device, 64, 64, 2048);
    if (!descriptorResult.has_value())
    {
        return RendererResult::failure(*descriptorResult.try_error());
    }
    state->descriptors = descriptorResult.take_value();

    auto resourcesResult = detail::DX12ResourcePool::create(*state->device, *state->queues,
                                                              *state->descriptors);
    if (!resourcesResult.has_value())
    {
        return RendererResult::failure(*resourcesResult.try_error());
    }
    state->resources = resourcesResult.take_value();

    auto buffersResult = detail::DX12BufferManager::create(*state->resources);
    if (!buffersResult.has_value())
    {
        return RendererResult::failure(*buffersResult.try_error());
    }
    state->buffers = buffersResult.take_value();

    auto texturesResult = detail::DX12TextureManager::create(*state->resources);
    if (!texturesResult.has_value())
    {
        return RendererResult::failure(*texturesResult.try_error());
    }
    state->textures = texturesResult.take_value();

    auto viewsResult = detail::DX12ViewManager::create(*state->device, *state->descriptors,
                                                        *state->resources);
    if (!viewsResult.has_value())
    {
        return RendererResult::failure(*viewsResult.try_error());
    }
    state->views = viewsResult.take_value();

    auto presentationResult = detail::DX12SwapChain::create(*state->device, *state->queue, *state->views,
                                                                  a_nativeWindow, a_clientSize);
    if (!presentationResult.has_value())
    {
        return RendererResult::failure(*presentationResult.try_error());
    }
    state->presentation = presentationResult.take_value();

    auto surfacesResult = detail::DX12SurfacePool::create(*state->device, *state->views,
                                                             *state->resources, a_clientSize);
    if (!surfacesResult.has_value())
    {
        return RendererResult::failure(*surfacesResult.try_error());
    }
    state->surfaces = surfacesResult.take_value();

    auto pipelineManagerResult = detail::DX12PipelineManager::create(*state->device, *state->queues);
    if (!pipelineManagerResult.has_value())
    {
        return RendererResult::failure(*pipelineManagerResult.try_error());
    }
    state->pipelineLibrary = pipelineManagerResult.take_value();
    auto pipelineResult = detail::DX12PipelineCache::create(*state->device, *state->resources,
                                                               *state->pipelineLibrary);
    if (!pipelineResult.has_value())
    {
        return RendererResult::failure(*pipelineResult.try_error());
    }
    state->pipelines = pipelineResult.take_value();

    auto meshResult = detail::DX12StaticMeshPool::create(*state->device, *state->resources);
    if (!meshResult.has_value())
    {
        return RendererResult::failure(*meshResult.try_error());
    }
    state->meshes = meshResult.take_value();

    auto commandsResult = detail::DX12CommandPool::create(*state->device);
    if (!commandsResult.has_value())
    {
        return RendererResult::failure(*commandsResult.try_error());
    }
    state->commands = commandsResult.take_value();
    auto computeCommandsResult = detail::DX12CommandPool::create(*state->device, GpuQueueType::Compute);
    if (!computeCommandsResult.has_value())
    {
        return RendererResult::failure(*computeCommandsResult.try_error());
    }
    state->computeCommands = computeCommandsResult.take_value();
    auto copyCommandsResult = detail::DX12CommandPool::create(*state->device, GpuQueueType::Copy);
    if (!copyCommandsResult.has_value())
    {
        return RendererResult::failure(*copyCommandsResult.try_error());
    }
    state->copyCommands = copyCommandsResult.take_value();
    DX12Backend renderer(std::move(state));
    return RendererResult::success(std::make_unique<DX12Backend>(std::move(renderer)));
}

/// @brief GPU 作業の完了を確認してから Window 依存資源を破棄する
Result<void> DX12Backend::shutdown()
{
    if (!m_state)
    {
        return Result<void>::success();
    }
    Result<void> waitResult = Result<void>::success();
    if (m_state->queues)
    {
        waitResult = m_state->queues->wait_idle();
        if (!waitResult.has_value())
        {
            // 完了未確認の資源を解放せず、呼出側が Shutdown を再試行できる状態を残す
            return waitResult;
        }
        m_state->commands->mark_idle();
        m_state->computeCommands->mark_idle();
        m_state->copyCommands->mark_idle();
    }
    m_state.reset();
    return waitResult;
}

/// @brief Graph の Clear、固定 Mesh、Copy Pass を記録し、Frame の GPU 完了条件を残す
Result<void> DX12Backend::render_frame(std::uint64_t a_frame)
{
    if (!m_state)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.render_frame"});
    }
    State& state = *m_state;
    // 部分 Submit 後の失敗では Graph の初期状態を保証できないため、停止まで再投入しない
    if (state.isFaulted)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.faulted"});
    }
    if (state.renderThreadId == std::thread::id{})
    {
        state.renderThreadId = std::this_thread::get_id();
    }
    if (state.renderThreadId != std::this_thread::get_id())
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "DX12Backend.render_frame"});
    }
    if (state.hasRendered && a_frame <= state.lastFrame)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.frameOrder"});
    }

    // MainThread から届いた最新 Size だけを反映し、最小化中は GPU 操作を保留する
    WindowSize requestedSize{};
    bool isMinimized = false;
    {
        std::lock_guard lock(state.surfaceMutex);
        requestedSize = state.requestedSize;
        isMinimized = state.isMinimized;
    }
    if (isMinimized || requestedSize.width == 0 || requestedSize.height == 0)
    {
        state.lastFrame = a_frame;
        state.hasRendered = true;
        return Result<void>::success();
    }
    auto resizeResult = state.resize(requestedSize);
    if (!resizeResult.has_value())
    {
        return resizeResult;
    }

    const UINT index = state.presentation->current_index();
    // Slot に対応する Upload CBV と Surface の再書込み前に前回 Frame の GPU 使用を終える
    auto graphicsIdle = state.commands->wait_for_slot(*state.queue, index);
    auto computeIdle = state.computeCommands->wait_for_slot(state.queues->context(GpuQueueType::Compute), index);
    auto copyIdle = state.copyCommands->wait_for_slot(state.queues->context(GpuQueueType::Copy), index);
    if (!graphicsIdle.has_value() || !computeIdle.has_value() || !copyIdle.has_value())
    {
        return Result<void>::failure(!graphicsIdle.has_value() ? *graphicsIdle.try_error()
                                     : !computeIdle.has_value() ? *computeIdle.try_error()
                                                                : *copyIdle.try_error());
    }
    ID3D12Resource* backBuffer = state.presentation->back_buffer(index);
    const GpuViewHandle colorView = state.surfaces->color_view(index);
    const GpuViewHandle depthView = state.surfaces->depth_view();
    const GpuResourceHandle colorResource = state.surfaces->color_resource(index);

    // 一時 Color と永続 Depth を Clear し、Copy Pass で Back Buffer へ転送する
    FrameGraph frameGraph;
    auto& graph = frameGraph.builder();
    auto backBufferHandle = graph.import_resource("BackBuffer", GraphResourceState::Present,
                                                  GraphResourceState::Present);
    auto colorHandle = graph.create_resource("Offscreen", GraphResourceLifetime::Transient,
                                             GraphResourceState::Common, GraphResourceState::Common);
    auto depthHandle = graph.create_resource("Depth", GraphResourceLifetime::Persistent,
                                             GraphResourceState::Common, GraphResourceState::Common);
    if (!backBufferHandle.has_value() || !colorHandle.has_value() || !depthHandle.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.graphResources"});
    }
    const GraphResourceHandle back = backBufferHandle.take_value();
    const GraphResourceHandle color = colorHandle.take_value();
    const GraphResourceHandle depth = depthHandle.take_value();
    const detail::TrianglePassContext triangleContext{*state.pipelines, *state.meshes, color, depth,
                                                      state.meshes->triangle(),
                                                      colorView, depthView,
                                                      {1.0f, 1.0f, 1.0f, 1.0f}};
    auto clearPass = frameGraph.add_pass(std::make_unique<detail::DX12ClearPass>(color, depth,
                                                                                 colorView, depthView));
    auto meshPass = frameGraph.add_pass(std::make_unique<detail::DX12TrianglePass>(triangleContext));
    auto copyPass = frameGraph.add_pass(std::make_unique<detail::DX12CopyToBackBufferPass>(
        color, back, colorResource, backBuffer));
    if (!clearPass.has_value() || !meshPass.has_value() || !copyPass.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.graphPasses"});
    }
    auto compiled = frameGraph.build();
    if (!compiled.has_value())
    {
        return Result<void>::failure(*compiled.try_error());
    }

    // 論理 Handle と物理 Resource の対応を Graph に照合してから実行へ渡す
    detail::DX12GraphResourceBindings bindings(graph);
    if (!bindings.bind(back, backBuffer).has_value() ||
        !bindings.bind(color, state.surfaces->color(index)).has_value() ||
        !bindings.bind(depth, state.surfaces->depth()).has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.graphBindings"});
    }
    auto physicalResources = bindings.resolve();
    if (!physicalResources.has_value())
    {
        return Result<void>::failure(*physicalResources.try_error());
    }
    std::vector<detail::GraphPassCallback> callbacks;
    callbacks.reserve(frameGraph.passes().size());
    // Graph が Pass を所有し、同期 Record の終了まで借用先 Resource を保持する
    for (const auto& pass : frameGraph.passes())
    {
        callbacks.emplace_back([&state, graphPass = pass.get(), requestedSize, index](ID3D12GraphicsCommandList* a_list) {
            auto* commandContext = state.commands->find_recording_context(a_list);
            if (!commandContext)
            {
                return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.passContext"});
            }
            detail::DX12CommandRecorder recorder(a_list, GpuQueueType::Graphics,
                                                  *state.resources, *state.pipelineLibrary);
            FrameGraphContext context(recorder, {requestedSize.width, requestedSize.height, index,
                                                 commandContext});
            return graphPass->execute(context);
        });
    }
    auto executeResult = detail::DX12GraphExecutor::execute(
        compiled.take_value(), *state.queues,
        {state.commands.get(), state.computeCommands.get(), state.copyCommands.get()}, index,
        physicalResources.take_value(), callbacks);
    if (!executeResult.has_value())
    {
        state.isFaulted = true;
        return executeResult;
    }
    // FrameController が 60 FPS を制御するため Present 側では待機を追加しない
    auto presentResult = state.presentation->present();
    if (!presentResult.has_value())
    {
        state.isFaulted = true;
        return presentResult;
    }
    state.lastFrame = a_frame;
    state.hasRendered = true;
    {
        std::lock_guard lock(state.surfaceMutex);
        ++state.progress.presentedFrames;
    }
    return Result<void>::success();
}

/// @brief 外部 Graph の論理 Resource を現在の Surface と Manager 資源へ解決する
Result<void> DX12Backend::render_graph_frame(std::uint64_t a_frame, FrameGraph& a_graph)
{
    if (!m_state || m_state->isFaulted)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.render_graph_frame"});
    }
    State& state = *m_state;
    if (state.renderThreadId == std::thread::id{})
    {
        state.renderThreadId = std::this_thread::get_id();
    }
    if (state.renderThreadId != std::this_thread::get_id())
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "DX12Backend.render_graph_frame"});
    }
    if (state.hasRendered && a_frame <= state.lastFrame)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.graphFrameOrder"});
    }
    WindowSize requestedSize{};
    bool isMinimized = false;
    {
        std::lock_guard lock(state.surfaceMutex);
        requestedSize = state.requestedSize;
        isMinimized = state.isMinimized;
    }
    if (isMinimized || requestedSize.width == 0 || requestedSize.height == 0)
    {
        state.lastFrame = a_frame;
        state.hasRendered = true;
        return Result<void>::success();
    }
    auto resizeResult = state.resize(requestedSize);
    if (!resizeResult.has_value())
    {
        return resizeResult;
    }
    const UINT index = state.presentation->current_index();
    auto graphicsIdle = state.commands->wait_for_slot(*state.queue, index);
    auto computeIdle = state.computeCommands->wait_for_slot(state.queues->context(GpuQueueType::Compute), index);
    auto copyIdle = state.copyCommands->wait_for_slot(state.queues->context(GpuQueueType::Copy), index);
    if (!graphicsIdle.has_value() || !computeIdle.has_value() || !copyIdle.has_value())
    {
        return Result<void>::failure(!graphicsIdle.has_value() ? *graphicsIdle.try_error()
                                     : !computeIdle.has_value() ? *computeIdle.try_error()
                                                                : *copyIdle.try_error());
    }

    // 外部 Pass は Surface の最終状態を COMMON に戻し、表示用 Copy が次に使う
    auto compiledResult = a_graph.build();
    auto bindingResult = a_graph.resource_bindings();
    if (!compiledResult.has_value() || !bindingResult.has_value())
    {
        return Result<void>::failure(!compiledResult.has_value() ? *compiledResult.try_error()
                                                             : *bindingResult.try_error());
    }
    auto bindings = bindingResult.take_value();
    detail::DX12GraphResourceBindings physicalBindings(a_graph.builder());
    bool hasSurfaceColor = false;
    for (std::uint32_t resourceIndex = 0; resourceIndex < bindings.size(); ++resourceIndex)
    {
        const auto node = GraphResourceHandle{resourceIndex, a_graph.builder().graph_id()};
        ID3D12Resource* resource = nullptr;
        switch (bindings[resourceIndex].kind)
        {
        case FrameGraphBindingKind::ManagedResource:
        {
            auto resolved = state.resources->resource(bindings[resourceIndex].resource);
            if (!resolved.has_value())
            {
                return Result<void>::failure(*resolved.try_error());
            }
            resource = resolved.take_value();
            break;
        }
        case FrameGraphBindingKind::SurfaceColor:
        {
            auto initialState = a_graph.builder().initial_state(node);
            auto finalState = a_graph.builder().final_state(node);
            if (hasSurfaceColor || !initialState.has_value() ||
                initialState.take_value() != GraphResourceState::Common || !finalState.has_value() ||
                finalState.take_value() != GraphResourceState::Common)
            {
                return Result<void>::failure({ErrorCategory::InvalidArgument,
                                              "DX12Backend.graphSurfaceColor"});
            }
            hasSurfaceColor = true;
            resource = state.surfaces->color(index);
            break;
        }
        case FrameGraphBindingKind::SurfaceDepth:
        {
            auto initialState = a_graph.builder().initial_state(node);
            auto finalState = a_graph.builder().final_state(node);
            if (!initialState.has_value() || !finalState.has_value() ||
                initialState.take_value() != GraphResourceState::Common ||
                finalState.take_value() != GraphResourceState::Common)
            {
                return Result<void>::failure({ErrorCategory::InvalidArgument,
                                              "DX12Backend.graphSurfaceDepth"});
            }
            resource = state.surfaces->depth();
            break;
        }
        }
        auto bound = physicalBindings.bind(node, resource);
        if (!bound.has_value())
        {
            return bound;
        }
    }
    if (!hasSurfaceColor)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument,
                                      "DX12Backend.graphMissingSurfaceColor"});
    }
    auto physicalResult = physicalBindings.resolve();
    if (!physicalResult.has_value())
    {
        return Result<void>::failure(*physicalResult.try_error());
    }
    auto compiled = compiledResult.take_value();
    std::vector<GpuQueueType> passQueues(a_graph.passes().size(), GpuQueueType::Graphics);
    for (const auto& pass : compiled.passes)
    {
        if (pass.sourceIndex >= passQueues.size())
        {
            return Result<void>::failure({ErrorCategory::InvalidState,
                                          "DX12Backend.graphPassIndex"});
        }
        passQueues[pass.sourceIndex] = pass.queue;
    }
    std::vector<detail::GraphPassCallback> callbacks;
    callbacks.reserve(a_graph.passes().size());
    for (std::size_t passIndex = 0; passIndex < a_graph.passes().size(); ++passIndex)
    {
        callbacks.emplace_back([&state, graphPass = a_graph.passes()[passIndex].get(),
                                queueType = passQueues[passIndex], requestedSize, index]
                               (ID3D12GraphicsCommandList* a_list) {
            auto* pool = queueType == GpuQueueType::Graphics ? state.commands.get()
                         : queueType == GpuQueueType::Compute ? state.computeCommands.get()
                                                              : state.copyCommands.get();
            auto* commandContext = pool->find_recording_context(a_list);
            if (!commandContext)
            {
                return Result<void>::failure({ErrorCategory::InvalidState,
                                              "DX12Backend.graphPassContext"});
            }
            detail::DX12CommandRecorder recorder(a_list, queueType, *state.resources,
                                                  *state.pipelineLibrary);
            FrameGraphContext context(recorder, {requestedSize.width, requestedSize.height, index,
                commandContext, nullptr, state.surfaces->color_view(index), state.surfaces->depth_view()});
            return graphPass->execute(context);
        });
    }
    FrameGraphExecutionStats executionStats;
    auto executeResult = detail::DX12GraphExecutor::execute(compiled, *state.queues,
        {state.commands.get(), state.computeCommands.get(), state.copyCommands.get()}, index,
        physicalResult.take_value(), callbacks, &executionStats);
    if (!executeResult.has_value())
    {
        state.isFaulted = true;
        return executeResult;
    }

    // Graph 終端の Surface を表示用 Back Buffer に Copy してから Present へ戻す
    auto copyLeaseResult = state.commands->acquire_batch(*state.queue, index);
    if (!copyLeaseResult.has_value())
    {
        state.isFaulted = true;
        return Result<void>::failure(*copyLeaseResult.try_error());
    }
    const auto copyLease = copyLeaseResult.take_value();
    ID3D12Resource* backBuffer = state.presentation->back_buffer(index);
    detail::DX12CommandRecorder copyRecorder(copyLease.list, GpuQueueType::Graphics,
                                              *state.resources, *state.pipelineLibrary);
    auto colorTransition = copyRecorder.transition(state.surfaces->color_resource(index),
        GpuResourceState::Common, GpuResourceState::CopySource);
    if (!colorTransition.has_value())
    {
        [[maybe_unused]] auto abortResult = state.commands->abort(copyLease);
        state.isFaulted = true;
        return colorTransition;
    }
    D3D12_RESOURCE_BARRIER backBarrier{};
    backBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backBarrier.Transition.pResource = backBuffer;
    backBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    backBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    backBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    copyLease.list->ResourceBarrier(1, &backBarrier);
    auto copyResult = copyRecorder.copy_to_back_buffer(backBuffer, state.surfaces->color_resource(index));
    if (!copyResult.has_value())
    {
        [[maybe_unused]] auto abortResult = state.commands->abort(copyLease);
        state.isFaulted = true;
        return copyResult;
    }
    std::swap(backBarrier.Transition.StateBefore, backBarrier.Transition.StateAfter);
    copyLease.list->ResourceBarrier(1, &backBarrier);
    auto colorRestore = copyRecorder.transition(state.surfaces->color_resource(index),
        GpuResourceState::CopySource, GpuResourceState::Common);
    if (!colorRestore.has_value())
    {
        [[maybe_unused]] auto abortResult = state.commands->abort(copyLease);
        state.isFaulted = true;
        return colorRestore;
    }
    auto submitResult = state.commands->submit(*state.queue, copyLease);
    if (!submitResult.has_value())
    {
        [[maybe_unused]] auto abortResult = state.commands->abort(copyLease);
        state.isFaulted = true;
        return submitResult;
    }
    auto retireResult = state.commands->retire(*state.queue, copyLease);
    if (!retireResult.has_value())
    {
        state.isFaulted = true;
        return retireResult;
    }
    auto presentResult = state.presentation->present();
    if (!presentResult.has_value())
    {
        state.isFaulted = true;
        return presentResult;
    }
    state.lastFrame = a_frame;
    state.hasRendered = true;
    a_graph.update_execution_stats(std::move(executionStats));
    {
        std::lock_guard lock(state.surfaceMutex);
        ++state.progress.presentedFrames;
    }
    return Result<void>::success();
}

/// @brief Window Event の最新 Size と最小化状態を Render Thread へ渡す
Result<void> DX12Backend::request_surface(WindowSize a_clientSize, bool a_isMinimized)
{
    if (!m_state)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12Backend.request_surface"});
    }
    std::lock_guard lock(m_state->surfaceMutex);
    if (!a_isMinimized)
    {
        m_state->requestedSize = a_clientSize;
    }
    m_state->isMinimized = a_isMinimized;
    return Result<void>::success();
}

/// @brief 適用済み Surface と Present 数を同期して返す
Result<BackendProgress> DX12Backend::progress() const
{
    if (!m_state)
    {
        return Result<BackendProgress>::failure({ErrorCategory::InvalidState, "DX12Backend.progress"});
    }
    std::lock_guard lock(m_state->surfaceMutex);
    return Result<BackendProgress>::success(m_state->progress);
}

/// @brief Backend 停止後の借用を防ぐため Owner 不在なら nullptr を返す
IBufferManager* DX12Backend::get_buffer_manager() noexcept
{
    return m_state ? m_state->buffers.get() : nullptr;
}

/// @brief Backend 停止後の借用を防ぐため Owner 不在なら nullptr を返す
ITextureManager* DX12Backend::get_texture_manager() noexcept
{
    return m_state ? m_state->textures.get() : nullptr;
}

/// @brief 表示用 View と Resource View の共通 Owner を貸す
IViewManager* DX12Backend::get_view_manager() noexcept
{
    return m_state ? m_state->views.get() : nullptr;
}

/// @brief Queue の実所有は Backend State に残す
IQueuePool* DX12Backend::get_queue_pool() noexcept
{
    return m_state ? m_state->queues.get() : nullptr;
}

/// @brief Hardware/WARP の診断契約を持つ Device を貸す
IRenderDevice* DX12Backend::get_render_device() noexcept
{
    return m_state ? m_state->device.get() : nullptr;
}

/// @brief Shader と PSO の所有契約を持つ Manager を貸す
IPipelineManager* DX12Backend::get_pipeline_manager() noexcept
{
    return m_state ? m_state->pipelineLibrary.get() : nullptr;
}

/// @brief Graphics の Command Context 貸出契約を持つ Pool を貸す
ICommandPool* DX12Backend::get_command_pool() noexcept
{
    return m_state ? m_state->commands.get() : nullptr;
}

/// @brief Adapter 選択経路を診断可能にする
bool DX12Backend::is_warp() const noexcept
{
    return m_state && m_state->device->is_warp();
}
} // namespace cue
