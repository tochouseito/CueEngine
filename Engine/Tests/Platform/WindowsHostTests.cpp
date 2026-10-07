#include <WindowsHost/WindowsHost.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Passes/PresentToSwapChainPass.h>

namespace
{
/// @brief 再構築した表示 Pass の個数と GPU 記録時の実寸法を Thread 間で共有する
struct ResizePassStats final
{
    std::atomic<std::uint64_t> executions = 0;
    std::atomic<std::uint64_t> dimensions = 0;
    std::atomic<std::uint64_t> instances = 0;
};

/// @brief 標準表示 Pass の実描画を保ったまま Context の寸法を観測する
class ResizeDisplayPass final : public cue::FrameGraphPass
{
  public:
    /// @brief Test が所有する統計を Graph の寿命だけ借用する
    explicit ResizeDisplayPass(ResizePassStats &a_stats) noexcept : m_stats(&a_stats)
    {
    }

    /// @brief 表示 Pass の診断名を返す
    [[nodiscard]] const char *name() const noexcept override
    {
        return "ResizeDisplay";
    }

    /// @brief BackBuffer を使用する Graphics Queue を選ぶ
    [[nodiscard]] cue::QueueType type() const noexcept override
    {
        return cue::QueueType::Graphics;
    }

    /// @brief Graph ごとの Pipeline と Resource Handle を標準 Pass に構築させる
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder &a_builder) override
    {
        return m_present.setup(a_builder);
    }

    /// @brief 表示元と BackBuffer の Resource 宣言を標準 Pass に委ねる
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder &a_builder) override
    {
        return m_present.describe_resources(a_builder);
    }

    /// @brief 成功した GPU 記録だけを数え、Graph の寸法を原子的に公開する
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext &a_context) override
    {
        auto result = m_present.execute(a_context);
        if (result.has_value())
        {
            const auto dimensions = (static_cast<std::uint64_t>(a_context.width()) << 32) | a_context.height();
            m_stats->dimensions.store(dimensions);
            m_stats->executions.fetch_add(1);
        }
        return result;
    }

  private:
    ResizePassStats *m_stats = nullptr;
    cue::PresentToSwapChainPass m_present;
};

/// @brief Window の現在の Client Area を一つの比較値へ変換する
[[nodiscard]] std::uint64_t pack_size(cue::WindowSize a_size) noexcept
{
    return (static_cast<std::uint64_t>(a_size.width) << 32) | a_size.height;
}

/// @brief Win32 の確定寸法と Window の通知寸法に合う Pass が実行されるまで Pump を進める
[[nodiscard]] bool wait_for_display(cue::WindowsHost &a_host, const ResizePassStats &a_stats, HWND a_handle,
                                    const cue::Window &a_window, std::uint64_t a_afterExecution,
                                    std::uint64_t a_minInstances)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto step = a_host.step();
        if (!step.has_value() || !*step.try_value())
        {
            return false;
        }
        RECT client{};
        auto progress = a_host.frame_progress();
        if (!GetClientRect(a_handle, &client) || !progress.has_value())
        {
            return false;
        }
        const cue::WindowSize actual{static_cast<std::uint32_t>(client.right - client.left),
                                     static_cast<std::uint32_t>(client.bottom - client.top)};
        if (actual.width > 0 && actual.height > 0 && pack_size(a_window.client_size()) == pack_size(actual) &&
            a_stats.executions.load() > a_afterExecution && a_stats.dimensions.load() == pack_size(actual) &&
            a_stats.instances.load() >= a_minInstances && progress.try_value()->updateThreadId != std::thread::id{} &&
            progress.try_value()->renderThreadId != std::thread::id{})
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief 最小化で新規 Frame が止まっても既存の CPU Callback 完了だけを待つ
[[nodiscard]] bool wait_for_idle(cue::WindowsHost &a_host, HWND a_handle)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto step = a_host.step();
        auto progress = a_host.frame_progress();
        if (!step.has_value() || !*step.try_value() || !progress.has_value())
        {
            return false;
        }
        if (IsIconic(a_handle) && progress.try_value()->updatedFrames == progress.try_value()->submittedFrames &&
            progress.try_value()->renderedFrames == progress.try_value()->submittedFrames)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

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
    cue::WindowsHost *borrowed = nullptr;
    int mainCalls = 0;
    const auto ownerId = std::this_thread::get_id();
    config.callbacks.main = [&](std::uint64_t a_frame, std::stop_token)
    {
        auto nested = borrowed->step();
        auto stopped = borrowed->shutdown();
        auto progress = borrowed->frame_progress();
        if (std::this_thread::get_id() != ownerId || nested.has_value() || stopped.has_value() ||
            !progress.has_value() || progress.try_value()->submittedFrames != a_frame ||
            nested.try_error()->category != cue::ErrorCategory::InvalidState ||
            stopped.try_error()->category != cue::ErrorCategory::InvalidState)
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.host.main"});
        }
        ++mainCalls;
        return cue::Result<void>::success();
    };
    cue::WindowsHost host(std::move(config));
    borrowed = &host;
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
    if (!secondStepResult.has_value() || !*secondStepResult.try_value() || !progressResult.has_value() ||
        progressResult.try_value()->renderedFrames != 2 || mainCalls != 2)
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && !hasRendered)
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

/// @brief 上位機能が借用を解除できなかった停止では Window を保持し、次の停止で回収する
int test_callback_shutdown_retry()
{
    cue::Window* borrowed = nullptr;
    int attempts = 0;
    cue::WindowsHostConfig config{{"Shutdown Callback Test", {320, 240}}, {1, false, 0}, {}};
    config.callbacks.initializeWindow = [&](cue::Window& a_window)
    {
        borrowed = &a_window;
        return cue::Result<void>::success();
    };
    config.callbacks.shutdownWindow = [&]()
    {
        ++attempts;
        return attempts == 1
                   ? cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.shutdown_pending"})
                   : cue::Result<void>::success();
    };
    cue::WindowsHost host(std::move(config));
    if (!host.initialize().has_value())
    {
        return 1;
    }
    auto first = host.shutdown();
    if (first.has_value() || first.try_error()->operation != "Test.shutdown_pending" || attempts != 1 || !borrowed ||
        borrowed->state() != cue::WindowState::Visible || host.step().has_value())
    {
        return 2;
    }
    if (!host.shutdown().has_value() || attempts != 2 || !host.shutdown().has_value() || attempts != 2)
    {
        return 3;
    }
    return 0;
}

/// @brief 最大化、最小化、復帰と連続 Resize を実 Window から送り GPU 表示寸法を確認する
int test_resize_presentation(bool a_usesWorkers)
{
    ResizePassStats stats;
    cue::Window *borrowed = nullptr;
    const auto ownerId = std::this_thread::get_id();
    cue::WindowsHostConfig config{
        {a_usesWorkers ? "CueWindowsHost Resize Worker" : "CueWindowsHost Resize Single", {320, 240}},
        {1, a_usesWorkers, 0},
        {3, false, false}};
    config.callbacks.initializeWindow = [&](cue::Window &a_window)
    {
        borrowed = &a_window;
        return a_window.state() == cue::WindowState::Created
                   ? cue::Result<void>::success()
                   : cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.resize.window"});
    };
    config.graph.displayPassFactory = [&]() -> std::unique_ptr<cue::FrameGraphPass>
    {
        stats.instances.fetch_add(1);
        return std::make_unique<ResizeDisplayPass>(stats);
    };
    cue::WindowsHost host(std::move(config));
    if (!host.initialize().has_value() || !borrowed)
    {
        return 1;
    }
    const HWND handle =
        FindWindowW(nullptr, a_usesWorkers ? L"CueWindowsHost Resize Worker" : L"CueWindowsHost Resize Single");
    if (!handle || GetWindowThreadProcessId(handle, nullptr) != GetCurrentThreadId() ||
        !wait_for_display(host, stats, handle, *borrowed, 0, 1))
    {
        return 2;
    }
    auto initialProgress = host.frame_progress();
    if (!initialProgress.has_value())
    {
        return 2;
    }
    const auto initialInstances = stats.instances.load();

    // 最大化で Client Area が変わるまで、Window 操作と GPU 記録の双方を進める
    const auto beforeMaximize = stats.executions.load();
    const auto initialSize = borrowed->client_size();
    ShowWindow(handle, SW_MAXIMIZE);
    if (!wait_for_display(host, stats, handle, *borrowed, beforeMaximize, initialInstances + 1) ||
        !IsZoomed(handle) ||
        pack_size(borrowed->client_size()) == pack_size(initialSize))
    {
        return 3;
    }

    // 最小化を Pump した後で先行 Render を排出し、その間は新しい GPU Pass を記録しない
    ShowWindow(handle, SW_MINIMIZE);
    if (!wait_for_idle(host, handle))
    {
        return 4;
    }
    const auto pausedExecutions = stats.executions.load();
    for (int index = 0; index < 12; ++index)
    {
        auto step = host.step();
        if (!step.has_value() || !*step.try_value())
        {
            return 5;
        }
    }
    if (stats.executions.load() != pausedExecutions)
    {
        return 5;
    }

    // 元の Window を維持したまま復帰し、新しい Graph の表示 Pass を使用する
    const auto beforeRestore = stats.executions.load();
    ShowWindow(handle, SW_RESTORE);
    if (!wait_for_display(host, stats, handle, *borrowed, beforeRestore, 1) || IsIconic(handle))
    {
        return 6;
    }
    const auto restored = borrowed->client_size();

    // 同じ Pump 前の複数通知では最新寸法を採用し、旧 Graph の寸法を記録しない
    ShowWindow(handle, SW_SHOWNORMAL);
    if (!SetWindowPos(handle, nullptr, 0, 0, 480, 360, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ||
        !SetWindowPos(handle, nullptr, 0, 0, 540, 400, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ||
        !SetWindowPos(handle, nullptr, 0, 0, 600, 440, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
    {
        return 7;
    }
    const auto beforeResize = stats.executions.load();
    const auto beforeResizeInstances = stats.instances.load();
    if (!wait_for_display(host, stats, handle, *borrowed, beforeResize, beforeResizeInstances + 1) ||
        pack_size(borrowed->client_size()) == pack_size(restored))
    {
        return 8;
    }
    auto progress = host.frame_progress();
    if (!progress.has_value() || progress.try_value()->updateThreadId == std::thread::id{} ||
        progress.try_value()->renderThreadId == std::thread::id{} ||
        (progress.try_value()->updateThreadId != ownerId) != a_usesWorkers ||
        (progress.try_value()->renderThreadId != ownerId) != a_usesWorkers ||
        progress.try_value()->updateThreadId != initialProgress.try_value()->updateThreadId ||
        progress.try_value()->renderThreadId != initialProgress.try_value()->renderThreadId ||
        progress.try_value()->renderedFrames <= initialProgress.try_value()->renderedFrames ||
        !host.shutdown().has_value() || IsWindow(handle))
    {
        return 9;
    }
    return 0;
}

/// @brief CTest の失敗出力に検査名と戻り値を残し、再現時の分岐を特定する
[[nodiscard]] int report_failure(const char *a_test, int a_code)
{
    std::fprintf(stderr, "WindowsHostTests %s failed: %d\n", a_test, a_code);
    return a_code;
}
} // namespace

/// @brief 実 Window 上で WindowsHost の生成と停止を検証する
int main()
{
    if (const int result = test_failed_initialization(); result != 0)
    {
        return report_failure("initialization", 10 + result);
    }
    if (const int result = test_invalid_config(); result != 0)
    {
        return report_failure("config", 15 + result);
    }
    if (const int result = test_window_lifecycle(); result != 0)
    {
        return report_failure("lifecycle", 20 + result);
    }
    if (const int result = test_worker_frame_progress(); result != 0)
    {
        return report_failure("worker", 30 + result);
    }
    if (const int result = test_callback_shutdown_retry(); result != 0)
    {
        return report_failure("shutdown retry", 40 + result);
    }
    for (const bool usesWorkers : {false, true})
    {
        if (const int result = test_resize_presentation(usesWorkers); result != 0)
        {
            return report_failure(usesWorkers ? "resize worker" : "resize single", 50 + result);
        }
    }
    return 0;
}
