#include <Cue/Platform/Windows/WindowsPlatform.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
/// @brief 失敗した処理とNative診断値をDebuggerへ渡して終了Codeを返す
int report_failure(int a_exitCode, const cue::Error* a_error)
{
    // Error がある場合は終了 Code に加えて Native 診断を Debugger へ残す
    if (a_error)
    {
        char message[256]{};
        std::snprintf(message, sizeof(message), "CueWindowHost: %s (nativeCode=%lld)\n",
                      a_error->operation.c_str(), static_cast<long long>(a_error->nativeCode));
        OutputDebugStringA(message);
    }
    return a_exitCode;
}

/// @brief Window生成から終了までをHostが所有して進める
int run_window_host()
{
    // System より Window を先に破棄できるよう所有順を定める
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return report_failure(1, systemResult.try_error());
    }
    auto system = systemResult.take_value();

    // Window 生成までは非表示にし、成功後に表示する
    const cue::WindowDescriptor descriptor{"CueEngine Window Host", {1280, 720}};
    auto windowResult = system->create_window(descriptor);
    if (!windowResult.has_value())
    {
        return report_failure(2, windowResult.try_error());
    }
    auto window = windowResult.take_value();
    auto showResult = window->show();
    if (!showResult.has_value())
    {
        return report_failure(3, showResult.try_error());
    }

    // Close 要求を受けた Host が明示的に Window を破棄する
    bool isDestroyed = false;
    while (true)
    {
        auto pumpResult = system->pump_events();
        if (!pumpResult.has_value())
        {
            return report_failure(4, pumpResult.try_error());
        }

        cue::WindowEvent event{};
        while (window->try_pop_event(event))
        {
            if (event.type == cue::WindowEventType::CloseRequested)
            {
                auto destroyResult = window->destroy();
                if (!destroyResult.has_value())
                {
                    return report_failure(5, destroyResult.try_error());
                }
            }
            else if (event.type == cue::WindowEventType::Destroyed)
            {
                isDestroyed = true;
            }
        }

        // Destroyed を確認してから正常終了と判断する
        if (*pumpResult.try_value() == cue::PumpStatus::QuitRequested)
        {
            return isDestroyed ? 0 : 6;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
} // namespace

/// @brief WindowsのWindow Hostを起動し、失敗時は非0で終了する
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Host 境界で予期しない C++ 例外を終了 Code に変換する
    try
    {
        return run_window_host();
    }
    catch (const std::exception&)
    {
        OutputDebugStringW(L"CueWindowHost: unexpected C++ exception\n");
        return 7;
    }
}
