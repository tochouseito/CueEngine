#include <Platform/Windows/WindowsPlatform.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <intrin.h>
#include <windows.h>

namespace cue
{
namespace
{
/// @brief 同じ Thread 内の Timer を再利用し、複数 Thread の待機期限を混線させない
class ThreadTimer final
{
  public:
    /// @brief 高精度 Timer と取消 Event を所有し、未対応 OS では通常 Timer へ戻す
    ThreadTimer() noexcept
    {
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!m_timer)
        {
            m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
        m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    /// @brief Thread の終了時に取消 Callback の借用を残さず Handle を解放する
    ~ThreadTimer()
    {
        if (m_timer)
        {
            CloseHandle(m_timer);
        }
        if (m_stop)
        {
            CloseHandle(m_stop);
        }
    }

    ThreadTimer(const ThreadTimer &) = delete;
    ThreadTimer &operator=(const ThreadTimer &) = delete;

    /// @brief 相対期限を 100 ns 単位で切り上げ、通知に短縮されず停止だけを受け付ける
    [[nodiscard]] std::optional<WaitStatus> sleep(std::chrono::nanoseconds a_duration, std::stop_token a_token) noexcept
    {
        if (!m_timer || !m_stop || !ResetEvent(m_stop))
        {
            return std::nullopt;
        }
        LARGE_INTEGER due{};
        const auto count = a_duration.count();
        due.QuadPart = -(count / 100 + (count % 100 != 0));
        if (!SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE))
        {
            return std::nullopt;
        }
        // Callback は ThreadTimer より先に破棄し、別 Thread の取消完了まで Handle を維持する
        std::stop_callback cancelled(a_token, [this]() noexcept { SetEvent(m_stop); });
        const HANDLE handles[] = {m_stop, m_timer};
        const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (a_token.stop_requested() || result == WAIT_OBJECT_0)
        {
            CancelWaitableTimer(m_timer);
            return WaitStatus::Stopped;
        }
        return result == WAIT_OBJECT_0 + 1 ? std::optional{WaitStatus::TimedOut} : std::nullopt;
    }

  private:
    HANDLE m_timer = nullptr;
    HANDLE m_stop = nullptr;
};

/// @brief Worker終了待ちの間も呼出Thread宛てのWindow Messageを処理する
Result<void> wait_for_thread_with_messages(HANDLE a_thread)
{
    // DXGIの同期SendMessageをRender Workerから受けられるよう、join前にMessage Queueを回す
    bool hasQuitMessage = false;
    int quitCode = 0;
    for (;;)
    {
        const DWORD waitStatus = MsgWaitForMultipleObjectsEx(1, &a_thread, INFINITE, QS_ALLINPUT,
                                                              MWMO_INPUTAVAILABLE);
        if (waitStatus == WAIT_OBJECT_0)
        {
            break;
        }
        if (waitStatus != WAIT_OBJECT_0 + 1)
        {
            const DWORD nativeCode = GetLastError();
            if (hasQuitMessage)
            {
                PostQuitMessage(quitCode);
            }
            return Result<void>::failure({ErrorCategory::PlatformFailure, "Thread.join.wait", nativeCode});
        }

        // WM_QUITはWindowSystemが終了状態として扱うため、join完了後にQueueへ戻す
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                hasQuitMessage = true;
                quitCode = static_cast<int>(message.wParam);
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (hasQuitMessage)
    {
        PostQuitMessage(quitCode);
    }
    return Result<void>::success();
}

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
        // 待機開始前の通知や停止要求を先に判定して不要な Sleep を避ける
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

        // Condition と Stop Token の両方で解除し、解除後に理由を判定する
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
        // FPS 制御では通常通知で短縮せず、停止要求だけを割り込ませる
        if (a_stopToken.stop_requested())
        {
            return WaitStatus::Stopped;
        }
        if (a_duration <= std::chrono::nanoseconds::zero())
        {
            return WaitStatus::TimedOut;
        }
        // Timer は Thread ごとに所有し、Main と Worker の同時 Sleep でも Set の期限を分離する
        thread_local ThreadTimer timer;
        if (auto status = timer.sleep(a_duration, a_stopToken))
        {
            return *status;
        }
        // Native Timer が作れない場合も従来の停止可能な待機で継続する
        std::unique_lock lock(m_mutex);
        m_condition.wait_for(lock, a_stopToken, a_duration, []() { return false; });
        return a_stopToken.stop_requested() ? WaitStatus::Stopped : WaitStatus::TimedOut;
    }

    /// @brief 最終スピン中に SMT の実行資源へ配慮し、Scheduler に長い休止を依頼しない
    void relax() noexcept override
    {
#if defined(_M_IX86) || defined(_M_X64)
        _mm_pause();
#elif defined(_M_ARM64)
        __yield();
#else
        std::atomic_signal_fence(std::memory_order_relaxed);
#endif
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
        // Worker の Result と例外を保持し、join した Thread へ渡す
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
        // 明示 join がなくても Routine の捕捉先より先に Worker を止める
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
        // 自分自身を join できず、成功した join だけ完了済みとして記録する
        if (!m_isJoined)
        {
            if (std::this_thread::get_id() == m_thread.get_id())
            {
                return Result<void>::failure({ErrorCategory::WrongThread, "Thread.join.self"});
            }
            try
            {
                auto waitResult = wait_for_thread_with_messages(static_cast<HANDLE>(m_thread.native_handle()));
                if (!waitResult.has_value())
                {
                    return waitResult;
                }
                m_thread.join();
                m_isJoined = true;
            }
            catch (const std::system_error& exception)
            {
                return Result<void>::failure({ErrorCategory::PlatformFailure, "Thread.join", exception.code().value()});
            }
        }

        // join 後に Worker が保存した失敗を優先順位付きで返す
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
        // Routine がなければ Thread を起動せず、呼出側へ入力失敗を返す
        if (!a_routine)
        {
            return Result<std::unique_ptr<Thread>>::failure({ErrorCategory::InvalidArgument, "ThreadFactory.start"});
        }
        try
        {
            // Thread の一意所有権を抽象 Interface として返す
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
        // Service の寿命は返した構造体の Owner が管理する
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
