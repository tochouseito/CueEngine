#include <WindowsHost/WindowsHost.h>

#include <atomic>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include <DX12/DX12Backend.h>
#include <DX12/DX12CommandPool.h>
#include <DX12/DX12MainFrameGraph.h>
#include <DX12/DX12RenderDevice.h>
#include <DX12/DX12SwapChain.h>
#include <Platform/Diagnostics.h>
#include <Platform/WindowSystem.h>
#include <Platform/Windows/WindowsPlatform.h>
#include <RHI/BackendFactory.h>
#include <Runtime/Runtime.h>

namespace cue
{
class WindowsHost::State final
{
  public:
    std::unique_ptr<WindowSystem> system;
    std::unique_ptr<Window> window;
    std::unique_ptr<IBackend> backend;
    dx12::DX12Backend *dx12Backend = nullptr;
    std::unique_ptr<dx12::DX12MainFrameGraph> graph;
    WindowsThreadServices services;
    std::unique_ptr<Runtime> runtime;
    std::atomic<bool> isRenderStopped = false;
    std::atomic<bool> isPresentationSuspended = false;
    WindowSize presentationSize{};
    bool isCloseRequested = false;
    bool isDestroyed = false;

    /// @brief 固定 Graph の Command を Graphics Queue に提出して表示する
    [[nodiscard]] Result<void> render(std::uint64_t a_frameIndex, std::stop_token a_stopToken,
                                      std::uint32_t a_frameCount)
    {
        if (a_stopToken.stop_requested() || isRenderStopped.load() || isPresentationSuspended.load())
        {
            return Result<void>::success();
        }
        auto *commandPool = dx12Backend->get_command_pool();
        auto *queuePool = dx12Backend->get_queue_pool();
        auto *swapChain = dx12Backend->get_swap_chain();
        if (!commandPool || !queuePool || !swapChain || !swapChain->graphics_queue() || !graph)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "WindowsHost.render.resources"});
        }
        const auto frameSlot = static_cast<std::uint32_t>(a_frameIndex % a_frameCount);
        auto executeResult = graph->execute(frameSlot, *commandPool, *queuePool, [&]()
        {
            return a_stopToken.stop_requested() || isRenderStopped.load() || isPresentationSuspended.load();
        });
        if (!executeResult.has_value())
        {
            return Result<void>::failure(*executeResult.try_error());
        }
        if (!*executeResult.try_value())
        {
            return Result<void>::success();
        }
        return swapChain->present();
    }
};

/// @brief 起動設定と構築 Thread を固定する
WindowsHost::WindowsHost(WindowsHostConfig a_config)
    : m_config(std::move(a_config)), m_ownerId(std::this_thread::get_id())
{
}

/// @brief 明示停止がない場合も Window を回収する
WindowsHost::~WindowsHost()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("CueWindowsHost cleanup", *result.try_error(), DiagnosticSeverity::Error);
    }
}

/// @brief Windows の Window、Renderer Backend、Runtime を順に構築する
Result<void> WindowsHost::initialize()
{
    // Win32 の Window は構築 Thread でしか操作できない
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "WindowsHost.initialize"});
    }
    if (m_lifecycle != Lifecycle::Uninitialized)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "WindowsHost.initialize"});
    }
    m_lifecycle = Lifecycle::Stopped;

    // FrameController の許容先行数と Flip Model の最小 BackBuffer 数を表示前に検証する
    if (m_config.frame.maxFramesInFlight == 0 || m_config.frame.maxFramesInFlight > 2 ||
        m_config.presentation.bufferCount < 2)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "WindowsHost.config"});
    }

    // 部分初期化の失敗時は元の Error を返し、Cleanup の失敗は別に診断する
    auto rollback = [this](Error a_error)
    {
        auto cleanupResult = shutdown();
        if (!cleanupResult.has_value())
        {
            report_error("CueWindowsHost cleanup", *cleanupResult.try_error(), DiagnosticSeverity::Error);
        }
        return Result<void>::failure(std::move(a_error));
    };

    // WindowSystem は Window より長く生存させる
    m_state = std::make_unique<State>();
    auto systemResult = create_windows_window_system();
    if (!systemResult.has_value())
    {
        return rollback(*systemResult.try_error());
    }
    m_state->system = systemResult.take_value();

    // Window を作成し、表示に失敗した場合も所有先から回収する
    auto windowResult = m_state->system->create_window(m_config.window);
    if (!windowResult.has_value())
    {
        return rollback(*windowResult.try_error());
    }
    m_state->window = windowResult.take_value();

    // Backend が Device を所有し、Window より先に停止できる順序で保持する
    auto backendResult = create_backend();
    if (!backendResult.has_value())
    {
        return rollback(*backendResult.try_error());
    }
    m_state->backend = backendResult.take_value();
    m_state->dx12Backend = dynamic_cast<dx12::DX12Backend *>(m_state->backend.get());
    if (!m_state->dx12Backend)
    {
        return rollback({ErrorCategory::InvalidState, "WindowsHost.backend"});
    }

    // Window の Client Area と同じ大きさで Back Buffer と固定 Graph を用意する
    auto handleResult = borrow_windows_window_handle(*m_state->window);
    if (!handleResult.has_value())
    {
        return rollback(*handleResult.try_error());
    }
    m_state->presentationSize = m_state->window->client_size();
    dx12::DX12SwapChainConfig swapConfig{};
    swapConfig.width = m_state->presentationSize.width;
    swapConfig.height = m_state->presentationSize.height;
    swapConfig.bufferCount = m_config.presentation.bufferCount;
    swapConfig.isVSyncEnabled = m_config.presentation.isVSyncEnabled;
    swapConfig.isTearingAllowed = m_config.presentation.isTearingAllowed;
    auto swapResult = m_state->dx12Backend->create_swap_chain(*handleResult.try_value(), swapConfig);
    if (!swapResult.has_value())
    {
        return rollback(*swapResult.try_error());
    }
    auto *device = dynamic_cast<dx12::DX12RenderDevice *>(m_state->dx12Backend->get_render_device());
    auto *rtvAllocator = m_state->dx12Backend->get_descriptor_allocator(dx12::DX12DescriptorHeapRole::Rtv);
    auto *srvAllocator = m_state->dx12Backend->get_descriptor_allocator(dx12::DX12DescriptorHeapRole::ShaderView);
    if (!device || !rtvAllocator || !srvAllocator)
    {
        return rollback({ErrorCategory::InvalidState, "WindowsHost.graph.resources"});
    }
    auto graphResult = dx12::DX12MainFrameGraph::create(*device, *m_state->dx12Backend->get_swap_chain(),
                                                        m_config.frame.maxFramesInFlight, *rtvAllocator, *srvAllocator,
                                                        m_config.presentation.clearColor);
    if (!graphResult.has_value())
    {
        return rollback(*graphResult.try_error());
    }
    m_state->graph = graphResult.take_value();

    // Runtime が借りる時間と Worker Service を、Runtime より長く生存させる
    auto servicesResult = create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return rollback(*servicesResult.try_error());
    }
    m_state->services = servicesResult.take_value();

    m_state->runtime = std::make_unique<Runtime>(m_config.frame, *m_state->services.clock, *m_state->services.waiter,
                                                 *m_state->services.threadFactory);
    State *state = m_state.get();
    const auto frameCount = m_config.frame.maxFramesInFlight;
    auto runtimeResult =
        m_state->runtime->initialize([](std::uint64_t, std::stop_token) { return Result<void>::success(); },
                                     [state, frameCount](std::uint64_t a_frameIndex, std::stop_token a_stopToken)
                                     { return state->render(a_frameIndex, a_stopToken, frameCount); });
    if (!runtimeResult.has_value())
    {
        return rollback(*runtimeResult.try_error());
    }

    // GPU と Frame の初期化に成功した後で Window を表示する
    auto showResult = m_state->window->show();
    if (!showResult.has_value())
    {
        return rollback(*showResult.try_error());
    }

    m_lifecycle = Lifecycle::Running;
    return Result<void>::success();
}

/// @brief Windows Event を消費して Window の終了要求を返す
Result<bool> WindowsHost::step()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "WindowsHost.step"});
    }
    if (m_lifecycle != Lifecycle::Running)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "WindowsHost.step"});
    }

    // Win32 Message を処理し、Queue 上の終了通知も同じ周回で反映する
    auto pumpResult = m_state->system->pump_events();
    if (!pumpResult.has_value())
    {
        return Result<bool>::failure(*pumpResult.try_error());
    }
    WindowEvent event{};
    while (m_state->window->try_pop_event(event))
    {
        if (event.type == WindowEventType::CloseRequested)
        {
            m_state->isCloseRequested = true;
            m_state->isRenderStopped.store(true);
        }
        else if (event.type == WindowEventType::Destroyed)
        {
            m_state->isDestroyed = true;
            m_state->isRenderStopped.store(true);
        }
        else if (event.type == WindowEventType::Minimized || event.type == WindowEventType::Resized ||
                 event.type == WindowEventType::Restored)
        {
            // Resize 対応までは旧 Back Buffer サイズでの追加提出を止める
            m_state->isPresentationSuspended.store(event.clientSize.width == 0 || event.clientSize.height == 0 ||
                                                   event.clientSize.width != m_state->presentationSize.width ||
                                                   event.clientSize.height != m_state->presentationSize.height);
        }
    }

    // Window が生存したままの WM_QUIT は所有契約に反する
    if (*pumpResult.try_value() == PumpStatus::QuitRequested && !m_state->isDestroyed)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "WindowsHost.quit"});
    }
    if (m_state->isCloseRequested || m_state->isDestroyed || *pumpResult.try_value() == PumpStatus::QuitRequested)
    {
        return Result<bool>::success(false);
    }

    // 終了 Event がない周回だけ次の Frame を進める。枠が満杯なら次の周回で再試行する
    auto frameResult = m_state->runtime->step();
    if (!frameResult.has_value())
    {
        return Result<bool>::failure(*frameResult.try_error());
    }
    return Result<bool>::success(true);
}

/// @brief 実行中だけ CPU Frame の進行を構築 Thread に公開する
Result<FrameProgress> WindowsHost::frame_progress() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<FrameProgress>::failure({ErrorCategory::WrongThread, "WindowsHost.frame_progress"});
    }
    if (m_lifecycle != Lifecycle::Running || !m_state || !m_state->runtime)
    {
        return Result<FrameProgress>::failure({ErrorCategory::InvalidState, "WindowsHost.frame_progress"});
    }
    return m_state->runtime->progress();
}

/// @brief Runtime、Backend、Window を順に停止して Destroyed と Quit を処理する
Result<void> WindowsHost::shutdown()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "WindowsHost.shutdown"});
    }
    m_lifecycle = Lifecycle::Stopped;
    if (!m_state)
    {
        return Result<void>::success();
    }

    // Worker の Callback を止めてから GPU と Window の所有先を解放する
    m_state->isRenderStopped.store(true);
    std::optional<Error> failure;
    if (m_state->runtime)
    {
        auto runtimeResult = m_state->runtime->shutdown();
        if (!runtimeResult.has_value())
        {
            failure = *runtimeResult.try_error();
        }
        m_state->runtime.reset();
    }
    if (m_state->graph)
    {
        auto graphResult = m_state->graph->shutdown();
        if (!graphResult.has_value() && !failure)
        {
            failure = *graphResult.try_error();
        }
        m_state->graph.reset();
    }
    if (m_state->backend)
    {
        auto backendResult = m_state->backend->shutdown();
        if (!backendResult.has_value() && !failure)
        {
            failure = *backendResult.try_error();
        }
        m_state->backend.reset();
    }
    if (m_state->window && !m_state->isDestroyed)
    {
        auto destroyResult = m_state->window->destroy();
        if (!destroyResult.has_value())
        {
            if (!failure)
            {
                failure = *destroyResult.try_error();
            }
        }
        if (destroyResult.has_value() && m_state->system)
        {
            auto pumpResult = m_state->system->pump_events();
            if (!pumpResult.has_value() && !failure)
            {
                failure = *pumpResult.try_error();
            }
            WindowEvent event{};
            while (m_state->window->try_pop_event(event))
            {
                if (event.type == WindowEventType::Destroyed)
                {
                    m_state->isDestroyed = true;
                }
            }
            if (pumpResult.has_value() &&
                (*pumpResult.try_value() != PumpStatus::QuitRequested || !m_state->isDestroyed) && !failure)
            {
                failure = Error{ErrorCategory::InvalidState, "WindowsHost.shutdown.quit"};
            }
        }
    }
    // System が借用する Window の参照を先に失効させる
    m_state->window.reset();
    m_state->system.reset();
    m_state->services = {};
    m_state.reset();
    return failure ? Result<void>::failure(*failure) : Result<void>::success();
}
} // namespace cue
