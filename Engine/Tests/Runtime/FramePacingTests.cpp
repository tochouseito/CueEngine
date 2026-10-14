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
    const auto plan = test_plan();
    if (plan)
    {
        return plan;
    }
    const auto notification = test_single_notification();
    return notification ? notification : test_delayed_admission();
}
