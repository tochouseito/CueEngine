#include <Runtime/FrameController.h>

#include <chrono>
#include <optional>

#include <Platform/Windows/WindowsPlatform.h>

namespace
{
/// @brief Single Thread の通知と予測期限を実時間に依存せず検証する
class ManualClock final : public cue::Clock
{
  public:
    /// @brief Test が明示した時間だけを Controller へ返す
    std::chrono::steady_clock::time_point now() const noexcept override
    {
        return time;
    }
    std::chrono::steady_clock::time_point time{};
};

/// @brief Main の待機要求を記録し、実時間に依存せず期限の計算を検証する
class RecordingWaiter final : public cue::Waiter
{
  public:
    /// @brief Controller より長く生存する Test 時計を借用する
    explicit RecordingWaiter(ManualClock &a_clock) : m_clock(a_clock)
    {
    }
    /// @brief 単一 Thread の通知世代を返す
    std::uint64_t generation() const noexcept override
    {
        return m_generation;
    }
    /// @brief 停止通知を受け付ける
    void notify_all() noexcept override
    {
        ++m_generation;
    }
    /// @brief FPS 待機が従来の通知 Timeout へ戻った場合を検出する
    cue::WaitStatus wait_for_change(std::uint64_t, std::chrono::nanoseconds a_duration,
                                    std::stop_token a_token) noexcept override
    {
        ++notificationWaits;
        return sleep_for(a_duration, a_token);
    }
    /// @brief 指定時間だけ時計を進め、Main を塞ぐ長い要求も記録する
    cue::WaitStatus sleep_for(std::chrono::nanoseconds a_duration, std::stop_token a_token) noexcept override
    {
        if (a_token.stop_requested())
        {
            return cue::WaitStatus::Stopped;
        }
        ++sleeps;
        longestSleep = (std::max)(longestSleep, a_duration);
        m_clock.time += a_duration;
        return cue::WaitStatus::TimedOut;
    }
    /// @brief Spin の一回を短い時間として進める
    void relax() noexcept override
    {
        ++spins;
        m_clock.time += std::chrono::microseconds(10);
    }
    unsigned sleeps = 0;
    unsigned spins = 0;
    unsigned notificationWaits = 0;
    std::chrono::nanoseconds longestSleep{};

  private:
    ManualClock &m_clock;
    std::uint64_t m_generation = 0;
};

/// @brief Single でも Sleep / Hybrid の設定を使用し、期限より前に次 Frame を許可しない
int test_single_wait_policy()
{
    using namespace std::chrono_literals;
    auto servicesResult = cue::create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return 1;
    }
    auto services = servicesResult.take_value();
    for (const auto mode : {cue::FrameWaitMode::Sleep, cue::FrameWaitMode::Hybrid})
    {
        ManualClock clock;
        RecordingWaiter waiter(clock);
        cue::FrameControllerDesc config{1, false, 100};
        config.waitMode = mode;
        config.spinDuration = 500us;
        auto success = [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); };
        cue::FrameController controller(config, clock, waiter, *services.threadFactory, success, success);
        if (!controller.start().has_value() || !controller.advance().has_value())
        {
            return 2;
        }
        bool isReady = false;
        for (int attempt = 0; attempt < 30 && !isReady; ++attempt)
        {
            auto ready = controller.wait_for_frame();
            if (!ready.has_value() || (*ready.try_value() && clock.time.time_since_epoch() < 10ms))
            {
                return 3;
            }
            isReady = *ready.try_value();
        }
        if (!isReady || clock.time.time_since_epoch() != 10ms || waiter.sleeps == 0 || waiter.longestSleep > 1ms ||
            waiter.notificationWaits != 0 || ((waiter.spins != 0) != (mode == cue::FrameWaitMode::Hybrid)) ||
            !controller.stop().has_value())
        {
            return 4;
        }
    }
    cue::FrameControllerDesc config{1, false, 1000};
    config.spinDuration = 5ms;
    if (cue::effective_spin_duration(config) != 125us)
    {
        return 5;
    }
    config.maxFps = 0;
    return cue::effective_spin_duration(config) == 0ns ? 0 : 6;
}

/// @brief 予測時刻なし / 古い時刻 / 過大な時刻では固定周期へ戻る
int test_plan()
{
    using namespace std::chrono_literals;
    const auto now = std::chrono::steady_clock::time_point(10ms);
    const auto fixed = now + 5ms;
    for (const auto prediction :
         {std::optional<std::chrono::steady_clock::time_point>{}, std::optional{now}, std::optional{now + 1s}})
    {
        const auto plan =
            cue::plan_frame_cycle(cue::FrameSyncMode::DisplayPrediction, fixed, now, prediction, 1ms, true);
        if (plan.effectiveMode != cue::FrameSyncMode::FixedPeriod || plan.deadline != fixed ||
            plan.fallback != cue::FrameSyncFallback::DisplayTimingUnavailable)
        {
            return 1;
        }
    }
    const auto plan = cue::plan_frame_cycle(cue::FrameSyncMode::DisplayPrediction, fixed, now, now + 20ms, 2ms, true);
    if (plan.deadline != now + 18ms || plan.effectiveMode != cue::FrameSyncMode::DisplayPrediction ||
        !plan.waitsForPresentation)
    {
        return 2;
    }
    const auto missing = cue::plan_frame_cycle(cue::FrameSyncMode::PresentationReady, fixed, now, {}, 1ms, false);
    return missing.fallback == cue::FrameSyncFallback::PresentationUnavailable ? 0 : 3;
}

/// @brief Main では表示通知を非待機で確認し、DisplayPrediction の期限と入力年齢を分けて測る
int test_single_notification()
{
    using namespace std::chrono_literals;
    auto servicesResult = cue::create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return 1;
    }
    auto services = servicesResult.take_value();
    ManualClock clock;
    bool isPresentationReady = false;
    cue::FrameControllerDesc config{1, false, 0};
    config.syncMode = cue::FrameSyncMode::DisplayPrediction;
    config.predictionSlack = 1ms;
    cue::FrameController controller(
        config, clock, *services.waiter, *services.threadFactory,
        [&](std::uint64_t, std::stop_token)
        {
            clock.time += 2ms;
            return cue::Result<void>::success();
        },
        [&](std::uint64_t, std::stop_token)
        {
            clock.time += 3ms;
            return cue::Result<void>::success();
        });
    if (!controller
             .register_pacing_source({[&](std::stop_token, bool a_nonBlocking)
                                      {
                                          return a_nonBlocking
                                                     ? cue::Result<bool>::success(isPresentationReady)
                                                     : cue::Result<bool>::failure(
                                                           {cue::ErrorCategory::InvalidState, "Test.blocking_main"});
                                      },
                                      [&]() { return std::optional{std::chrono::steady_clock::time_point(20ms)}; }})
             .has_value() ||
        !controller.start().has_value() || !controller.advance().has_value())
    {
        return 2;
    }
    clock.time = std::chrono::steady_clock::time_point(18ms);
    auto early = controller.wait_for_frame();
    clock.time = std::chrono::steady_clock::time_point(19ms);
    auto unready = controller.wait_for_frame();
    clock.time = std::chrono::steady_clock::time_point(29ms);
    isPresentationReady = true;
    auto ready = controller.wait_for_frame();
    const auto timing = controller.timing_info();
    const auto progress = controller.progress();
    if (!early.has_value() || *early.try_value() || !unready.has_value() || *unready.try_value() ||
        !ready.has_value() || !*ready.try_value() || progress.completedCycles != 1 ||
        progress.effectiveSyncMode != cue::FrameSyncMode::DisplayPrediction ||
        timing.inputToRenderStart.averageDuration != 2ms || timing.inputToRenderComplete.averageDuration != 5ms ||
        timing.limitWait.lastDuration != 14ms || timing.presentationWait.lastDuration != 10ms ||
        !controller.stop().has_value())
    {
        return 3;
    }
    return 0;
}
/// @brief Message Pump / 表示枠の待ちで採用が遅れても、古い FPS 期限で次 Frame を連続投入しない
int test_delayed_admission()
{
    using namespace std::chrono_literals;
    auto servicesResult = cue::create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return 1;
    }
    auto services = servicesResult.take_value();
    ManualClock clock;
    auto callback = [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); };
    cue::FrameController controller({1, false, 60}, clock, *services.waiter, *services.threadFactory, callback,
                                    callback);
    if (!controller.start().has_value() || !controller.advance().has_value())
    {
        return 2;
    }
    clock.time = std::chrono::steady_clock::time_point(20ms);
    auto available = controller.wait_for_frame();
    clock.time = std::chrono::steady_clock::time_point(80ms);
    auto delayed = controller.advance();
    auto next = controller.wait_for_frame();
    return available.has_value() && *available.try_value() && delayed.has_value() && *delayed.try_value() &&
                   next.has_value() && !*next.try_value() && controller.stop().has_value()
               ? 0
               : 3;
}
} // namespace

/// @brief 予測の Fallback と非待機通知の同期契約を確認する
int main()
{
    if (const auto policy = test_single_wait_policy(); policy != 0)
    {
        return 10 + policy;
    }
    const auto plan = test_plan();
    if (plan)
    {
        return plan;
    }
    const auto notification = test_single_notification();
    return notification ? notification : test_delayed_admission();
}
