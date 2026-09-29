#include <Cue/Renderer/D3D12/D3D12Renderer.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <Cue/Renderer/FrameGraph/FrameGraph.h>

#include "D3D12DeviceContext.h"
#include "D3D12CommandPool.h"
#include "D3D12CommandRecorder.h"
#include "D3D12GraphExecutor.h"
#include "D3D12PipelineCache.h"
#include "D3D12PipelineLibrary.h"
#include "D3D12Presentation.h"
#include "D3D12QueueContext.h"
#include "D3D12QueuePool.h"
#include "D3D12ResourcePool.h"
#include "D3D12StaticMeshPool.h"
#include "D3D12SurfacePool.h"
#include "D3D12TrianglePass.h"
#include "D3D12ViewManager.h"

namespace cue
{
class D3D12Renderer::State final
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

    // 宣言順を Owner の寿命順にする。Queue Pool は Device より先に破棄される
    std::unique_ptr<detail::D3D12DeviceContext> device;
    std::unique_ptr<detail::D3D12QueuePool> queues;
    detail::D3D12QueueContext* queue = nullptr;
    std::unique_ptr<detail::D3D12ResourcePool> resources;
    std::unique_ptr<detail::D3D12ViewManager> views;
    std::unique_ptr<detail::D3D12Presentation> presentation;
    std::unique_ptr<detail::D3D12SurfacePool> surfaces;
    std::unique_ptr<detail::D3D12PipelineLibrary> pipelineLibrary;
    std::unique_ptr<detail::D3D12PipelineCache> pipelines;
    std::unique_ptr<detail::D3D12StaticMeshPool> meshes;
    std::unique_ptr<detail::D3D12CommandPool> commands;
    std::unique_ptr<detail::D3D12CommandPool> computeCommands;
    std::unique_ptr<detail::D3D12CommandPool> copyCommands;
    std::mutex surfaceMutex;
    WindowSize requestedSize{};
    D3D12RendererProgress progress{};
    bool isMinimized = false;
    bool resizePending = false;
    std::thread::id renderThreadId;
    std::uint64_t lastFrame = 0;
    bool hasRendered = false;
    bool isFaulted = false;
};

/// @brief 初期化済み GPU State を受け取る
D3D12Renderer::D3D12Renderer(std::unique_ptr<State> a_state) noexcept
    : m_state(std::move(a_state))
{
}

/// @brief 呼出側が明示停止を忘れても GPU 完了後に所有資源を破棄する
D3D12Renderer::~D3D12Renderer()
{
    [[maybe_unused]] auto result = shutdown();
}

/// @brief Hardware を優先し、対応 Adapter がなければ WARP で表示資源を生成する
Result<std::unique_ptr<D3D12Renderer>> D3D12Renderer::create(void* a_nativeWindow, WindowSize a_clientSize)
{
    using RendererResult = Result<std::unique_ptr<D3D12Renderer>>;

    // Window 生成後の有効な Client Area だけで Swap Chain を作る
    if (!a_nativeWindow || a_clientSize.width == 0 || a_clientSize.height == 0)
    {
        return RendererResult::failure({ErrorCategory::InvalidArgument, "D3D12Renderer.create"});
    }
    auto state = std::make_unique<State>();
    state->requestedSize = a_clientSize;
    state->progress.surfaceSize = a_clientSize;

    // 借用元の Device と Queue を先に生成してから表示と Command Context を作る
    auto deviceResult = detail::D3D12DeviceContext::create();
    if (!deviceResult.has_value())
    {
        return RendererResult::failure(*deviceResult.try_error());
    }
    state->device = deviceResult.take_value();

    auto queueResult = detail::D3D12QueuePool::create(*state->device);
    if (!queueResult.has_value())
    {
        return RendererResult::failure(*queueResult.try_error());
    }
    state->queues = queueResult.take_value();
    state->queue = &state->queues->context(GpuQueueType::Graphics);

    auto resourcesResult = detail::D3D12ResourcePool::create(*state->device, *state->queues);
    if (!resourcesResult.has_value())
    {
        return RendererResult::failure(*resourcesResult.try_error());
    }
    state->resources = resourcesResult.take_value();

    auto viewsResult = detail::D3D12ViewManager::create(*state->device, 2 * detail::k_backBufferCount);
    if (!viewsResult.has_value())
    {
        return RendererResult::failure(*viewsResult.try_error());
    }
    state->views = viewsResult.take_value();

    auto presentationResult = detail::D3D12Presentation::create(*state->device, *state->queue, *state->views,
                                                                  a_nativeWindow, a_clientSize);
    if (!presentationResult.has_value())
    {
        return RendererResult::failure(*presentationResult.try_error());
    }
    state->presentation = presentationResult.take_value();

    auto surfacesResult = detail::D3D12SurfacePool::create(*state->device, *state->views,
                                                             *state->resources, a_clientSize);
    if (!surfacesResult.has_value())
    {
        return RendererResult::failure(*surfacesResult.try_error());
    }
    state->surfaces = surfacesResult.take_value();

    state->pipelineLibrary = detail::D3D12PipelineLibrary::create(*state->device, *state->queues);
    auto pipelineResult = detail::D3D12PipelineCache::create(*state->device, *state->resources,
                                                               *state->pipelineLibrary);
    if (!pipelineResult.has_value())
    {
        return RendererResult::failure(*pipelineResult.try_error());
    }
    state->pipelines = pipelineResult.take_value();

    auto meshResult = detail::D3D12StaticMeshPool::create(*state->device, *state->resources);
    if (!meshResult.has_value())
    {
        return RendererResult::failure(*meshResult.try_error());
    }
    state->meshes = meshResult.take_value();

    auto commandsResult = detail::D3D12CommandPool::create(*state->device);
    if (!commandsResult.has_value())
    {
        return RendererResult::failure(*commandsResult.try_error());
    }
    state->commands = commandsResult.take_value();
    auto computeCommandsResult = detail::D3D12CommandPool::create(*state->device, GpuQueueType::Compute);
    if (!computeCommandsResult.has_value())
    {
        return RendererResult::failure(*computeCommandsResult.try_error());
    }
    state->computeCommands = computeCommandsResult.take_value();
    auto copyCommandsResult = detail::D3D12CommandPool::create(*state->device, GpuQueueType::Copy);
    if (!copyCommandsResult.has_value())
    {
        return RendererResult::failure(*copyCommandsResult.try_error());
    }
    state->copyCommands = copyCommandsResult.take_value();
    D3D12Renderer renderer(std::move(state));
    return RendererResult::success(std::make_unique<D3D12Renderer>(std::move(renderer)));
}

/// @brief GPU 作業の完了を確認してから Window 依存資源を破棄する
Result<void> D3D12Renderer::shutdown()
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
Result<void> D3D12Renderer::render_frame(std::uint64_t a_frame)
{
    if (!m_state)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12Renderer.render_frame"});
    }
    State& state = *m_state;
    // 部分 Submit 後の失敗では Graph の初期状態を保証できないため、停止まで再投入しない
    if (state.isFaulted)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12Renderer.faulted"});
    }
    if (state.renderThreadId == std::thread::id{})
    {
        state.renderThreadId = std::this_thread::get_id();
    }
    if (state.renderThreadId != std::this_thread::get_id())
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "D3D12Renderer.render_frame"});
    }
    if (state.hasRendered && a_frame <= state.lastFrame)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12Renderer.frameOrder"});
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
    FrameGraphBuilder graph;
    auto backBufferHandle = graph.import_resource("BackBuffer", GraphResourceState::Present,
                                                  GraphResourceState::Present);
    auto colorHandle = graph.create_resource("Offscreen", GraphResourceLifetime::Transient,
                                             GraphResourceState::Common, GraphResourceState::Common);
    auto depthHandle = graph.create_resource("Depth", GraphResourceLifetime::Persistent,
                                             GraphResourceState::Common, GraphResourceState::Common);
    if (!backBufferHandle.has_value() || !colorHandle.has_value() || !depthHandle.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12Renderer.graphResources"});
    }
    const GraphResourceHandle back = backBufferHandle.take_value();
    const GraphResourceHandle color = colorHandle.take_value();
    const GraphResourceHandle depth = depthHandle.take_value();
    const detail::TrianglePassContext triangleContext{*state.pipelines, *state.meshes, color, depth,
                                                      state.meshes->triangle(), requestedSize, index,
                                                      colorView, depthView,
                                                      {1.0f, 1.0f, 1.0f, 1.0f}};
    const detail::D3D12TrianglePass trianglePass(triangleContext);
    auto clearPass = graph.add_pass("Clear", {{color, GraphResourceState::RenderTarget,
                                                GraphAccess::Write},
                                               {depth, GraphResourceState::DepthWrite,
                                                GraphAccess::Write}});
    auto meshPass = trianglePass.setup(graph);
    auto copyPass = graph.add_pass("CopyToBackBuffer", {{color, GraphResourceState::CopySource,
                                                          GraphAccess::Read},
                                                         {back, GraphResourceState::CopyDest,
                                                          GraphAccess::Write}});
    if (!clearPass.has_value() || !meshPass.has_value() || !copyPass.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12Renderer.graphPasses"});
    }
    auto compiled = graph.compile();
    if (!compiled.has_value())
    {
        return Result<void>::failure(*compiled.try_error());
    }

    ID3D12Resource* offscreen = state.surfaces->color(index);
    std::vector<detail::GraphPassCallback> callbacks;
    callbacks.emplace_back([&state, colorView, depthView](ID3D12GraphicsCommandList* a_list) {
        detail::D3D12CommandRecorder recorder(a_list, GpuQueueType::Graphics,
                                              *state.resources, *state.pipelineLibrary);
        auto targets = recorder.set_render_targets(colorView, depthView);
        if (!targets.has_value())
        {
            return targets;
        }
        auto clearColor = recorder.clear_color(colorView, {0.07f, 0.13f, 0.25f, 1.0f});
        if (!clearColor.has_value())
        {
            return clearColor;
        }
        return recorder.clear_depth(depthView, 1.0f);
    });
    // Pass はこの同期 Record が完了するまで生存し、State の資源を借用する
    callbacks.emplace_back([&state, &trianglePass](ID3D12GraphicsCommandList* a_list) {
        detail::D3D12CommandRecorder recorder(a_list, GpuQueueType::Graphics,
                                              *state.resources, *state.pipelineLibrary);
        return trianglePass.execute(recorder);
    });
    callbacks.emplace_back([&state, backBuffer, colorResource](ID3D12GraphicsCommandList* a_list) {
        detail::D3D12CommandRecorder recorder(a_list, GpuQueueType::Graphics,
                                              *state.resources, *state.pipelineLibrary);
        return recorder.copy_to_back_buffer(backBuffer, colorResource);
    });
    auto executeResult = detail::D3D12GraphExecutor::execute(
        compiled.take_value(), *state.queues,
        {state.commands.get(), state.computeCommands.get(), state.copyCommands.get()}, index,
        {backBuffer, offscreen, state.surfaces->depth()}, callbacks);
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

/// @brief Window Event の最新 Size と最小化状態を Render Thread へ渡す
Result<void> D3D12Renderer::request_surface(WindowSize a_clientSize, bool a_isMinimized)
{
    if (!m_state)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "D3D12Renderer.request_surface"});
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
Result<D3D12RendererProgress> D3D12Renderer::progress() const
{
    if (!m_state)
    {
        return Result<D3D12RendererProgress>::failure({ErrorCategory::InvalidState, "D3D12Renderer.progress"});
    }
    std::lock_guard lock(m_state->surfaceMutex);
    return Result<D3D12RendererProgress>::success(m_state->progress);
}

/// @brief Adapter 選択経路を診断可能にする
bool D3D12Renderer::is_warp() const noexcept
{
    return m_state && m_state->device->is_warp();
}
} // namespace cue
