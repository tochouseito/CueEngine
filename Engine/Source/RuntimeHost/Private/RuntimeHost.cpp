#include <Cue/RuntimeHost/RuntimeHost.h>

#include <cstdio>
#include <optional>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <Cue/Platform/Windows/WindowsPlatform.h>

namespace cue
{
class RuntimeHost::State final
{
public:
    std::unique_ptr<WindowSystem> system;
    std::unique_ptr<Window> window;
    WindowsThreadServices services;
    std::unique_ptr<FrameController> controller;
    bool isCloseRequested = false;
    bool isDestroyed = false;
};

namespace
{
/// @brief 初期化失敗の後に発生したCleanup失敗もDebuggerへ残す
void report_cleanup_failure(const Error& a_error)
{
    char message[256]{};
    std::snprintf(message, sizeof(message), "CueRuntimeHost cleanup: %s (nativeCode=%lld)\n",
                  a_error.operation.c_str(), static_cast<long long>(a_error.nativeCode));
    OutputDebugStringA(message);
}
} // namespace

/// @brief 構築ThreadとHost設定を固定する
RuntimeHost::RuntimeHost(RuntimeHostDesc a_desc)
    : m_desc(std::move(a_desc)), m_ownerId(std::this_thread::get_id())
{
}

/// @brief 呼出側が明示停止を忘れてもWorkerから順に回収する
RuntimeHost::~RuntimeHost()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_cleanup_failure(*result.try_error());
    }
}

/// @brief WindowとServiceを構築し、開始前のCallbackを登録する
Result<void> RuntimeHost::initialize(FrameCallback a_update, FrameCallback a_render)
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "RuntimeHost.initialize"});
    }
    if (m_lifecycle != Lifecycle::Uninitialized)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "RuntimeHost.initialize"});
    }
    m_lifecycle = Lifecycle::Stopped;
    if (!a_update || !a_render)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "RuntimeHost.callbacks"});
    }

    // 部分初期化の失敗時は元Errorを返し、Cleanupの失敗は別の診断として残す
    auto rollback = [this](Error a_error) {
        auto cleanupResult = shutdown();
        if (!cleanupResult.has_value())
        {
            report_cleanup_failure(*cleanupResult.try_error());
        }
        return Result<void>::failure(std::move(a_error));
    };

    m_state = std::make_unique<State>();
    auto systemResult = create_windows_window_system();
    if (!systemResult.has_value())
    {
        return rollback(*systemResult.try_error());
    }
    m_state->system = systemResult.take_value();

    auto windowResult = m_state->system->create_window(m_desc.window);
    if (!windowResult.has_value())
    {
        return rollback(*windowResult.try_error());
    }
    m_state->window = windowResult.take_value();
    auto showResult = m_state->window->show();
    if (!showResult.has_value())
    {
        return rollback(*showResult.try_error());
    }

    auto servicesResult = create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return rollback(*servicesResult.try_error());
    }
    m_state->services = servicesResult.take_value();

    m_state->controller = std::make_unique<FrameController>(
        m_desc.frame, *m_state->services.clock, *m_state->services.waiter,
        *m_state->services.threadFactory);
    auto registerResult = m_state->controller->register_callbacks(std::move(a_update), std::move(a_render));
    if (!registerResult.has_value())
    {
        return rollback(*registerResult.try_error());
    }
    auto startResult = m_state->controller->start();
    if (!startResult.has_value())
    {
        return rollback(*startResult.try_error());
    }
    m_lifecycle = Lifecycle::Running;
    return Result<void>::success();
}

/// @brief MainThreadのWindow Eventを消費して継続可否を返す
Result<bool> RuntimeHost::pump_events()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<bool>::failure({ErrorCategory::WrongThread, "RuntimeHost.pump_events"});
    }
    if (m_lifecycle != Lifecycle::Running)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "RuntimeHost.pump_events"});
    }
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
    if (*pumpResult.try_value() == PumpStatus::QuitRequested && !m_state->isDestroyed)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "RuntimeHost.quit"});
    }
    if (m_state->isCloseRequested || m_state->isDestroyed ||
        *pumpResult.try_value() == PumpStatus::QuitRequested)
    {
        return Result<bool>::success(false);
    }
    if (m_desc.testFrameLimit != 0 &&
        m_state->controller->progress().renderedFrames >= m_desc.testFrameLimit)
    {
        return Result<bool>::success(false);
    }
    return Result<bool>::success(true);
}

/// @brief 実行中のFrameControllerをMainThreadへ借用する
FrameController& RuntimeHost::frame_controller() noexcept
{
    return *m_state->controller;
}

/// @brief 停止失敗があっても借用先とWindowを順に解放する
Result<void> RuntimeHost::shutdown()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "RuntimeHost.shutdown"});
    }
    m_lifecycle = Lifecycle::Stopped;
    if (!m_state)
    {
        return Result<void>::success();
    }

    std::optional<Error> failure;
    if (m_state->controller)
    {
        auto stopResult = m_state->controller->stop();
        if (!stopResult.has_value())
        {
            failure = *stopResult.try_error();
        }
        m_state->controller.reset();
    }
    m_state->services = {};

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
                failure = Error{ErrorCategory::InvalidState, "RuntimeHost.shutdown.quit"};
            }
        }
    }
    m_state->window.reset();
    m_state->system.reset();
    m_state.reset();
    return failure ? Result<void>::failure(*failure) : Result<void>::success();
}
} // namespace cue
