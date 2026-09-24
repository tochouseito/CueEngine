#include <Cue/Platform/Windows/WindowsPlatform.h>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace cue
{
namespace
{
class WindowsClock final : public Clock
{
public:
    /// @brief 単調時計の現在時刻を返す
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override
    {
        return std::chrono::steady_clock::now();
    }
};

class WindowsWaiter final : public Waiter
{
public:
    /// @brief 現在の通知世代を返す
    [[nodiscard]] std::uint64_t generation() const noexcept override
    {
        std::lock_guard lock(m_mutex);
        return m_generation;
    }

    /// @brief 世代更新と通知を同じLock境界で同期する
    void notify_all() noexcept override
    {
        {
            std::lock_guard lock(m_mutex);
            ++m_generation;
        }
        m_condition.notify_all();
    }

    /// @brief 通知、時間切れ、停止要求まで待つ
    [[nodiscard]] WaitStatus wait_for_change(std::uint64_t a_observedGeneration,
                                             std::chrono::nanoseconds a_duration,
                                             std::stop_token a_stopToken) noexcept override
    {
        std::unique_lock lock(m_mutex);
        if (a_stopToken.stop_requested())
        {
            return WaitStatus::Stopped;
        }
        if (m_generation != a_observedGeneration)
        {
            return WaitStatus::Notified;
        }
        if (a_duration <= std::chrono::nanoseconds::zero())
        {
            return WaitStatus::TimedOut;
        }

        m_condition.wait_for(lock, a_stopToken, a_duration,
                             [this, a_observedGeneration]() { return m_generation != a_observedGeneration; });
        if (a_stopToken.stop_requested())
        {
            return WaitStatus::Stopped;
        }
        return m_generation != a_observedGeneration ? WaitStatus::Notified : WaitStatus::TimedOut;
    }

    /// @brief 通知で待機時間を短縮せず停止要求だけを受け付ける
    [[nodiscard]] WaitStatus sleep_for(std::chrono::nanoseconds a_duration,
                                       std::stop_token a_stopToken) noexcept override
    {
        std::unique_lock lock(m_mutex);
        if (a_stopToken.stop_requested())
        {
            return WaitStatus::Stopped;
        }
        if (a_duration <= std::chrono::nanoseconds::zero())
        {
            return WaitStatus::TimedOut;
        }
        m_condition.wait_for(lock, a_stopToken, a_duration, []() { return false; });
        return a_stopToken.stop_requested() ? WaitStatus::Stopped : WaitStatus::TimedOut;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable_any m_condition;
    std::uint64_t m_generation = 0;
};

class WindowsThread final : public Thread
{
public:
    /// @brief Routineを別Threadで開始する
    explicit WindowsThread(ThreadRoutine a_routine)
    {
        m_thread = std::jthread([this, routine = std::move(a_routine)](std::stop_token a_stopToken) mutable {
            try
            {
                auto result = routine(a_stopToken);
                if (!result.has_value())
                {
                    std::lock_guard lock(m_resultMutex);
                    m_error = *result.try_error();
                }
            }
            catch (...)
            {
                std::lock_guard lock(m_resultMutex);
                m_exception = std::current_exception();
            }
        });
    }

    /// @brief 停止要求後にWorkerの終了を待って破棄する
    ~WindowsThread() override
    {
        request_stop();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    /// @brief Routineへ協調停止を通知する
    void request_stop() noexcept override
    {
        m_thread.request_stop();
    }

    /// @brief Workerの完了とRoutine結果を取得する
    [[nodiscard]] Result<void> join() override
    {
        if (!m_isJoined)
        {
            if (std::this_thread::get_id() == m_thread.get_id())
            {
                return Result<void>::failure({ErrorCategory::WrongThread, "Thread.join.self"});
            }
            try
            {
                m_thread.join();
                m_isJoined = true;
            }
            catch (const std::system_error& exception)
            {
                return Result<void>::failure({ErrorCategory::PlatformFailure, "Thread.join", exception.code().value()});
            }
        }

        std::lock_guard lock(m_resultMutex);
        if (m_exception)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "Thread.routine.exception"});
        }
        if (m_error)
        {
            return Result<void>::failure(*m_error);
        }
        return Result<void>::success();
    }

    /// @brief 診断用のWorker識別子を返す
    [[nodiscard]] std::thread::id id() const noexcept override
    {
        return m_thread.get_id();
    }

private:
    std::mutex m_resultMutex;
    std::optional<Error> m_error;
    std::exception_ptr m_exception;
    std::jthread m_thread;
    bool m_isJoined = false;
};

class WindowsThreadFactory final : public ThreadFactory
{
public:
    /// @brief Routineを別Threadで開始して所有Handleを返す
    [[nodiscard]] Result<std::unique_ptr<Thread>> start(ThreadRoutine a_routine) override
    {
        if (!a_routine)
        {
            return Result<std::unique_ptr<Thread>>::failure({ErrorCategory::InvalidArgument, "ThreadFactory.start"});
        }
        try
        {
            std::unique_ptr<Thread> thread = std::make_unique<WindowsThread>(std::move(a_routine));
            return Result<std::unique_ptr<Thread>>::success(std::move(thread));
        }
        catch (const std::system_error& exception)
        {
            return Result<std::unique_ptr<Thread>>::failure(
                {ErrorCategory::PlatformFailure, "ThreadFactory.start", exception.code().value()});
        }
        catch (const std::exception&)
        {
            return Result<std::unique_ptr<Thread>>::failure({ErrorCategory::PlatformFailure, "ThreadFactory.start"});
        }
    }
};
} // namespace

/// @brief WindowsのFrame制御用Serviceを一括生成する
Result<WindowsThreadServices> create_windows_thread_services()
{
    try
    {
        WindowsThreadServices services{};
        services.clock = std::make_unique<WindowsClock>();
        services.waiter = std::make_unique<WindowsWaiter>();
        services.threadFactory = std::make_unique<WindowsThreadFactory>();
        return Result<WindowsThreadServices>::success(std::move(services));
    }
    catch (const std::exception&)
    {
        return Result<WindowsThreadServices>::failure({ErrorCategory::PlatformFailure, "WindowsThreadServices.create"});
    }
}
} // namespace cue
