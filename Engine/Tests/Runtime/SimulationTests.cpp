#include <Runtime/FrameController.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stop_token>
#include <thread>

#include <Platform/Windows/WindowsPlatform.h>

namespace
{
/// @brief 60 Hz の World 前後状態から 120 FPS の不変描画値を抽出する
int test_fixed_interpolation()
{
    cue::SimulationStepper simulation({60, 4});
    double previous = 0.0, current = 0.0;
    unsigned updates = 0, zeroStepFrames = 0;
    cue::simulationCallback update = [&](const cue::SimulationStep &a_step, std::stop_token)
    {
        previous = current;
        current += a_step.deltaTime.count();
        ++updates;
        return cue::Result<void>::success();
    };
    for (std::uint64_t frame = 0; frame <= 120; ++frame)
    {
        const auto elapsed = std::chrono::nanoseconds(frame * 1'000'000'000ULL / 120);
        auto advanced = simulation.advance(frame, std::chrono::steady_clock::time_point(elapsed), update);
        if (!advanced.has_value())
        {
            return 1;
        }
        const auto &info = *advanced.try_value();
        const auto expectedTicks = frame * 1'000'000'000ULL / 120 * 60 / 1'000'000'000ULL;
        const double rendered = previous + (current - previous) * info.interpolationAlpha;
        const double expected = (std::max)(0.0, std::chrono::duration<double>(elapsed).count() - 1.0 / 60);
        if (info.frameIndex != frame || info.currentTick != expectedTicks || info.interpolationAlpha < 0 ||
            info.interpolationAlpha >= 1 || info.updateSteps > 1 || std::abs(rendered - expected) > 1e-8)
        {
            return 2;
        }
        zeroStepFrames += info.updateSteps == 0;
    }
    return updates == 60 && zeroStepFrames == 61 ? 0 : 3;
}

/// @brief 過負荷を上限へ丸め、余った完全 Tick を次 Frame へ持ち越さない
int test_overload_and_switch()
{
    using namespace std::chrono_literals;
    cue::SimulationStepper simulation({60, 4, 250ms});
    auto update = [](const cue::SimulationStep &, std::stop_token) { return cue::Result<void>::success(); };
    auto initial = simulation.advance(0, {}, update);
    auto overloaded = simulation.advance(1, std::chrono::steady_clock::time_point(1s), update);
    if (!initial.has_value() || !overloaded.has_value() || overloaded.try_value()->updateSteps != 4 ||
        overloaded.try_value()->currentTick != 4 || overloaded.try_value()->droppedElapsed != 750ms + 183333333ns ||
        overloaded.try_value()->interpolationAlpha != 0)
    {
        return 1;
    }
    auto noBurst = simulation.advance(2, std::chrono::steady_clock::time_point(1s), update);
    if (!noBurst.has_value() || noBurst.try_value()->updateSteps != 0)
    {
        return 2;
    }
    if (!simulation.configure({0, 4, 250ms}).has_value())
    {
        return 3;
    }
    auto variable = simulation.advance(3, std::chrono::steady_clock::time_point(2s), update);
    if (!variable.has_value() || variable.try_value()->currentTick != 5 || variable.try_value()->isFixed ||
        variable.try_value()->interpolationAlpha != 1.0 || variable.try_value()->frameElapsed != 0ns)
    {
        return 4;
    }
    std::stop_source stopped;
    stopped.request_stop();
    auto canceled =
        simulation.advance(4, std::chrono::steady_clock::time_point(2s + 10ms), update, stopped.get_token());
    if (!canceled.has_value() || !canceled.try_value()->wasCanceled || canceled.try_value()->currentTick != 5 ||
        simulation.advance(5, std::chrono::steady_clock::time_point(1s), update).has_value())
    {
        return 5;
    }
    cue::SimulationStepper failed({60, 4});
    if (!failed.advance(0, {}, update).has_value())
    {
        return 6;
    }
    auto rejected =
        failed.advance(1, std::chrono::steady_clock::time_point(20ms), [](const cue::SimulationStep &, std::stop_token)
                       { return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.fixed_update"}); });
    return !rejected.has_value() && rejected.try_error()->operation == "Test.fixed_update" &&
                   !failed.advance(2, std::chrono::steady_clock::time_point(30ms), update).has_value() &&
                   !failed.configure({0, 4}).has_value()
               ? 0
               : 7;
}

/// @brief 120 FPS の Frame 採取時刻を明示して Snapshot を Render へ渡す
class ManualClock final : public cue::Clock
{
  public:
    /// @brief 単一 Thread の確定時刻を返す
    std::chrono::steady_clock::time_point now() const noexcept override
    {
        return time;
    }
    std::chrono::steady_clock::time_point time{};
};

/// @brief 並列 Frame でも Main が採用した時刻を同じ時間軸で渡す
class AtomicClock final : public cue::Clock
{
  public:
    /// @brief Main と Worker が競合せず同じ Test 時刻を読む
    std::chrono::steady_clock::time_point now() const noexcept override
    {
        return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ticks.load()));
    }
    std::atomic<std::int64_t> ticks = 0;
};

/// @brief Update F1 が World を進めても、Render F0 の Tick / 補間情報が変化しない
int test_parallel_snapshot_versions()
{
    auto servicesResult = cue::create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return 1;
    }
    auto services = servicesResult.take_value();
    AtomicClock clock;
    std::atomic<bool> renderStarted = false, nextSnapshotReady = false;
    std::atomic<std::uint64_t> currentTick = 0;
    cue::FrameController *borrowed = nullptr;
    cue::FrameController controller(
        {2, true, 0}, clock, *services.waiter, *services.threadFactory,
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); },
        [&](std::uint64_t a_frame, std::stop_token a_token)
        {
            if (a_frame == 0)
            {
                renderStarted.store(true);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!nextSnapshotReady.load() && !a_token.stop_requested() &&
                       std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            auto info = borrowed->simulation_info(a_frame);
            if (!nextSnapshotReady.load() || !info.has_value() || info.try_value()->currentTick != a_frame ||
                currentTick.load() != 1)
            {
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.parallel_snapshot"});
            }
            return cue::Result<void>::success();
        },
        {60, 4});
    borrowed = &controller;
    if (!controller
             .register_simulation_callbacks(
                 [&](const cue::SimulationStep &a_step, std::stop_token)
                 {
                     currentTick.store(a_step.tick);
                     return cue::Result<void>::success();
                 },
                 [&](const cue::FrameSimulationInfo &a_info, std::stop_token)
                 {
                     if (a_info.frameIndex == 1)
                     {
                         nextSnapshotReady.store(true);
                     }
                     return cue::Result<void>::success();
                 })
             .has_value() ||
        !controller.start().has_value() || !controller.advance().has_value())
    {
        return 2;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!renderStarted.load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clock.ticks.store(20'000'000);
    auto next = controller.advance();
    if (!renderStarted.load() || !next.has_value() || !*next.try_value())
    {
        return 3;
    }
    while (controller.progress().renderedFrames < 2 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return controller.progress().renderedFrames == 2 && controller.stop().has_value() ? 0 : 4;
}

/// @brief Controller 経由で Tick と Frame を分離し、固定 / 可変切替でも ID を維持する
int test_controller_snapshots()
{
    auto servicesResult = cue::create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return 1;
    }
    auto services = servicesResult.take_value();
    ManualClock clock;
    std::array<cue::FrameSimulationInfo, 8> snapshots;
    std::uint64_t tick = 0;
    cue::FrameController *borrowed = nullptr;
    cue::FrameController controller(
        {1, false, 0}, clock, *services.waiter, *services.threadFactory,
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); },
        [&](std::uint64_t a_frame, std::stop_token)
        {
            auto info = borrowed->simulation_info(a_frame);
            return info.has_value() &&
                           info.try_value()->currentTick == snapshots[a_frame % snapshots.size()].currentTick
                       ? cue::Result<void>::success()
                       : cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.snapshot"});
        },
        {60, 4});
    borrowed = &controller;
    if (!controller
             .register_simulation_callbacks(
                 [&](const cue::SimulationStep &a_step, std::stop_token)
                 {
                     tick = a_step.tick;
                     return cue::Result<void>::success();
                 },
                 [&](const cue::FrameSimulationInfo &a_info, std::stop_token)
                 {
                     snapshots[a_info.frameIndex % snapshots.size()] = a_info;
                     return cue::Result<void>::success();
                 })
             .has_value() ||
        !controller.start().has_value())
    {
        return 2;
    }
    for (std::uint64_t frame = 0; frame < 7; ++frame)
    {
        clock.time = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(frame * 1'000'000'000ULL / 120));
        auto advanced = controller.advance();
        if (!advanced.has_value() || !*advanced.try_value())
        {
            return 3;
        }
    }
    clock.time = std::chrono::steady_clock::time_point(std::chrono::seconds(2));
    controller.reset_simulation_sample_time();
    auto restored = controller.advance();
    auto restoredInfo = controller.simulation_info(7);
    if (!restored.has_value() || !*restored.try_value() || !restoredInfo.has_value() ||
        restoredInfo.try_value()->frameElapsed != std::chrono::nanoseconds::zero() || tick != 3 ||
        controller.progress().renderedFrames != 8 || !controller.pause().has_value() ||
        !controller.resume({1, false, 0}, cue::SimulationConfig{0, 4}).has_value())
    {
        return 4;
    }
    auto resumed = controller.advance();
    auto info = controller.simulation_info(8);
    const auto resumedTimings = controller.timing_info();
    return resumed.has_value() && *resumed.try_value() && info.has_value() && !info.try_value()->isFixed &&
                   info.try_value()->currentTick == 4 && controller.progress().submittedFrames == 9 &&
                   !controller.simulation_info(0).has_value() && resumedTimings.frameInterval.sampleCount == 0 &&
                   controller.stop().has_value()
               ? 0
               : 5;
}
} // namespace

/// @brief 周期の分離、補間、過負荷、停止と構成切替を検証する
int main()
{
    if (const auto result = test_fixed_interpolation(); result != 0)
    {
        return 10 + result;
    }
    if (const auto result = test_overload_and_switch(); result != 0)
    {
        return 20 + result;
    }
    if (const auto result = test_controller_snapshots(); result != 0)
    {
        return 30 + result;
    }
    return test_parallel_snapshot_versions();
}
