#include <Cue/Platform/Windows/WindowsPlatform.h>
#include <Cue/Runtime/Runtime.h>

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
    cue::Runtime runtime({1, false, 0}, *a_services.clock, *a_services.waiter, *a_services.threadFactory);
    if (runtime.step().has_value() || runtime.progress().has_value())
    {
        return 1;
    }
    // Render は同じ Frame の Update 完了後に実行される
    auto initResult = runtime.initialize(
        [&](std::uint64_t a_frame, std::stop_token) {
            updates = a_frame + 1;
            return cue::Result<void>::success();
        },
        [&](std::uint64_t a_frame, std::stop_token) {
            renders = updates == a_frame + 1 ? updates : 0;
            return cue::Result<void>::success();
        });
    if (!initResult.has_value())
    {
        return 2;
    }
    auto stepResult = runtime.step();
    auto progressResult = runtime.progress();
    if (!stepResult.has_value() || !progressResult.has_value() ||
        progressResult.try_value()->renderedFrames != 1 || updates != 1 || renders != 1)
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
    return 0;
}
