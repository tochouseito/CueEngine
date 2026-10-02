#include <WindowsHost/WindowsHost.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

namespace
{
/// @brief 不正な Window 設定で部分生成物を残さず停止することを確認する
int test_failed_initialization()
{
    cue::WindowsHost host({{"", {320, 240}}, {}, {}});
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

/// @brief 起動前に Frame 先行数と BackBuffer 数の不正値を拒否する
int test_invalid_config()
{
    cue::WindowsHostConfig invalidFrame{{"Invalid Frame", {320, 240}}, {}, {}};
    invalidFrame.frame.maxFramesInFlight = 0;
    cue::WindowsHost frameHost(std::move(invalidFrame));
    auto frameResult = frameHost.initialize();
    if (frameResult.has_value() || frameResult.try_error()->category != cue::ErrorCategory::InvalidArgument ||
        !frameHost.shutdown().has_value())
    {
        return 1;
    }

    cue::WindowsHostConfig invalidPresentation{{"Invalid Presentation", {320, 240}}, {}, {}};
    invalidPresentation.presentation.bufferCount = 1;
    cue::WindowsHost presentationHost(std::move(invalidPresentation));
    auto presentationResult = presentationHost.initialize();
    if (presentationResult.has_value() ||
        presentationResult.try_error()->category != cue::ErrorCategory::InvalidArgument ||
        !presentationHost.shutdown().has_value())
    {
        return 2;
    }
    return 0;
}

/// @brief Window の表示、Thread 制約、二重停止を確認する
int test_window_lifecycle()
{
    cue::WindowsHostConfig config{{"CueWindowsHost Test", {320, 240}}, {1, false, 0}, {3, true, false}};
    cue::WindowsHost host(std::move(config));
    if (host.step().has_value() || host.frame_progress().has_value())
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
    // 構成した単一 Thread の Frame 進行を Host から確認する
    auto secondStepResult = host.step();
    auto progressResult = host.frame_progress();
    if (!secondStepResult.has_value() || !*secondStepResult.try_value() ||
        !progressResult.has_value() || progressResult.try_value()->renderedFrames != 2)
    {
        return 8;
    }
    if (!host.shutdown().has_value() || !host.shutdown().has_value())
    {
        return 6;
    }
    if (host.step().has_value() || host.frame_progress().has_value())
    {
        return 7;
    }
    return 0;
}

/// @brief 既定の Worker 構成で複数回の描画提出と Present が完了する
int test_worker_frame_progress()
{
    cue::WindowsHost host({{"CueWindowsHost Worker Test", {320, 240}}, {}, {}});
    if (!host.initialize().has_value())
    {
        return 1;
    }

    bool hasRendered = false;
    for (int attempt = 0; attempt < 200 && !hasRendered; ++attempt)
    {
        auto stepResult = host.step();
        auto progressResult = host.frame_progress();
        if (!stepResult.has_value() || !*stepResult.try_value() || !progressResult.has_value())
        {
            return 2;
        }
        hasRendered = progressResult.try_value()->renderedFrames >= 3;
        if (!hasRendered)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (!host.shutdown().has_value())
    {
        return 3;
    }
    return hasRendered ? 0 : 4;
}
} // namespace

/// @brief 実 Window 上で WindowsHost の生成と停止を検証する
int main()
{
    if (const int result = test_failed_initialization(); result != 0)
    {
        return 10 + result;
    }
    if (const int result = test_invalid_config(); result != 0)
    {
        return 15 + result;
    }
    if (const int result = test_window_lifecycle(); result != 0)
    {
        return 20 + result;
    }
    if (const int result = test_worker_frame_progress(); result != 0)
    {
        return 30 + result;
    }
    return 0;
}
