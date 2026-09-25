#include <Cue/WindowsHost/WindowsHost.h>

#include <optional>
#include <utility>

#include <Cue/Platform/Diagnostics.h>
#include <Cue/Platform/Windows/WindowsPlatform.h>
#include <Cue/Renderer/D3D12/D3D12Renderer.h>

namespace cue
{
class WindowsHost::State final
{
public:
    std::unique_ptr<WindowSystem> system;
    std::unique_ptr<Window> window;
    WindowsThreadServices services;
    std::unique_ptr<D3D12Renderer> renderer;
    std::unique_ptr<Runtime> runtime;
    bool isCloseRequested = false;
    bool isDestroyed = false;
};

/// @brief Host設定と構築Threadを固定する
WindowsHost::WindowsHost(WindowsHostDesc a_desc)
    : m_desc(std::move(a_desc)), m_ownerId(std::this_thread::get_id())
{
}

/// @brief 明示停止がない場合もRuntimeとWindowを順に回収する
WindowsHost::~WindowsHost()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_error("CueWindowsHost cleanup", *result.try_error(), DiagnosticSeverity::Error);
    }
}

/// @brief Windows資源を構築して共通Runtimeを開始する
Result<void> WindowsHost::initialize(FrameCallback a_update, FrameCallback a_render)
{
    return initialize_impl(std::move(a_update), std::move(a_render), false);
}

/// @brief 製品用RendererのFrame処理をHost初期化中に接続する
Result<void> WindowsHost::initialize_renderer(FrameCallback a_update)
{
    return initialize_impl(std::move(a_update), {}, true);
}

/// @brief 共通のWindow・Service・Runtime初期化をRenderer有無で組み立てる
Result<void> WindowsHost::initialize_impl(FrameCallback a_update, FrameCallback a_render, bool a_createRenderer)
{
    // WindowsHost は構築 Thread でしか操作できない
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "WindowsHost.initialize"});
    }

    // WindowsHost は一度しか初期化できない
    if (m_lifecycle != Lifecycle::Uninitialized)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "WindowsHost.initialize"});
    }
    m_lifecycle = Lifecycle::Stopped;

    // Callback がない場合は Runtime を開始できない
    if (!a_update || (!a_createRenderer && !a_render))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "WindowsHost.callbacks"});
    }

    // 部分初期化の失敗時は元の Error を返し、Cleanup の失敗は別の診断として残す
    auto rollback = [this](Error a_error) {
        auto cleanupResult = shutdown();
        if (!cleanupResult.has_value())
        {
            report_error("CueWindowsHost cleanup", *cleanupResult.try_error(), DiagnosticSeverity::Error);
        }
        return Result<void>::failure(std::move(a_error));
    };

    // WindowsHost は Window、Service、Runtime を所有し、Runtime は Service を借用する
    m_state = std::make_unique<State>();

    // WindowSystem は Window より長く生存させるため、最初に生成する
    auto systemResult = create_windows_window_system();
    if (!systemResult.has_value())
    {
        return rollback(*systemResult.try_error());
    }
    m_state->system = systemResult.take_value();

    // Descriptor から Window を作り、表示に失敗した場合も所有先から回収する
    auto windowResult = m_state->system->create_window(m_desc.window);
    if (!windowResult.has_value())
    {
        return rollback(*windowResult.try_error());
    }
    m_state->window = windowResult.take_value();

    // Window が公開された後の失敗は shutdown で破棄する
    auto showResult = m_state->window->show();
    if (!showResult.has_value())
    {
        return rollback(*showResult.try_error());
    }

    // FrameController が借用する Clock、Waiter、ThreadFactory をまとめて生成する
    auto servicesResult = create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return rollback(*servicesResult.try_error());
    }
    m_state->services = servicesResult.take_value();

    if (a_createRenderer)
    {
        // Native HandleはWindowが生存する間だけ借用し、D3D12型はRenderer実装へ閉じ込める
        auto handleResult = borrow_windows_window_handle(*m_state->window);
        if (!handleResult.has_value())
        {
            return rollback(*handleResult.try_error());
        }
        auto rendererResult = D3D12Renderer::create(handleResult.take_value(), m_state->window->client_size());
        if (!rendererResult.has_value())
        {
            return rollback(*rendererResult.try_error());
        }
        m_state->renderer = rendererResult.take_value();
        D3D12Renderer* renderer = m_state->renderer.get();
        a_render = [renderer](std::uint64_t a_frame, std::stop_token a_stopToken) {
            if (a_stopToken.stop_requested())
            {
                return Result<void>::success();
            }
            return renderer->render_frame(a_frame);
        };
    }

    // Runtime が借用する Service は、Runtime 停止後まで Host が保持する
    m_state->runtime = std::make_unique<Runtime>(m_desc.frame, *m_state->services.clock,
                                                 *m_state->services.waiter,
                                                 *m_state->services.threadFactory);

    // Callback 登録と Worker 開始が成功してから Running とする
    auto initResult = m_state->runtime->initialize(std::move(a_update), std::move(a_render));
    if (!initResult.has_value())
    {
        return rollback(*initResult.try_error());
    }
    m_lifecycle = Lifecycle::Running;
    return Result<void>::success();
}

/// @brief Windows Eventを消費し、終了要求がなければ共通Runtimeを進める
Result<bool> WindowsHost::step()
{
    // Window Event と Runtime 進行は、初期化した Thread に限定する
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "WindowsHost.step"});
    }
    if (m_lifecycle != Lifecycle::Running)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "WindowsHost.step"});
    }

    // Win32 Message を先に処理し、終了要求後の Frame 投入を防ぐ
    auto pumpResult = m_state->system->pump_events();
    if (!pumpResult.has_value())
    {
        return Result<bool>::failure(*pumpResult.try_error());
    }

    // Queue に積まれた Window Event を、この Step の終了判断へ反映する
    WindowEvent event{};
    while (m_state->window->try_pop_event(event))
    {
        if (event.type == WindowEventType::CloseRequested)
        {
            m_state->isCloseRequested = true;
        }
        else if (event.type == WindowEventType::Destroyed)
        {
            m_state->isDestroyed = true;
        }
    }

    // 公開済み Window の破棄を伴わない WM_QUIT は想定外とする
    if (*pumpResult.try_value() == PumpStatus::QuitRequested && !m_state->isDestroyed)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "WindowsHost.quit"});
    }

    // Close 要求、破棄、WM_QUIT のいずれかで Main に正常終了を返す
    if (m_state->isCloseRequested || m_state->isDestroyed ||
        *pumpResult.try_value() == PumpStatus::QuitRequested)
    {
        return Result<bool>::success(false);
    }

    // Window が継続中のときだけ Runtime の次の Frame を進める
    auto stepResult = m_state->runtime->step();
    if (!stepResult.has_value())
    {
        return Result<bool>::failure(*stepResult.try_error());
    }
    return Result<bool>::success(true);
}

/// @brief Hostが所有するRuntimeの進行状態を返す
Result<FrameProgress> WindowsHost::progress() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<FrameProgress>::failure({ErrorCategory::WrongThread, "WindowsHost.progress"});
    }
    if (m_lifecycle != Lifecycle::Running)
    {
        return Result<FrameProgress>::failure({ErrorCategory::InvalidState, "WindowsHost.progress"});
    }
    return m_state->runtime->progress();
}

/// @brief Runtime停止後にWindowを破棄しDestroyedとQuitを確認する
Result<void> WindowsHost::shutdown()
{
    // 構築 Thread 以外からの破棄は Win32 と Worker の所有契約に反する
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "WindowsHost.shutdown"});
    }
    m_lifecycle = Lifecycle::Stopped;
    if (!m_state)
    {
        return Result<void>::success();
    }

    // 最初の失敗を保持しつつ、後続の資源も必ず回収する
    std::optional<Error> failure;
    if (m_state->runtime)
    {
        auto stopResult = m_state->runtime->shutdown();
        if (!stopResult.has_value())
        {
            failure = *stopResult.try_error();
        }
        m_state->runtime.reset();
    }
    // Render Worker停止後にGPU完了を待ち、Windowより先にSwap Chainを解放する
    if (m_state->renderer)
    {
        auto rendererResult = m_state->renderer->shutdown();
        if (!rendererResult.has_value() && !failure)
        {
            failure = *rendererResult.try_error();
        }
        m_state->renderer.reset();
    }
    // Worker の join 完了後に借用されていた Service を解放する
    m_state->services = {};

    // Window が残る場合は明示破棄し、Destroy と Quit の通知まで消費する
    if (m_state->window && !m_state->isDestroyed)
    {
        auto destroyResult = m_state->window->destroy();
        if (!destroyResult.has_value() && !failure)
        {
            failure = *destroyResult.try_error();
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
    // Window を System より先に解放し、System の非所有参照を失効させる
    m_state->window.reset();
    m_state->system.reset();
    m_state.reset();
    return failure ? Result<void>::failure(*failure) : Result<void>::success();
}
} // namespace cue
