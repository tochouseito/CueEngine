#include <Platform/Windows/WindowsPlatform.h>
#include <Runtime/Runtime.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stop_token>

namespace
{
/// @brief Windowを生成せず、注入したServiceだけでFrameを進める
int test_windowless_runtime(cue::WindowsThreadServices& a_services)
{
    // Window を渡さず Service だけで Runtime を構築する
    std::uint64_t updates = 0;
    std::uint64_t renders = 0;
    std::uint64_t mainFrames = 0;
    cue::Runtime runtime({1, false, 0}, *a_services.clock, *a_services.waiter, *a_services.threadFactory);
    auto beforeInitialize = runtime.is_idle();
    if (runtime.step().has_value() || runtime.progress().has_value() || beforeInitialize.has_value() ||
        beforeInitialize.try_error()->category != cue::ErrorCategory::InvalidState)
    {
        return 1;
    }
    // Render は同じ Frame の Update 完了後に実行される
    auto initResult = runtime.initialize(
        [&](std::uint64_t a_frame, std::stop_token)
        {
            if (mainFrames != a_frame + 1)
            {
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.runtime.main"});
            }
            updates = a_frame + 1;
            return cue::Result<void>::success();
        },
        [&](std::uint64_t a_frame, std::stop_token) {
            renders = updates == a_frame + 1 ? updates : 0;
            return cue::Result<void>::success();
        },
        [&](std::uint64_t a_frame, std::stop_token)
        {
            // Main の実行中は Controller を破棄せず、Snapshot の参照だけ許可する
            auto nested = runtime.step();
            auto stopped = runtime.shutdown();
            auto progress = runtime.progress();
            if (nested.has_value() || stopped.has_value() || !progress.has_value() ||
                nested.try_error()->category != cue::ErrorCategory::InvalidState ||
                stopped.try_error()->category != cue::ErrorCategory::InvalidState ||
                progress.try_value()->submittedFrames != a_frame)
            {
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.runtime.reentry"});
            }
            mainFrames = a_frame + 1;
            return cue::Result<void>::success();
        });
    if (!initResult.has_value())
    {
        return 2;
    }
    auto initiallyIdle = runtime.is_idle();
    if (!initiallyIdle.has_value() || !*initiallyIdle.try_value())
    {
        return 5;
    }
    auto stepResult = runtime.step();
    auto progressResult = runtime.progress();
    auto completed = runtime.is_idle();
    if (!stepResult.has_value() || !progressResult.has_value() || progressResult.try_value()->renderedFrames != 1 ||
        updates != 1 || renders != 1 || !completed.has_value() || !*completed.try_value())
    {
        return 3;
    }
    // Shutdown は複数回安全で、停止後の操作と再初期化は拒否する
    if (!runtime.shutdown().has_value() || !runtime.shutdown().has_value() ||
        runtime.step().has_value() || runtime.initialize({}, {}).has_value())
    {
        return 4;
    }
    return 0;
}

/// @brief Worker の未完了状態と完了後の静止状態を Runtime 経由で確認する
int test_worker_runtime_idle(cue::WindowsThreadServices &a_services)
{
    std::atomic<bool> releaseUpdate = false;
    cue::Runtime runtime({1, true, 0}, *a_services.clock, *a_services.waiter, *a_services.threadFactory);
    auto initialized = runtime.initialize(
        [&](std::uint64_t, std::stop_token a_token)
        {
            while (!releaseUpdate.load() && !a_token.stop_requested())
            {
                [[maybe_unused]] const auto status =
                    a_services.waiter->sleep_for(std::chrono::milliseconds(1), a_token);
            }
            return cue::Result<void>::success();
        },
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); });
    if (!initialized.has_value() || !runtime.step().has_value())
    {
        return 1;
    }
    auto pending = runtime.is_idle();
    if (!pending.has_value() || *pending.try_value())
    {
        return 2;
    }
    releaseUpdate = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto idle = runtime.is_idle();
        if (!idle.has_value())
        {
            return 3;
        }
        if (*idle.try_value())
        {
            return runtime.shutdown().has_value() ? 0 : 4;
        }
        [[maybe_unused]] const auto status = a_services.waiter->sleep_for(std::chrono::milliseconds(1), {});
    }
    return 5;
}

/// @brief Update 失敗時は静止待ちから最初の Error を取得できる
int test_worker_runtime_idle_failure(cue::WindowsThreadServices &a_services)
{
    cue::Runtime runtime({1, true, 0}, *a_services.clock, *a_services.waiter, *a_services.threadFactory);
    auto initialized = runtime.initialize(
        [](std::uint64_t, std::stop_token)
        { return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.runtime.update.failure", 41}); },
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); });
    if (!initialized.has_value() || !runtime.step().has_value())
    {
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto idle = runtime.is_idle();
        if (!idle.has_value())
        {
            auto stopped = runtime.shutdown();
            return idle.try_error()->nativeCode == 41 && !stopped.has_value() && stopped.try_error()->nativeCode == 41
                       ? 0
                       : 2;
        }
        [[maybe_unused]] const auto status = a_services.waiter->sleep_for(std::chrono::milliseconds(1), {});
    }
    return 3;
}

/// @brief 初期化失敗時に借用先を使い続けず、別Runtimeで再試行できる
int test_failed_runtime(cue::WindowsThreadServices& a_services)
{
    // Callback 不足による初期化失敗後も明示停止できる
    cue::Runtime invalidCallbacks({1, false, 0}, *a_services.clock, *a_services.waiter,
                                  *a_services.threadFactory);
    auto callbackResult = invalidCallbacks.initialize({}, {});
    if (callbackResult.has_value() ||
        callbackResult.try_error()->category != cue::ErrorCategory::InvalidArgument ||
        !invalidCallbacks.shutdown().has_value())
    {
        return 1;
    }

    // Controller の設定検証で失敗しても停止できる
    cue::Runtime invalidFrame({0, false, 0}, *a_services.clock, *a_services.waiter,
                              *a_services.threadFactory);
    auto success = [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); };
    auto frameResult = invalidFrame.initialize(success, success);
    if (frameResult.has_value() || frameResult.try_error()->category != cue::ErrorCategory::InvalidArgument ||
        !invalidFrame.shutdown().has_value())
    {
        return 2;
    }
    return 0;
}
} // namespace

/// @brief 共通RuntimeがWindowなしで起動・停止できることを確認する
int main()
{
    auto servicesResult = cue::create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return 1;
    }
    auto services = servicesResult.take_value();
    if (const int result = test_windowless_runtime(services); result != 0)
    {
        return 10 + result;
    }
    if (const int result = test_failed_runtime(services); result != 0)
    {
        return 20 + result;
    }
    if (const int result = test_worker_runtime_idle(services); result != 0)
    {
        return 30 + result;
    }
    if (const int result = test_worker_runtime_idle_failure(services); result != 0)
    {
        return 40 + result;
    }
    return 0;
}
