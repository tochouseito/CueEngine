#include <Cue/Platform/Windows/WindowsPlatform.h>

#include <memory>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/// @brief 生成失敗後の再試行、実WindowのClose経路とClass解放を確認する
int main()
{
    // 入力不正で失敗しても同じ System で生成を再試行できる
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 1;
    }
    auto system = systemResult.take_value();

    const cue::WindowDescriptor invalid{"", {640, 480}};
    auto invalidResult = system->create_window(invalid);
    if (invalidResult.has_value() || !invalidResult.try_error() ||
        invalidResult.try_error()->category != cue::ErrorCategory::InvalidArgument)
    {
        return 2;
    }

    // 不正 UTF-8 の Title は Native Window 生成前に拒否する
    const cue::WindowDescriptor invalidTitle{std::string{"\xC0\xAF", 2}, {640, 480}};
    auto invalidTitleResult = system->create_window(invalidTitle);
    if (invalidTitleResult.has_value() || !invalidTitleResult.try_error() ||
        invalidTitleResult.try_error()->category != cue::ErrorCategory::InvalidArgument)
    {
        return 20;
    }

    // 有効な Descriptor で Window を一つ生成し、初期状態と Size を確認する
    const cue::WindowDescriptor descriptor{"CueEngine Window Lifecycle", {640, 480}};
    auto windowResult = system->create_window(descriptor);
    if (!windowResult.has_value())
    {
        return 3;
    }
    auto window = windowResult.take_value();
    if (window->state() != cue::WindowState::Created || window->client_size().width != 640 ||
        window->client_size().height != 480)
    {
        return 4;
    }

    // 単一 Main Window 契約により二つ目の生成を拒否する
    auto secondWindow = system->create_window(descriptor);
    if (secondWindow.has_value() || !secondWindow.try_error() ||
        secondWindow.try_error()->category != cue::ErrorCategory::InvalidState)
    {
        return 5;
    }

    // Native HWND の Client Area も要求した Size と一致する
    const HWND handle = FindWindowW(nullptr, L"CueEngine Window Lifecycle");
    if (!handle || GetWindowThreadProcessId(handle, nullptr) != GetCurrentThreadId())
    {
        return 6;
    }
    RECT clientRect{};
    if (!GetClientRect(handle, &clientRect) || clientRect.right - clientRect.left != 640 ||
        clientRect.bottom - clientRect.top != 480)
    {
        return 7;
    }

    wchar_t className[128]{};
    if (!GetClassNameW(handle, className, 128))
    {
        return 8;
    }

    // 表示後の初期 Event を取り除き、以降の Message を個別に確認する
    if (!window->show().has_value() || !IsWindowVisible(handle))
    {
        return 9;
    }
    cue::WindowEvent event{};
    while (window->try_pop_event(event))
    {
    }

    // 最小化と復元は順序を保った Event として届く
    SendMessageW(handle, WM_SIZE, SIZE_MINIMIZED, 0);
    SendMessageW(handle, WM_SIZE, SIZE_RESTORED, 0);
    if (!window->try_pop_event(event) || event.type != cue::WindowEventType::Minimized ||
        !window->try_pop_event(event) || event.type != cue::WindowEventType::Restored ||
        event.clientSize.width != 640 || event.clientSize.height != 480)
    {
        return 10;
    }

    // 重複 Close 要求は一件だけ通知し、Window は Host の破棄まで残る
    SendMessageW(handle, WM_CLOSE, 0, 0);
    SendMessageW(handle, WM_CLOSE, 0, 0);
    if (window->state() != cue::WindowState::CloseRequested || !IsWindow(handle) ||
        !window->try_pop_event(event) || event.type != cue::WindowEventType::CloseRequested ||
        window->try_pop_event(event))
    {
        return 11;
    }

    // 明示破棄は Destroyed を通知し、二度目も成功する
    if (!window->destroy().has_value() || window->state() != cue::WindowState::Destroyed ||
        IsWindow(handle) || !window->try_pop_event(event) || event.type != cue::WindowEventType::Destroyed ||
        !window->destroy().has_value())
    {
        return 12;
    }

    // WM_QUIT を Pump する前でも破棄済み Main Window の再生成を防ぐ
    auto afterDestroy = system->create_window(descriptor);
    if (afterDestroy.has_value() || !afterDestroy.try_error() ||
        afterDestroy.try_error()->category != cue::ErrorCategory::InvalidState)
    {
        return 15;
    }

    // Quit を受けた後、Window から順に所有物を解放して Class 解除を確認する
    auto pumpResult = system->pump_events();
    if (!pumpResult.has_value() || *pumpResult.try_value() != cue::PumpStatus::QuitRequested)
    {
        return 13;
    }

    window.reset();
    system.reset();
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    if (GetClassInfoExW(GetModuleHandleW(nullptr), className, &windowClass))
    {
        return 14;
    }

    // 外部から DestroyWindow された場合も Owner の状態と借用を解除する
    auto externalSystemResult = cue::create_windows_window_system();
    if (!externalSystemResult.has_value())
    {
        return 16;
    }
    auto externalSystem = externalSystemResult.take_value();
    const cue::WindowDescriptor externalDescriptor{"CueEngine External Destroy", {320, 240}};
    auto externalWindowResult = externalSystem->create_window(externalDescriptor);
    if (!externalWindowResult.has_value())
    {
        return 17;
    }
    auto externalWindow = externalWindowResult.take_value();
    const HWND externalHandle = FindWindowW(nullptr, L"CueEngine External Destroy");
    if (!externalHandle || !DestroyWindow(externalHandle) ||
        externalWindow->state() != cue::WindowState::Destroyed)
    {
        return 18;
    }
    externalWindow.reset();
    auto externalPump = externalSystem->pump_events();
    if (!externalPump.has_value() || *externalPump.try_value() != cue::PumpStatus::QuitRequested)
    {
        return 19;
    }
    externalSystem.reset();
    return 0;
}
