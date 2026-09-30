#include <WindowsHost/WindowsHost.h>

#include <optional>
#include <utility>

#include <Platform/Diagnostics.h>
#include <Platform/Windows/WindowsPlatform.h>
#include <Platform/WindowSystem.h>
#include <RHI/BackendFactory.h>

namespace cue
{
class WindowsHost::State final
{
public:
    std::unique_ptr<WindowSystem> system;
    std::unique_ptr<Window> window;
    std::unique_ptr<IBackend> backend;
    bool isCloseRequested = false;
    bool isDestroyed = false;
};

/// @brief Window 設定と構築 Thread を固定する
WindowsHost::WindowsHost(WindowDescriptor a_descriptor)
    : m_descriptor(std::move(a_descriptor)), m_ownerId(std::this_thread::get_id())
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

/// @brief Windows の Window と Renderer Backend を順に構築する
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

    // 部分初期化の失敗時は元の Error を返し、Cleanup の失敗は別に診断する
    auto rollback = [this](Error a_error) {
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
    auto windowResult = m_state->system->create_window(m_descriptor);
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

    // GPU 初期化に成功した後で Window を表示する
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
        }
        else if (event.type == WindowEventType::Destroyed)
        {
            m_state->isDestroyed = true;
        }
    }

    // Window が生存したままの WM_QUIT は所有契約に反する
    if (*pumpResult.try_value() == PumpStatus::QuitRequested && !m_state->isDestroyed)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "WindowsHost.quit"});
    }
    return Result<bool>::success(!m_state->isCloseRequested && !m_state->isDestroyed &&
                                 *pumpResult.try_value() != PumpStatus::QuitRequested);
}

/// @brief Backend を停止してから Window を破棄し、Destroyed と Quit の通知まで処理する
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

    // 最初の失敗を保持しつつ、Backend、Window、System の解放を続ける
    std::optional<Error> failure;
    if (m_state->backend)
    {
        auto backendResult = m_state->backend->shutdown();
        if (!backendResult.has_value())
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
    m_state.reset();
    return failure ? Result<void>::failure(*failure) : Result<void>::success();
}
} // namespace cue
