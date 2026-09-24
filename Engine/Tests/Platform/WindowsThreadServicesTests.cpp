#include <Cue/Platform/Windows/WindowsPlatform.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <stop_token>

namespace
{
/// @brief 通知の先行、停止、Routine失敗がHostへ伝わることを確認する
int run_tests()
{
    auto servicesResult = cue::create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return 1;
    }
    auto services = servicesResult.take_value();

    const auto firstTime = services.clock->now();
    const auto secondTime = services.clock->now();
    if (firstTime > secondTime)
    {
        return 2;
    }

    const auto generation = services.waiter->generation();
    services.waiter->notify_all();
    if (services.waiter->wait_for_change(generation, std::chrono::seconds(1), {}) != cue::WaitStatus::Notified)
    {
        return 3;
    }

    std::stop_source stopSource;
    stopSource.request_stop();
    if (services.waiter->sleep_for(std::chrono::seconds(1), stopSource.get_token()) != cue::WaitStatus::Stopped)
    {
        return 4;
    }

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
