#include <WindowsHost/WindowsHost.h>

#include <atomic>
#include <thread>

namespace
{
/// @brief 不正な Window 設定で部分生成物を残さず停止することを確認する
int test_failed_initialization()
{
    cue::WindowsHost host({"", {320, 240}});
    auto result = host.initialize();
    if (result.has_value() || result.try_error()->category != cue::ErrorCategory::InvalidArgument)
    {
        return 1;
    }
    if (!host.shutdown().has_value() || !host.shutdown().has_value())
    {
        return 2;
    }
    if (host.initialize().has_value())
    {
        return 3;
    }
    return 0;
}

/// @brief Window の表示、Thread 制約、二重停止を確認する
int test_window_lifecycle()
{
    cue::WindowsHost host({"CueWindowsHost Test", {320, 240}});
    if (host.step().has_value())
    {
        return 1;
    }
    if (!host.initialize().has_value())
    {
        return 2;
    }
    if (host.initialize().has_value())
    {
        return 3;
    }

    std::atomic<bool> rejectedOtherThread = false;
    std::thread worker([&] {
        auto stepResult = host.step();
        auto stopResult = host.shutdown();
        rejectedOtherThread = !stepResult.has_value() &&
                              stepResult.try_error()->category == cue::ErrorCategory::WrongThread &&
                              !stopResult.has_value() &&
                              stopResult.try_error()->category == cue::ErrorCategory::WrongThread;
    });
    worker.join();
    if (!rejectedOtherThread)
    {
        return 4;
    }

    auto stepResult = host.step();
    if (!stepResult.has_value() || !*stepResult.try_value())
    {
        return 5;
    }
    if (!host.shutdown().has_value() || !host.shutdown().has_value())
    {
        return 6;
    }
    if (host.step().has_value())
    {
        return 7;
    }
    return 0;
}
} // namespace

/// @brief 実 Window 上で WindowsHost の生成と停止を検証する
int main()
{
    if (const int result = test_failed_initialization(); result != 0)
    {
        return 10 + result;
    }
    if (const int result = test_window_lifecycle(); result != 0)
    {
        return 20 + result;
    }
    return 0;
}
