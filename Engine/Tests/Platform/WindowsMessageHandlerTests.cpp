#include <cstdint>
#include <stdexcept>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Platform/Windows/WindowsPlatform.h>

namespace
{
/// @brief 全 Message を消費しても必須 Event が残り、登録元だけが解除できることを確認する
int test_lifecycle()
{
    int called = 0;
    bool rejectedReentry = false;
    cue::WindowsMessageHandlerToken token;
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 1;
    }
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"Message Handler Test", {320, 240}});
    if (!windowResult.has_value())
    {
        return 2;
    }
    auto window = windowResult.take_value();
    auto handleResult = cue::borrow_windows_window_handle(*window);
    if (!handleResult.has_value())
    {
        return 2;
    }
    const auto handle = static_cast<HWND>(handleResult.take_value());
    auto registered = cue::register_windows_message_handler(
        *window,
        [&](const cue::WindowsMessage& a_message)
        {
            ++called;
            if (a_message.message == WM_APP + 1)
            {
                auto removed = cue::unregister_windows_message_handler(*window, token);
                auto destroyed = window->destroy();
                rejectedReentry = !removed.has_value() && !destroyed.has_value() &&
                                  removed.try_error()->category == cue::ErrorCategory::InvalidState &&
                                  destroyed.try_error()->category == cue::ErrorCategory::InvalidState;
            }
            return cue::WindowsMessageResult{true, 42};
        });
    if (!registered.has_value())
    {
        return 3;
    }
    token = registered.take_value();
    if (cue::register_windows_message_handler(*window, [](const auto&) { return cue::WindowsMessageResult{}; }).has_value() ||
        SendMessageW(handle, WM_APP + 1, 0, 0) != 42 || !rejectedReentry)
    {
        return 4;
    }
    bool rejectedThread = false;
    std::thread worker([&]()
    {
        auto removed = cue::unregister_windows_message_handler(*window, token);
        auto added = cue::register_windows_message_handler(*window, [](const auto&) { return cue::WindowsMessageResult{}; });
        rejectedThread = !removed.has_value() && !added.has_value() &&
                         removed.try_error()->category == cue::ErrorCategory::WrongThread &&
                         added.try_error()->category == cue::ErrorCategory::WrongThread;
    });
    worker.join();
    if (!rejectedThread)
    {
        return 5;
    }
    // UI が consumed と返しても最小化、復帰、Close の Event は必ず生成する
    cue::WindowEvent event;
    while (window->try_pop_event(event))
    {
    }
    SendMessageW(handle, WM_SIZE, SIZE_MINIMIZED, 0);
    SendMessageW(handle, WM_SIZE, SIZE_RESTORED, 0);
    SendMessageW(handle, WM_CLOSE, 0, 0);
    bool minimized = false;
    bool restored = false;
    bool closed = false;
    while (window->try_pop_event(event))
    {
        minimized |= event.type == cue::WindowEventType::Minimized;
        restored |= event.type == cue::WindowEventType::Restored;
        closed |= event.type == cue::WindowEventType::CloseRequested;
    }
    if (!minimized || !restored || !closed || window->state() != cue::WindowState::CloseRequested)
    {
        return 6;
    }
    auto stale = token;
    ++stale.generation;
    if (cue::unregister_windows_message_handler(*window, stale).has_value() ||
        !cue::unregister_windows_message_handler(*window, token).has_value() ||
        !cue::unregister_windows_message_handler(*window, token).has_value())
    {
        return 7;
    }
    const int before = called;
    SendMessageW(handle, WM_APP + 1, 0, 0);
    if (called != before)
    {
        return 8;
    }
    // 新しい登録世代を全消費にしても Destroy と Quit の通知を維持する
    auto replacement = cue::register_windows_message_handler(
        *window, [](const auto&) { return cue::WindowsMessageResult{true, 42}; });
    if (!replacement.has_value() || cue::unregister_windows_message_handler(*window, token).has_value() ||
        !window->destroy().has_value())
    {
        return 9;
    }
    auto pump = system->pump_events();
    if (!pump.has_value() || pump.take_value() != cue::PumpStatus::QuitRequested ||
        window->state() != cue::WindowState::Destroyed ||
        !cue::unregister_windows_message_handler(*window, replacement.take_value()).has_value())
    {
        return 10;
    }
    return 0;
}

/// @brief Window Procedure の外へ Callback 例外を出さず、Pump が Error を返すことを確認する
int test_exception()
{
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 1;
    }
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"Handler Exception", {320, 240}});
    if (!windowResult.has_value())
    {
        return 2;
    }
    auto window = windowResult.take_value();
    auto token = cue::register_windows_message_handler(*window, [](const cue::WindowsMessage&) -> cue::WindowsMessageResult
    {
        throw std::runtime_error("handler failure");
    });
    if (!token.has_value())
    {
        return 3;
    }
    auto handle = cue::borrow_windows_window_handle(*window);
    if (!handle.has_value())
    {
        return 3;
    }
    SendMessageW(static_cast<HWND>(handle.take_value()), WM_CLOSE, 0, 0);
    auto result = system->pump_events();
    if (result.has_value() || result.try_error()->operation != "WindowsMessageHandler" ||
        window->state() != cue::WindowState::CloseRequested)
    {
        return 4;
    }
    if (!cue::unregister_windows_message_handler(*window, token.take_value()).has_value() ||
        !window->destroy().has_value())
    {
        return 5;
    }
    // 次の System の検証へ WM_QUIT を残さない
    [[maybe_unused]] auto finalPump = system->pump_events();
    return 0;
}
} // namespace

/// @brief Handler の所有・解除と例外境界、UI 消費後の必須 Lifecycle を検証する
int main()
{
    if (const int result = test_lifecycle(); result != 0)
    {
        return 10 + result;
    }
    if (const int result = test_exception(); result != 0)
    {
        return 30 + result;
    }
    return 0;
}
