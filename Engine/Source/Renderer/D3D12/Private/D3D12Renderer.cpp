#include <Cue/Renderer/D3D12/D3D12Renderer.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <Cue/Renderer/FrameGraph/FrameGraph.h>

#include "D3D12DeviceContext.h"
#include "D3D12CommandPool.h"
#include "D3D12GraphExecutor.h"
#include "D3D12PipelineCache.h"
#include "D3D12Presentation.h"
#include "D3D12StaticMeshPool.h"
#include "D3D12SurfacePool.h"
#include "D3D12ViewManager.h"

namespace cue
{
class D3D12Renderer::State final
{
public:
    /// @brief GPU Submit の残りがあれば Owner の破棄前に完了を待つ
    ~State()
    {
        if (device && commands && commands->has_pending_gpu())
        {
            [[maybe_unused]] auto result = device->wait_idle();
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
        if (commands->has_pending_gpu())
        {
            auto idleResult = device->wait_idle();
            if (!idleResult.has_value())
            {
                return idleResult;
            }
        }
        // 途中失敗後も同じ Size で再試行し、Command List が欠けた状態を成功扱いしない
        resizePending = true;
        commands->release_for_resize();
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
        resizePending = false;
        {
            std::lock_guard lock(surfaceMutex);
            progress.surfaceSize = a_size;
        }
        return Result<void>::success();
    }

    // 宣言順を Owner の寿命順にする。Command と Presentation は View、Device より先に破棄される
    std::unique_ptr<detail::D3D12DeviceContext> device;
    std::unique_ptr<detail::D3D12ViewManager> views;
    std::unique_ptr<detail::D3D12Presentation> presentation;
    std::unique_ptr<detail::D3D12SurfacePool> surfaces;
    std::unique_ptr<detail::D3D12PipelineCache> pipelines;
    std::unique_ptr<detail::D3D12StaticMeshPool> meshes;
    std::unique_ptr<detail::D3D12CommandPool> commands;
    std::mutex surfaceMutex;
    WindowSize requestedSize{};
    D3D12RendererProgress progress{};
    bool isMinimized = false;
    bool resizePending = false;
    std::thread::id renderThreadId;
    std::uint64_t lastFrame = 0;
    bool hasRendered = false;
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

/// @brief Hardware 優先または明示 WARP で Device と Presentation 資源を生成する
Result<std::unique_ptr<D3D12Renderer>> D3D12Renderer::create(void* a_nativeWindow, WindowSize a_clientSize,
                                                               bool a_useWarp)
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

    // 借用元の Device と View Manager を先に生成してから表示と Command Context を作る
    auto deviceResult = detail::D3D12DeviceContext::create(a_useWarp);
    if (!deviceResult.has_value())
    {
        return RendererResult::failure(*deviceResult.try_error());
    }
    state->device = deviceResult.take_value();

    auto viewsResult = detail::D3D12ViewManager::create(*state->device, 2 * detail::k_backBufferCount);
    if (!viewsResult.has_value())
    {
        return RendererResult::failure(*viewsResult.try_error());
    }
    state->views = viewsResult.take_value();

    auto presentationResult = detail::D3D12Presentation::create(*state->device, *state->views,
                                                                  a_nativeWindow, a_clientSize);
    if (!presentationResult.has_value())
    {
        return RendererResult::failure(*presentationResult.try_error());
    }
    state->presentation = presentationResult.take_value();

    auto surfacesResult = detail::D3D12SurfacePool::create(*state->device, *state->views, a_clientSize);
    if (!surfacesResult.has_value())
    {
        return RendererResult::failure(*surfacesResult.try_error());
    }
    state->surfaces = surfacesResult.take_value();

    auto pipelineResult = detail::D3D12PipelineCache::create(*state->device);
    if (!pipelineResult.has_value())
    {
        return RendererResult::failure(*pipelineResult.try_error());
    }
    state->pipelines = pipelineResult.take_value();

    auto meshResult = detail::D3D12StaticMeshPool::create(*state->device);
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
    if (m_state->commands && m_state->commands->has_pending_gpu())
    {
        waitResult = m_state->device->wait_idle();
        if (!waitResult.has_value())
        {
            // 完了未確認の資源を解放せず、呼出側が Shutdown を再試行できる状態を残す
            return waitResult;
        }
        m_state->commands->mark_idle();
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
    ID3D12Resource* backBuffer = state.presentation->back_buffer(index);
    auto rtvResult = state.surfaces->color_rtv(index);
    if (!rtvResult.has_value())
    {
        return Result<void>::failure(*rtvResult.try_error());
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvResult.take_value();

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
    auto clearPass = graph.add_pass("Clear", {{color, GraphResourceState::RenderTarget,
                                                GraphAccess::Write},
                                               {depth, GraphResourceState::DepthWrite,
                                                GraphAccess::Write}});
    auto meshPass = graph.add_pass("FixedMesh", {{color, GraphResourceState::RenderTarget,
                                                   GraphAccess::Write},
                                                  {depth, GraphResourceState::DepthWrite,
                                                   GraphAccess::Write}});
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

    auto leaseResult = state.commands->acquire(*state.device, index);
    if (!leaseResult.has_value())
    {
        return Result<void>::failure(*leaseResult.try_error());
    }
    const detail::CommandLease lease = leaseResult.take_value();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = state.surfaces->depth_dsv();
    ID3D12Resource* offscreen = state.surfaces->color(index);
    std::vector<detail::GraphPassCallback> callbacks;
    callbacks.emplace_back([rtv, dsv](ID3D12GraphicsCommandList* a_list) {
        // Surface は Submit 完了まで State が所有し、Callback は View だけ借用する
        a_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        constexpr float k_clearColor[4] = {0.07f, 0.13f, 0.25f, 1.0f};
        a_list->ClearRenderTargetView(rtv, k_clearColor, 0, nullptr);
        a_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        return Result<void>::success();
    });
    // Frame 内の Draw 入力は値として固定し、次 Frame の更新に引きずられない
    const detail::StaticMeshHandle mesh = state.meshes->triangle();
    const std::array<float, 4> tint = {1.0f, 1.0f, 1.0f, 1.0f};
    callbacks.emplace_back([&state, index, requestedSize, rtv, dsv, mesh, tint](
                               ID3D12GraphicsCommandList* a_list) -> Result<void> {
        auto bindResult = state.pipelines->bind(a_list, index, tint);
        if (!bindResult.has_value())
        {
            return bindResult;
        }
        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(requestedSize.width);
        viewport.Height = static_cast<float>(requestedSize.height);
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor{0, 0, static_cast<LONG>(requestedSize.width),
                           static_cast<LONG>(requestedSize.height)};
        a_list->RSSetViewports(1, &viewport);
        a_list->RSSetScissorRects(1, &scissor);
        a_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        return state.meshes->draw(a_list, mesh);
    });
    callbacks.emplace_back([backBuffer, offscreen](ID3D12GraphicsCommandList* a_list) {
        a_list->CopyResource(backBuffer, offscreen);
        return Result<void>::success();
    });
    auto recordResult = detail::D3D12GraphExecutor::record(compiled.take_value(), lease.list,
                                                             {backBuffer, offscreen, state.surfaces->depth()}, callbacks);
    if (!recordResult.has_value())
    {
        [[maybe_unused]] auto abortResult = state.commands->abort(lease);
        return recordResult;
    }

    auto submitResult = state.commands->submit(*state.device, lease);
    if (!submitResult.has_value())
    {
        [[maybe_unused]] auto abortResult = state.commands->abort(lease);
        return submitResult;
    }
    // FrameController が 60 FPS を制御するため Present 側では待機を追加しない
    auto presentResult = state.presentation->present();
    if (!presentResult.has_value())
    {
        return presentResult;
    }
    auto retireResult = state.commands->retire(*state.device, lease);
    if (!retireResult.has_value())
    {
        return retireResult;
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
