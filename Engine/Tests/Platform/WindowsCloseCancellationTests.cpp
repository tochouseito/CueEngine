#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Platform/Windows/WindowsPlatform.h>

namespace
{
/// @brief Event を消費し、この Close 要求が一度だけ通知されたかを返す
[[nodiscard]] bool consume_close_event(cue::Window &a_window)
{
    cue::WindowEvent event;
    std::size_t closeCount = 0;
    while (a_window.try_pop_event(event))
    {
        closeCount += event.type == cue::WindowEventType::CloseRequested ? 1 : 0;
    }
    return closeCount == 1;
}
} // namespace

/// @brief Close 取り消し、再要求、生成 Thread 制約と破棄済み Window の拒否を検証する
int main()
{
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 1;
    }
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"Close Cancellation Test", {320, 240}});
    if (!windowResult.has_value())
    {
        return 2;
    }
    auto window = windowResult.take_value();
    auto handleResult = cue::borrow_windows_window_handle(*window);
    if (!handleResult.has_value())
    {
        return 3;
    }
    const auto handle = static_cast<HWND>(handleResult.take_value());

    // まだ表示していない Window では取り消し後も Created を維持する
    SendMessageW(handle, WM_CLOSE, 0, 0);
    if (window->state() != cue::WindowState::CloseRequested || !consume_close_event(*window) ||
        !window->cancel_close_request().has_value() || window->state() != cue::WindowState::Created ||
        !window->cancel_close_request().has_value())
    {
        return 4;
    }
    // 取り消し後の Close は再び通知し、重複 Close は同じ Event を増やさない
    SendMessageW(handle, WM_CLOSE, 0, 0);
    SendMessageW(handle, WM_CLOSE, 0, 0);
    if (!consume_close_event(*window))
    {
        return 5;
    }
    bool rejectedThread = false;
    std::thread worker(
        [&]()
        {
            const auto result = window->cancel_close_request();
            rejectedThread = !result.has_value() && result.try_error()->category == cue::ErrorCategory::WrongThread;
        });
    worker.join();
    if (!rejectedThread || window->state() != cue::WindowState::CloseRequested ||
        !window->cancel_close_request().has_value())
    {
        return 6;
    }
    // 破棄済みの Native Window を再び生存状態へ戻すことは許可しない
    if (!window->destroy().has_value())
    {
        return 7;
    }
    const auto destroyedResult = window->cancel_close_request();
    if (destroyedResult.has_value() || destroyedResult.try_error()->category != cue::ErrorCategory::InvalidState ||
        window->state() != cue::WindowState::Destroyed)
    {
        return 8;
    }
    [[maybe_unused]] auto finalPump = system->pump_events();
    return 0;
}
