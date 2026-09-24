#include "WindowHost.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <stop_token>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace cue
{
/// @brief 初回stepに必要な引数だけを保持する
WindowHost::WindowHost(std::wstring_view a_options)
    : m_options(a_options)
{
}

/// @brief Workerを止めてから借用先を解放する
WindowHost::~WindowHost()
{
    [[maybe_unused]] auto shutdownResult = shutdown();
}

/// @brief Windows Hostの所有物を順番に生成して実行可能にする
Result<void> WindowHost::initialize()
{
    if (m_isStarted || !parse_options(m_options))
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "WindowHost.start"});
    }

    auto systemResult = create_windows_window_system();
    if (!systemResult.has_value())
    {
        return Result<void>::failure(*systemResult.try_error());
    }
    m_system = systemResult.take_value();

    const WindowDescriptor descriptor{"CueEngine Window Host", {1280, 720}};
    auto windowResult = m_system->create_window(descriptor);
    if (!windowResult.has_value())
    {
        return Result<void>::failure(*windowResult.try_error());
    }
    m_window = windowResult.take_value();
    auto showResult = m_window->show();
    if (!showResult.has_value())
    {
        return showResult;
    }

    auto servicesResult = create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return Result<void>::failure(*servicesResult.try_error());
    }
    m_services = servicesResult.take_value();

    // CallbackはHostの記録だけを更新し、WindowとSystemはMainThreadだけが操作する
    m_controller = std::make_unique<FrameController>(FrameControllerDesc{2, m_useWorkerThreads, 60},
                                                     *m_services.clock, *m_services.waiter,
                                                     *m_services.threadFactory);
    auto registerResult = m_controller->register_callbacks(
        [this](std::uint64_t, std::stop_token a_token) {
            const auto status = m_services.waiter->sleep_for(std::chrono::milliseconds(16), a_token);
            if (status == WaitStatus::Stopped)
            {
                return Result<void>::success();
            }
            if (status != WaitStatus::TimedOut)
            {
                return Result<void>::failure({ErrorCategory::InvalidState, "DummyUpdate.wait"});
            }
            return Result<void>::success();
        },
        [this](std::uint64_t, std::stop_token a_token) {
            const auto status = m_services.waiter->sleep_for(std::chrono::milliseconds(16), a_token);
            if (status == WaitStatus::Stopped)
            {
                return Result<void>::success();
            }
            if (status != WaitStatus::TimedOut)
            {
                return Result<void>::failure({ErrorCategory::InvalidState, "DummyRender.wait"});
            }
            return Result<void>::success();
        });
    if (!registerResult.has_value())
    {
        return registerResult;
    }
    auto startResult = m_controller->start();
    if (!startResult.has_value())
    {
        return startResult;
    }
    m_isStarted = true;
    return Result<void>::success();
}

/// @brief Window Messageと自動Testの終了条件を確認する
Result<bool> WindowHost::pump_events()
{
    if (!m_isStarted)
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "WindowHost.pump_events"});
    }
    auto pumpResult = m_system->pump_events();
    if (!pumpResult.has_value())
    {
        return Result<bool>::failure(*pumpResult.try_error());
    }

    WindowEvent event{};
    while (m_window->try_pop_event(event))
    {
        if (event.type == WindowEventType::CloseRequested)
        {
            m_isCloseRequested = true;
        }
        else if (event.type == WindowEventType::Destroyed)
        {
            m_isDestroyed = true;
        }
    }

    if (*pumpResult.try_value() == PumpStatus::QuitRequested)
    {
        if (!m_isDestroyed)
        {
            return Result<bool>::failure({ErrorCategory::InvalidState, "WindowHost.quit"});
        }
        return Result<bool>::success(false);
    }
    if (m_isCloseRequested)
    {
        return Result<bool>::success(false);
    }
    const auto progress = m_controller->progress();
    if (progress.renderedFrames >= m_nextReportFrame ||
        (m_testFrameLimit != 0 && progress.renderedFrames >= m_testFrameLimit))
    {
        report_progress(progress);
        m_nextReportFrame = progress.renderedFrames + 60;
    }
    if (m_testFrameLimit != 0 && progress.renderedFrames >= m_testFrameLimit)
    {
        // 自動Testは両Callbackと待機時間を確認してからMainのstepを止める
        if (progress.updatedFrames < m_testFrameLimit || progress.renderedFrames < m_testFrameLimit ||
            progress.lastUpdateDuration < std::chrono::milliseconds(15) ||
            progress.lastRenderDuration < std::chrono::milliseconds(15))
        {
            return Result<bool>::failure({ErrorCategory::InvalidState, "WindowHost.test.frames"});
        }
        m_isCloseRequested = true;
        return Result<bool>::success(false);
    }
    return Result<bool>::success(true);
}

/// @brief 初期化から終了まで有効なControllerを借用する
FrameController& WindowHost::frame_controller() noexcept
{
    return *m_controller;
}

/// @brief Worker停止後にWindowを破棄し、DestroyedとQuitを確認する
Result<void> WindowHost::shutdown()
{
    std::optional<Error> failure;
    if (m_controller)
    {
        auto stopResult = m_controller->stop();
        if (!stopResult.has_value())
        {
            failure = *stopResult.try_error();
        }
        m_controller.reset();
    }
    if (m_window && !m_isDestroyed)
    {
        auto destroyResult = m_window->destroy();
        if (!destroyResult.has_value() && !failure)
        {
            failure = *destroyResult.try_error();
        }
        if (destroyResult.has_value() && m_system)
        {
            auto pumpResult = m_system->pump_events();
            if (!pumpResult.has_value() && !failure)
            {
                failure = *pumpResult.try_error();
            }
            WindowEvent event{};
            while (m_window->try_pop_event(event))
            {
                if (event.type == WindowEventType::Destroyed)
                {
                    m_isDestroyed = true;
                }
            }
            if (pumpResult.has_value() &&
                (*pumpResult.try_value() != PumpStatus::QuitRequested || !m_isDestroyed) && !failure)
            {
                failure = Error{ErrorCategory::InvalidState, "WindowHost.shutdown.quit"};
            }
        }
    }
    m_window.reset();
    m_system.reset();
    m_isStarted = false;
    return failure ? Result<void>::failure(*failure) : Result<void>::success();
}

/// @brief Frame数、経過時間、Thread識別をDebuggerへ残す
void WindowHost::report_progress(const FrameProgress& a_progress)
{
    const double fps = a_progress.lastFrameInterval.count() > 0
                           ? 1'000'000'000.0 / static_cast<double>(a_progress.lastFrameInterval.count())
                           : 0.0;
    char message[256]{};
    std::snprintf(message, sizeof(message),
                  "CueWindowHost: submitted=%llu update=%llu render=%llu fps=%.2f updateMs=%.3f renderMs=%.3f "
                  "updateThread=%zu renderThread=%zu lastUpdate=%llu lastRender=%llu\n",
                  static_cast<unsigned long long>(a_progress.submittedFrames),
                  static_cast<unsigned long long>(a_progress.updatedFrames),
                  static_cast<unsigned long long>(a_progress.renderedFrames),
                  fps,
                  static_cast<double>(a_progress.lastUpdateDuration.count()) / 1'000'000.0,
                  static_cast<double>(a_progress.lastRenderDuration.count()) / 1'000'000.0,
                  std::hash<std::thread::id>{}(a_progress.updateThreadId),
                  std::hash<std::thread::id>{}(a_progress.renderThreadId),
                  static_cast<unsigned long long>(a_progress.lastUpdateFrame),
                  static_cast<unsigned long long>(a_progress.lastRenderFrame));
    OutputDebugStringA(message);
}

/// @brief Test引数以外を拒否し、単一Thread fallbackを選択できるようにする
bool WindowHost::parse_options(std::wstring_view a_options)
{
    constexpr std::wstring_view k_prefix = L"--test-frames=";
    bool hasSingleThreadOption = false;
    while (!a_options.empty())
    {
        while (!a_options.empty() && a_options.front() == L' ')
        {
            a_options.remove_prefix(1);
        }
        if (a_options.empty())
        {
            break;
        }
        const auto separator = a_options.find(L' ');
        const auto option = a_options.substr(0, separator);
        if (option == L"--single-thread" && !hasSingleThreadOption)
        {
            m_useWorkerThreads = false;
            hasSingleThreadOption = true;
        }
        else if (option.starts_with(k_prefix) && m_testFrameLimit == 0)
        {
            std::uint64_t frameLimit = 0;
            for (wchar_t digit : option.substr(k_prefix.size()))
            {
                if (digit < L'0' || digit > L'9' || frameLimit > 1000)
                {
                    return false;
                }
                frameLimit = frameLimit * 10 + static_cast<std::uint64_t>(digit - L'0');
            }
            if (frameLimit == 0 || frameLimit > 1000)
            {
                return false;
            }
            m_testFrameLimit = frameLimit;
        }
        else
        {
            return false;
        }
        a_options.remove_prefix(separator == std::wstring_view::npos ? a_options.size() : separator + 1);
    }
    return true;
}
} // namespace cue
