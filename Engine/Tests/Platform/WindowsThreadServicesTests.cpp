#include <Cue/Platform/Windows/WindowsPlatform.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <stop_token>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
/// @brief 通知の先行、停止、Routine失敗がHostへ伝わることを確認する
int run_tests()
{
    // 実装を一括生成し、各 Service の契約を同じ所有期間で検証する
    auto servicesResult = cue::create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return 1;
    }
    auto services = servicesResult.take_value();

    // Clock は後戻りしない時刻を返す
    const auto firstTime = services.clock->now();
    const auto secondTime = services.clock->now();
    if (firstTime > secondTime)
    {
        return 2;
    }

    // 待機開始前に行った通知も世代差から検知できる
    const auto generation = services.waiter->generation();
    services.waiter->notify_all();
    if (services.waiter->wait_for_change(generation, std::chrono::seconds(1), {}) != cue::WaitStatus::Notified)
    {
        return 3;
    }

    // 停止済み Token は長い Sleep に入る前に反映される
    std::stop_source stopSource;
    stopSource.request_stop();
    if (services.waiter->sleep_for(std::chrono::seconds(1), stopSource.get_token()) != cue::WaitStatus::Stopped)
    {
        return 4;
    }

    // Worker 待機中の停止と、二度目の join の結果を確認する
    std::atomic<bool> wasStopped = false;
    auto threadResult = services.threadFactory->start([&](std::stop_token a_stopToken) {
        const auto observedGeneration = services.waiter->generation();
        wasStopped = services.waiter->wait_for_change(observedGeneration, std::chrono::seconds(10), a_stopToken) ==
                     cue::WaitStatus::Stopped;
        return cue::Result<void>::success();
    });
    if (!threadResult.has_value())
    {
        return 5;
    }
    auto thread = threadResult.take_value();
    thread->request_stop();
    if (!thread->join().has_value() || !wasStopped || !thread->join().has_value())
    {
        return 6;
    }

    // Routine が返した Error は join の診断値として保持する
    auto failureThreadResult = services.threadFactory->start([](std::stop_token) {
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "TestRoutine.failure", 42});
    });
    if (!failureThreadResult.has_value())
    {
        return 7;
    }
    auto failureThread = failureThreadResult.take_value();
    auto failureResult = failureThread->join();
    if (failureResult.has_value() || failureResult.try_error()->nativeCode != 42)
    {
        return 8;
    }
    if (failureThread->join().has_value())
    {
        return 9;
    }

    // 例外と空 Routine も Worker 境界から成功として漏らさない
    auto exceptionThreadResult = services.threadFactory->start([](std::stop_token) -> cue::Result<void> {
        throw std::runtime_error("worker failure");
    });
    if (!exceptionThreadResult.has_value())
    {
        return 10;
    }
    auto exceptionThread = exceptionThreadResult.take_value();
    if (exceptionThread->join().has_value())
    {
        return 11;
    }

    if (services.threadFactory->start({}).has_value())
    {
        return 12;
    }

    // Window所有Threadのjoin待機中もWorkerからの同期Messageを処理する
    auto windowSystemResult = cue::create_windows_window_system();
    if (!windowSystemResult.has_value())
    {
        return 14;
    }
    auto windowSystem = windowSystemResult.take_value();
    auto windowResult = windowSystem->create_window({"Thread Join Message Test", {320, 240}});
    if (!windowResult.has_value())
    {
        return 15;
    }
    auto window = windowResult.take_value();
    auto handleResult = cue::borrow_windows_window_handle(*window);
    if (!handleResult.has_value())
    {
        return 16;
    }
    const auto handle = static_cast<HWND>(handleResult.take_value());
    auto messageThreadResult = services.threadFactory->start([handle](std::stop_token) {
        SendMessageW(handle, WM_NULL, 0, 0);
        return cue::Result<void>::success();
    });
    if (!messageThreadResult.has_value())
    {
        return 17;
    }
    auto messageThread = messageThreadResult.take_value();
    if (!messageThread->join().has_value())
    {
        return 17;
    }
    if (!window->destroy().has_value())
    {
        return 18;
    }
    window.reset();
    windowSystem.reset();

    // 通知がない通常の Sleep は時間切れとして返る
    if (services.waiter->sleep_for(std::chrono::milliseconds(16), {}) != cue::WaitStatus::TimedOut)
    {
        return 13;
    }
    return 0;
}
} // namespace

/// @brief Windowsの時間・Thread実装を検証する
int main()
{
    return run_tests();
}
