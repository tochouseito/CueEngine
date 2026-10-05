#include <EditorHost/EditorHost.h>

#include <thread>
#include <utility>

namespace
{
/// @brief 初期化途中の失敗後も停止でき、同じ Host を再初期化しない
int test_failed_initialization()
{
    cue::EditorHostConfig config;
    config.window.title.clear();
    cue::EditorHost host(std::move(config));
    auto result = host.initialize();
    if (result.has_value() || result.try_error()->category != cue::ErrorCategory::InvalidArgument)
    {
        return 1;
    }
    if (!host.shutdown().has_value() || !host.shutdown().has_value())
    {
        return 2;
    }
    if (host.initialize().has_value() || host.step().has_value() || host.frame_progress().has_value())
    {
        return 3;
    }
    return 0;
}

/// @brief Editor の既定構成で描画を進め、UI Owner と同じ Thread 上の Frame 実行を確認する
int test_owner_thread_frames()
{
    cue::EditorHostConfig config;
    config.window.clientSize = {320, 240};
    config.frame.maxFps = 0;
    cue::EditorHost host(std::move(config));
    if (host.step().has_value() || host.frame_progress().has_value())
    {
        return 1;
    }
    auto initResult = host.initialize();
    if (!initResult.has_value() || host.initialize().has_value())
    {
        return 2;
    }
    for (int index = 0; index < 3; ++index)
    {
        auto stepResult = host.step();
        if (!stepResult.has_value() || !*stepResult.try_value())
        {
            return 3;
        }
    }
    auto progressResult = host.frame_progress();
    if (!progressResult.has_value())
    {
        return 4;
    }
    const auto &progress = *progressResult.try_value();
    const auto ownerId = std::this_thread::get_id();
    if (progress.submittedFrames != 3 || progress.updatedFrames != 3 || progress.renderedFrames != 3 ||
        progress.updateThreadId != ownerId || progress.renderThreadId != ownerId)
    {
        return 5;
    }

    bool rejectedOtherThread = false;
    std::thread worker(
        [&]()
        {
            auto initialize = host.initialize();
            auto step = host.step();
            auto progress = host.frame_progress();
            auto stop = host.shutdown();
            rejectedOtherThread = !initialize.has_value() && !step.has_value() && !progress.has_value() &&
                                  !stop.has_value() &&
                                  initialize.try_error()->category == cue::ErrorCategory::WrongThread &&
                                  step.try_error()->category == cue::ErrorCategory::WrongThread &&
                                  progress.try_error()->category == cue::ErrorCategory::WrongThread &&
                                  stop.try_error()->category == cue::ErrorCategory::WrongThread;
        });
    worker.join();
    if (!rejectedOtherThread)
    {
        return 6;
    }
    if (!host.shutdown().has_value() || !host.shutdown().has_value())
    {
        return 7;
    }
    if (host.step().has_value() || host.frame_progress().has_value())
    {
        return 8;
    }
    return 0;
}
} // namespace

/// @brief Editor 用の Host 基盤の異常系と実 Window 上の Frame 進行を確認する
int main()
{
    if (const int result = test_failed_initialization(); result != 0)
    {
        return 10 + result;
    }
    if (const int result = test_owner_thread_frames(); result != 0)
    {
        return 20 + result;
    }
    return 0;
}
