#include <Cue/WindowsHost/WindowsHost.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stop_token>

namespace
{
/// @brief 部分初期化失敗と二重停止後に新しいHostを構築できることを確認する
int test_failed_initialization()
{
    // Callback 不足で停止しても二重 Shutdown と再初期化の判定を確認する
    cue::WindowsHost invalidCallbacks({{"CueWindowsHost Test", {320, 240}}, {2, true, 0}});
    auto invalidResult = invalidCallbacks.initialize({}, {});
    if (invalidResult.has_value() || invalidResult.try_error()->category != cue::ErrorCategory::InvalidArgument ||
        !invalidCallbacks.shutdown().has_value() || !invalidCallbacks.shutdown().has_value())
    {
        return 1;
    }
    if (invalidCallbacks.initialize({}, {}).has_value())
    {
        return 2;
    }

    // Window Descriptor と Frame 設定の失敗を別々に確認する
    cue::WindowsHost invalidWindow({{"", {320, 240}}, {2, true, 0}});
    auto success = [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); };
    auto windowResult = invalidWindow.initialize(success, success);
    if (windowResult.has_value() || windowResult.try_error()->category != cue::ErrorCategory::InvalidArgument ||
        !invalidWindow.shutdown().has_value())
    {
        return 3;
    }
    cue::WindowsHost invalidFrame({{"CueWindowsHost Invalid Frame", {320, 240}}, {0, true, 0}});
    auto frameResult = invalidFrame.initialize(success, success);
    if (frameResult.has_value() || frameResult.try_error()->category != cue::ErrorCategory::InvalidArgument ||
        !invalidFrame.shutdown().has_value())
    {
        return 4;
    }
    return 0;
}

/// @brief Callbackの差替え、Frame進行、停止を両Thread構成で確認する
int test_frame_callbacks(bool a_useWorkerThreads)
{
    // Worker 構成と単一 Thread 構成で同じ進行条件を検証する
    std::atomic<std::uint64_t> updates = 0;
    std::atomic<std::uint64_t> renders = 0;
    cue::WindowsHost host({{"CueWindowsHost Test", {320, 240}}, {2, a_useWorkerThreads, 0}});
    auto initResult = host.initialize(
        [&](std::uint64_t, std::stop_token) {
            ++updates;
            return cue::Result<void>::success();
        },
        [&](std::uint64_t, std::stop_token) {
            ++renders;
            return cue::Result<void>::success();
        });
    if (!initResult.has_value())
    {
        return 1;
    }

    // 実 Window が閉じない場合でも期限を設けて Test を終える
    bool reachedLimit = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto stepResult = host.step();
        if (!stepResult.has_value())
        {
            return 2;
        }
        auto progressResult = host.progress();
        if (!progressResult.has_value())
        {
            return 3;
        }
        if (!*stepResult.try_value() || progressResult.try_value()->renderedFrames >= 4)
        {
            reachedLimit = true;
            break;
        }
    }
    auto progressResult = host.progress();
    if (!progressResult.has_value())
    {
        return 3;
    }
    const auto progress = progressResult.take_value();
    auto shutdownResult = host.shutdown();
    // 停止後も進行値は取得済みの Snapshot で検証する
    if (!reachedLimit || progress.updatedFrames < 4 || progress.renderedFrames < 4 ||
        updates.load() < 4 || renders.load() < 4 || !shutdownResult.has_value() ||
        !host.shutdown().has_value())
    {
        return 4;
    }
    if (host.step().has_value())
    {
        return 5;
    }
    return 0;
}

/// @brief Worker Callback失敗を最初のErrorとして保持して停止する
int test_callback_failure()
{
    // Render Callback の Error が Step と Shutdown の両方に残ることを確認する
    cue::WindowsHost host({{"CueWindowsHost Failure", {320, 240}}, {2, true, 0}});
    auto initResult = host.initialize(
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); },
        [](std::uint64_t, std::stop_token) {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.render.failure"});
        });
    if (!initResult.has_value())
    {
        return 1;
    }
    bool sawFailure = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto result = host.step();
        if (!result.has_value())
        {
            sawFailure = result.try_error()->operation == "Test.render.failure";
            break;
        }
    }
    auto shutdownResult = host.shutdown();
    if (!sawFailure || shutdownResult.has_value() ||
        shutdownResult.try_error()->operation != "Test.render.failure" ||
        !host.shutdown().has_value())
    {
        return 2;
    }
    return 0;
}
} // namespace

/// @brief 実Window上のWindowsHostが失敗後も資源を残さないことを確認する
int main()
{
    if (const int result = test_failed_initialization(); result != 0)
    {
        return 10 + result;
    }
    if (const int result = test_frame_callbacks(true); result != 0)
    {
        return 20 + result;
    }
    if (const int result = test_frame_callbacks(false); result != 0)
    {
        return 30 + result;
    }
    if (const int result = test_callback_failure(); result != 0)
    {
        return 40 + result;
    }
    return 0;
}
