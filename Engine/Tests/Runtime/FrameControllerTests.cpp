#include <Platform/Windows/WindowsPlatform.h>
#include <Runtime/FrameController.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace
{
/// @brief OS の実行遅延に影響されず、Frame 内の各段階から単調時刻を進める
class ManualClock final : public cue::Clock
{
  public:
    /// @brief 全 Thread が同じ Test 時刻を観測する
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override
    {
        return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(m_ticks.load()));
    }

    /// @brief Callback または入力待ちの経過時間を明示的に進める
    void advance(std::chrono::nanoseconds a_duration) noexcept
    {
        m_ticks.fetch_add(a_duration.count());
    }

  private:
    std::atomic<std::int64_t> m_ticks = 0;
};

class FailSecondThreadFactory final : public cue::ThreadFactory
{
public:
    /// @brief 一回だけ二番目のWorker生成を拒否する
    explicit FailSecondThreadFactory(cue::ThreadFactory& a_delegate)
        : m_delegate(a_delegate)
    {
    }

    /// @brief 失敗後の再試行では実Factoryへ委譲する
    [[nodiscard]] cue::Result<std::unique_ptr<cue::Thread>> start(cue::ThreadRoutine a_routine) override
    {
        if (++m_calls == 2)
        {
            return cue::Result<std::unique_ptr<cue::Thread>>::failure(
                {cue::ErrorCategory::PlatformFailure, "Test.second.worker", 89});
        }
        return m_delegate.start(std::move(a_routine));
    }

private:
    cue::ThreadFactory& m_delegate;
    int m_calls = 0;
};

/// @brief Frame番号の順序と有界な先行数を実Workerで確認する
int test_worker_frames(cue::WindowsThreadServices& a_services)
{
    // Update と Render の記録を共有し、同じ Frame の順序違反を検出する
    std::mutex recordMutex;
    std::array<bool, 6> wasUpdated{};
    std::uint64_t renderedFrames = 0;
    std::thread::id updateThreadId;
    std::thread::id renderThreadId;
    cue::FrameController controller(
        {2, true}, *a_services.clock, *a_services.waiter, *a_services.threadFactory,
        [&](std::uint64_t a_frame, std::stop_token) {
            std::lock_guard lock(recordMutex);
            if (a_frame >= wasUpdated.size())
            {
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.update.frame"});
            }
            wasUpdated[a_frame] = true;
            updateThreadId = std::this_thread::get_id();
            return cue::Result<void>::success();
        },
        [&](std::uint64_t a_frame, std::stop_token) {
            std::lock_guard lock(recordMutex);
            if (a_frame >= wasUpdated.size() || a_frame != renderedFrames || !wasUpdated[a_frame])
            {
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.render.order"});
            }
            ++renderedFrames;
            renderThreadId = std::this_thread::get_id();
            return cue::Result<void>::success();
        });
    if (!controller.start().has_value())
    {
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (controller.progress().renderedFrames < wasUpdated.size() && std::chrono::steady_clock::now() < deadline)
    {
        // 投入数と完了数の差が上限を超えないよう進行を観測する
        const auto generation = a_services.waiter->generation();
        if (controller.progress().submittedFrames < wasUpdated.size())
        {
            auto advanceResult = controller.advance();
            if (!advanceResult.has_value())
            {
                return 2;
            }
        }
        const auto progress = controller.progress();
        if (progress.submittedFrames - progress.renderedFrames > 2)
        {
            return 3;
        }
        [[maybe_unused]] const auto waitStatus =
            a_services.waiter->wait_for_change(generation, std::chrono::milliseconds(10), {});
    }
    if (!controller.stop().has_value())
    {
        return 4;
    }
    // Callback の記録と Controller の Snapshot が同じ完了状態を示す
    const auto progress = controller.progress();
    if (progress.submittedFrames != 6 || progress.updatedFrames != 6 || progress.renderedFrames != 6)
    {
        return 5;
    }
    if (progress.lastUpdateFrame != 5 || progress.lastRenderFrame != 5 ||
        progress.updateThreadId != updateThreadId || progress.renderThreadId != renderThreadId)
    {
        return 8;
    }
    if (updateThreadId == std::this_thread::get_id() || renderThreadId == std::this_thread::get_id() ||
        updateThreadId == renderThreadId)
    {
        return 6;
    }
    if (!controller.stop().has_value())
    {
        return 7;
    }
    return 0;
}

/// @brief 単一Thread経路が同じFrameのUpdate後にRenderを処理することを確認する
int test_single_thread(cue::WindowsThreadServices& a_services)
{
    // Worker を作らず、Main Thread 上の Update 後に Render が走る条件を作る
    bool wasUpdated = false;
    const auto ownerId = std::this_thread::get_id();
    cue::FrameController controller(
        {1, false}, *a_services.clock, *a_services.waiter, *a_services.threadFactory,
        [&](std::uint64_t a_frame, std::stop_token) {
            wasUpdated = a_frame == 0 && std::this_thread::get_id() == ownerId;
            return cue::Result<void>::success();
        },
        [&](std::uint64_t a_frame, std::stop_token) {
            if (!wasUpdated || a_frame != 0)
            {
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.single.order"});
            }
            return cue::Result<void>::success();
        });
    if (!controller.start().has_value())
    {
        return 1;
    }
    auto advanceResult = controller.advance();
    if (!advanceResult.has_value() || !*advanceResult.try_value())
    {
        return 2;
    }
    const auto progress = controller.progress();
    if (progress.renderedFrames != 1 || progress.lastUpdateFrame != 0 || progress.lastRenderFrame != 0 ||
        progress.updateThreadId != ownerId || progress.renderThreadId != ownerId ||
        !controller.stop().has_value())
    {
        return 3;
    }
    return 0;
}

/// @brief Hostが開始前にCallbackを一度だけ登録できることを確認する
int test_callback_registration(cue::WindowsThreadServices& a_services)
{
    // 登録前の開始と欠けた Callback は拒否する
    cue::FrameController controller({1, false, 0}, *a_services.clock, *a_services.waiter,
                                    *a_services.threadFactory);
    if (controller.start().has_value())
    {
        return 1;
    }
    if (controller.register_callbacks({},
                                      [](std::uint64_t, std::stop_token) {
                                          return cue::Result<void>::success();
                                      }).has_value())
    {
        return 2;
    }
    bool wasUpdated = false;
    bool wasRendered = false;
    // 正しい Callback は一度だけ登録でき、開始後の差替えは拒否する
    auto registerResult = controller.register_callbacks(
        [&](std::uint64_t a_frame, std::stop_token) {
            wasUpdated = a_frame == 0;
            return cue::Result<void>::success();
        },
        [&](std::uint64_t a_frame, std::stop_token) {
            wasRendered = wasUpdated && a_frame == 0;
            return cue::Result<void>::success();
        });
    if (!registerResult.has_value() ||
        controller.register_callbacks([](std::uint64_t, std::stop_token) {
                                          return cue::Result<void>::success();
                                      },
                                      [](std::uint64_t, std::stop_token) {
                                          return cue::Result<void>::success();
                                      }).has_value())
    {
        return 3;
    }
    if (!controller.start().has_value() ||
        controller.register_callbacks([](std::uint64_t, std::stop_token) {
                                          return cue::Result<void>::success();
                                      },
                                      [](std::uint64_t, std::stop_token) {
                                          return cue::Result<void>::success();
                                      }).has_value())
    {
        return 4;
    }
    auto stepResult = controller.step();
    if (!stepResult.has_value() || !wasUpdated || !wasRendered)
    {
        return 5;
    }
    return controller.stop().has_value() ? 0 : 6;
}

/// @brief Main / Update の前で開始を制限し、待機後の入力を描画へ渡すことを確認する
int test_frame_start_order(cue::WindowsThreadServices &a_services)
{
    for (const bool usesWorkers : {false, true})
    {
        ManualClock clock;
        int latestInput = 10;
        std::array<int, 3> inputs{};
        std::array<std::chrono::steady_clock::time_point, 3> starts{};
        cue::FrameController controller({2, usesWorkers, 20}, clock, *a_services.waiter, *a_services.threadFactory);
        auto registered = controller.register_callbacks(
            [&](std::uint64_t a_frame, std::stop_token)
            {
                if (a_frame >= inputs.size() || inputs[a_frame] != latestInput)
                {
                    return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.pacing.input"});
                }
                clock.advance(std::chrono::milliseconds(3));
                return cue::Result<void>::success();
            },
            [&](std::uint64_t, std::stop_token)
            {
                clock.advance(std::chrono::milliseconds(4));
                return cue::Result<void>::success();
            },
            [&](std::uint64_t a_frame, std::stop_token)
            {
                if (a_frame >= starts.size())
                {
                    return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.pacing.frame"});
                }
                starts[a_frame] = clock.now();
                inputs[a_frame] = latestInput;
                clock.advance(std::chrono::milliseconds(2));
                return cue::Result<void>::success();
            });
        if (!registered.has_value() || !controller.start().has_value())
        {
            return 1;
        }
        // 開始待機だけでは Callback を実行せず、最初の Frame は直ちに許可する
        auto ready = controller.wait_for_frame();
        auto first = controller.advance();
        if (!ready.has_value() || !*ready.try_value() || !first.has_value() || !*first.try_value())
        {
            return 2;
        }
        auto waitForCompletion = [&](std::uint64_t a_count)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (controller.progress().renderedFrames < a_count && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::yield();
            }
            return controller.progress().renderedFrames == a_count;
        };
        if (!waitForCompletion(1) || clock.now().time_since_epoch() != std::chrono::milliseconds(9))
        {
            // Render 後に 50 ms まで待つ実装へ戻った場合、完了を公開できない
            return 3;
        }
        auto blocked = controller.advance();
        latestInput = 42;
        auto waiting = controller.wait_for_frame();
        if (!blocked.has_value() || *blocked.try_value() || !waiting.has_value() || *waiting.try_value() ||
            controller.progress().submittedFrames != 1 || inputs[1] != 0)
        {
            return 4;
        }
        clock.advance(std::chrono::milliseconds(40));
        auto tooEarly = controller.advance();
        if (!tooEarly.has_value() || *tooEarly.try_value())
        {
            return 5;
        }
        clock.advance(std::chrono::milliseconds(1));
        ready = controller.wait_for_frame();
        // Host の Message Pump に相当する入力更新は開始待機を終えてから行う
        latestInput = 99;
        auto second = controller.advance();
        if (!ready.has_value() || !*ready.try_value() || !second.has_value() || !*second.try_value() ||
            !waitForCompletion(2) || inputs[1] != 99 || starts[1] - starts[0] != std::chrono::milliseconds(50))
        {
            return 6;
        }
        const auto timing = controller.timing_info();
        if (timing.main.lastDuration != std::chrono::milliseconds(2) ||
            timing.update.lastDuration != std::chrono::milliseconds(3) ||
            timing.render.lastDuration != std::chrono::milliseconds(4) ||
            timing.limitWait.lastDuration != std::chrono::milliseconds(41) || timing.limitWait.sampleCount != 2)
        {
            return 7;
        }
        // 長い停止後も以前の期限へ追いつくための連続投入は行わない
        clock.advance(std::chrono::seconds(1));
        auto late = controller.advance();
        if (!late.has_value() || !*late.try_value() || !waitForCompletion(3))
        {
            return 8;
        }
        auto burst = controller.advance();
        if (!burst.has_value() || *burst.try_value() || !controller.stop().has_value())
        {
            return 9;
        }
    }
    return 0;
}

/// @brief 指定 FPS で開始した空 Callback の Frame 間隔と独立した待機計測を確認する
int test_fps_limit(cue::WindowsThreadServices& a_services)
{
    // 20 FPS の二つ目の完了までに最低限の間隔が空くことを測る
    cue::FrameController controller(
        {1, false, 20}, *a_services.clock, *a_services.waiter, *a_services.threadFactory,
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); },
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); });
    if (!controller.start().has_value() || !controller.step().has_value())
    {
        return 1;
    }
    const auto firstCompletion = a_services.clock->now();
    const auto deadline = firstCompletion + std::chrono::seconds(2);
    while (controller.progress().renderedFrames < 2 && a_services.clock->now() < deadline)
    {
        auto secondStep = controller.step();
        if (!secondStep.has_value())
        {
            return 2;
        }
    }
    const auto elapsed = a_services.clock->now() - firstCompletion;
    const auto progress = controller.progress();
    if (elapsed < std::chrono::milliseconds(40) || progress.renderedFrames != 2 ||
        progress.lastFrameInterval < std::chrono::milliseconds(40))
    {
        return 3;
    }
    // Callback の時間と FPS 待機を別に集計し、二つ目の完了間隔だけを記録する
    const auto timings = controller.timing_info();
    if (timings.update.sampleCount != 2 || timings.render.sampleCount != 2 || timings.limitWait.sampleCount != 2 ||
        timings.frameInterval.sampleCount != 1 || timings.limitWait.lastDuration < std::chrono::milliseconds(40) ||
        timings.render.lastDuration != progress.lastRenderDuration ||
        timings.limitWait.lastDuration != progress.lastLimitWaitDuration)
    {
        return 5;
    }
    return controller.stop().has_value() ? 0 : 4;
}

/// @brief Worker 経路でも空 Callback の開始制限が適用されることを確認する
int test_worker_fps_limit(cue::WindowsThreadServices& a_services)
{
    // Worker 経路でも Render 完了間隔を Snapshot から確認する
    cue::FrameController controller(
        {1, true, 20}, *a_services.clock, *a_services.waiter, *a_services.threadFactory,
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); },
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); });
    if (!controller.start().has_value())
    {
        return 1;
    }
    const auto deadline = a_services.clock->now() + std::chrono::seconds(5);
    while (controller.progress().renderedFrames < 2 && a_services.clock->now() < deadline)
    {
        if (!controller.step().has_value())
        {
            return 2;
        }
    }
    const auto progress = controller.progress();
    if (progress.renderedFrames < 2 || progress.lastFrameInterval < std::chrono::milliseconds(40))
    {
        return 3;
    }
    return controller.stop().has_value() ? 0 : 4;
}

/// @brief 低 FPS の開始待ちでも Main に短時間で戻り、Worker を速やかに回収する
int test_stop_during_fps_limit(cue::WindowsThreadServices& a_services)
{
    // 1 FPS の次回開始を待ち切らず Main に戻り、停止操作を行えることを測る
    std::atomic<int> renderCalls = 0;
    cue::FrameController controller(
        {2, true, 1}, *a_services.clock, *a_services.waiter, *a_services.threadFactory,
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); },
        [&](std::uint64_t, std::stop_token) {
            ++renderCalls;
            return cue::Result<void>::success();
        });
    if (!controller.start().has_value() || !controller.step().has_value())
    {
        return 1;
    }
    const auto deadline = a_services.clock->now() + std::chrono::seconds(5);
    while (controller.progress().renderedFrames < 1 && a_services.clock->now() < deadline)
    {
        [[maybe_unused]] const auto status = a_services.waiter->sleep_for(std::chrono::milliseconds(1), {});
    }
    const auto waitStart = a_services.clock->now();
    auto pending = controller.step();
    if (!pending.has_value() || *pending.try_value() || renderCalls.load() != 1 ||
        controller.progress().renderedFrames != 1 ||
        a_services.clock->now() - waitStart >= std::chrono::milliseconds(100))
    {
        return 2;
    }
    const auto stopStart = a_services.clock->now();
    if (!controller.stop().has_value())
    {
        return 3;
    }
    return a_services.clock->now() - stopStart < std::chrono::milliseconds(500) ? 0 : 4;
}

/// @brief Workerの失敗がMainThreadへ伝わり停止できることを確認する
int test_failure(cue::WindowsThreadServices& a_services)
{
    // Update Worker で発生した Native 診断値を Main Thread と停止結果で照合する
    cue::FrameController controller(
        {1, true}, *a_services.clock, *a_services.waiter, *a_services.threadFactory,
        [](std::uint64_t, std::stop_token) {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.update.failure", 37});
        },
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); });
    if (!controller.start().has_value() || !controller.advance().has_value())
    {
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool sawFailure = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto result = controller.advance();
        if (!result.has_value())
        {
            auto idle = controller.is_idle();
            sawFailure =
                result.try_error()->nativeCode == 37 && !idle.has_value() && idle.try_error()->nativeCode == 37;
            break;
        }
        const auto generation = a_services.waiter->generation();
        [[maybe_unused]] const auto waitStatus =
            a_services.waiter->wait_for_change(generation, std::chrono::milliseconds(10), {});
    }
    auto stopResult = controller.stop();
    return sawFailure && !stopResult.has_value() && stopResult.try_error()->nativeCode == 37 ? 0 : 2;
}

/// @brief Render失敗とCallback例外がMainThreadへ伝わることを確認する
int test_render_failure(cue::WindowsThreadServices& a_services)
{
    // Render Callback の例外は Worker 外へ投げず Result に変換する
    cue::FrameController controller(
        {1, true}, *a_services.clock, *a_services.waiter, *a_services.threadFactory,
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); },
        [](std::uint64_t, std::stop_token) -> cue::Result<void> { throw std::runtime_error("render failure"); });
    if (!controller.start().has_value() || !controller.advance().has_value())
    {
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool sawFailure = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto result = controller.advance();
        if (!result.has_value())
        {
            auto idle = controller.is_idle();
            sawFailure = result.try_error()->operation == "FrameController.render.exception" && !idle.has_value() &&
                         idle.try_error()->operation == "FrameController.render.exception";
            break;
        }
        const auto generation = a_services.waiter->generation();
        [[maybe_unused]] const auto waitStatus =
            a_services.waiter->wait_for_change(generation, std::chrono::milliseconds(10), {});
    }
    auto stopResult = controller.stop();
    return sawFailure && !stopResult.has_value() ? 0 : 2;
}

/// @brief 二番目のWorker起動失敗で一番目を回収して再試行できることを確認する
int test_start_rollback(cue::WindowsThreadServices& a_services)
{
    // 二番目の Worker だけ失敗させ、最初の Worker が残らないことを確認する
    FailSecondThreadFactory factory(*a_services.threadFactory);
    cue::FrameController controller(
        {2, true}, *a_services.clock, *a_services.waiter, factory,
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); },
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); });
    auto firstStart = controller.start();
    if (firstStart.has_value() || firstStart.try_error()->nativeCode != 89)
    {
        return 1;
    }
    if (!controller.start().has_value())
    {
        return 2;
    }
    return controller.stop().has_value() ? 0 : 3;
}

/// @brief 待機中のCallbackを停止要求で解除しWorkerを回収する
int test_stop_during_callback(cue::WindowsThreadServices& a_services)
{
    // Callback 内で停止可能な長い待機を作る
    std::atomic<bool> entered = false;
    cue::FrameController controller(
        {1, true}, *a_services.clock, *a_services.waiter, *a_services.threadFactory,
        [&](std::uint64_t, std::stop_token a_token) {
            entered = true;
            [[maybe_unused]] const auto waitStatus = a_services.waiter->sleep_for(std::chrono::seconds(10), a_token);
            return cue::Result<void>::success();
        },
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); });
    if (!controller.start().has_value() || !controller.advance().has_value())
    {
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered && std::chrono::steady_clock::now() < deadline)
    {
        [[maybe_unused]] const auto waitStatus = a_services.waiter->sleep_for(std::chrono::milliseconds(1), {});
    }
    if (!entered)
    {
        return 2;
    }
    // Stop Token により Callback が待機を解除し、期限内に停止する
    const auto stopStart = std::chrono::steady_clock::now();
    if (!controller.stop().has_value())
    {
        return 3;
    }
    return std::chrono::steady_clock::now() - stopStart < std::chrono::seconds(5) ? 0 : 4;
}

/// @brief Main は構築 Thread で一度だけ準備し、満杯時に呼ばず、準備前に Worker へ公開しない
int test_main_stage(cue::WindowsThreadServices &a_services)
{
    for (bool usesWorkers : {false, true})
    {
        const auto ownerId = std::this_thread::get_id();
        std::atomic<bool> wasPrepared = false;
        std::atomic<bool> wasUpdated = false;
        std::atomic<bool> allowsUpdate = !usesWorkers;
        int mainCalls = 0;
        cue::FrameController controller({1, usesWorkers, 0}, *a_services.clock, *a_services.waiter,
                                        *a_services.threadFactory);
        auto beforeStart = controller.is_idle();
        if (beforeStart.has_value() || beforeStart.try_error()->category != cue::ErrorCategory::InvalidState)
        {
            return 5;
        }
        auto registered = controller.register_callbacks(
            [&](std::uint64_t a_frame, std::stop_token a_token)
            {
                if (!wasPrepared.load() || a_frame != 0 || (std::this_thread::get_id() == ownerId) == usesWorkers)
                {
                    return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.main.order"});
                }
                // Worker を停止可能な待機に置き、投入枠が満杯の間の Main 呼出数を検査する
                while (!allowsUpdate.load() && !a_token.stop_requested())
                {
                    [[maybe_unused]] const auto status =
                        a_services.waiter->sleep_for(std::chrono::milliseconds(1), a_token);
                }
                wasUpdated = true;
                return cue::Result<void>::success();
            },
            [&](std::uint64_t, std::stop_token)
            {
                return wasUpdated.load()
                           ? cue::Result<void>::success()
                           : cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.main.render"});
            },
            [&](std::uint64_t a_frame, std::stop_token)
            {
                ++mainCalls;
                // Main 内の Snapshot 参照は許可し、投入と停止の再入は拒否する
                auto nested = controller.step();
                auto stopped = controller.stop();
                if (std::this_thread::get_id() != ownerId || a_frame != 0 ||
                    controller.progress().submittedFrames != 0 || nested.has_value() || stopped.has_value())
                {
                    return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.main.thread"});
                }
                wasPrepared = true;
                return cue::Result<void>::success();
            });
        if (!registered.has_value() || !controller.start().has_value())
        {
            return 1;
        }
        auto initiallyIdle = controller.is_idle();
        if (!initiallyIdle.has_value() || !*initiallyIdle.try_value())
        {
            return 6;
        }
        auto accepted = controller.advance();
        if (!accepted.has_value() || !*accepted.try_value() || mainCalls != 1)
        {
            return 2;
        }
        if (usesWorkers)
        {
            auto pending = controller.is_idle();
            if (!pending.has_value() || *pending.try_value())
            {
                return 7;
            }
            auto full = controller.step();
            if (!full.has_value() || *full.try_value() || mainCalls != 1)
            {
                return 3;
            }
        }
        allowsUpdate = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (controller.progress().renderedFrames == 0 && std::chrono::steady_clock::now() < deadline)
        {
            [[maybe_unused]] const auto status = a_services.waiter->sleep_for(std::chrono::milliseconds(1), {});
        }
        auto completed = controller.is_idle();
        if (controller.progress().renderedFrames != 1 || !completed.has_value() || !*completed.try_value() ||
            !controller.stop().has_value())
        {
            return 4;
        }
    }
    return 0;
}

/// @brief Main の Error と例外を保存し、未完成 Frame の投入と Update / Render を止める
int test_main_failure(cue::WindowsThreadServices &a_services)
{
    for (bool usesWorkers : {false, true})
    {
        for (bool throws : {false, true})
        {
            std::atomic<int> workerCalls = 0;
            cue::FrameController controller({1, usesWorkers, 0}, *a_services.clock, *a_services.waiter,
                                            *a_services.threadFactory);
            auto worker = [&](std::uint64_t, std::stop_token)
            {
                ++workerCalls;
                return cue::Result<void>::success();
            };
            auto registered = controller.register_callbacks(
                worker, worker,
                [&](std::uint64_t, std::stop_token) -> cue::Result<void>
                {
                    if (throws)
                    {
                        throw std::runtime_error("main failure");
                    }
                    return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.main.failure"});
                });
            if (!registered.has_value() || !controller.start().has_value())
            {
                return 1;
            }
            const auto *operation = throws ? "FrameController.main.exception" : "Test.main.failure";
            auto failed = controller.step();
            auto retried = controller.step();
            auto stopped = controller.stop();
            if (failed.has_value() || retried.has_value() || stopped.has_value() ||
                failed.try_error()->operation != operation || retried.try_error()->operation != operation ||
                stopped.try_error()->operation != operation || workerCalls.load() != 0 ||
                controller.progress().submittedFrames != 0)
            {
                return 2;
            }
        }
    }
    return 0;
}

/// @brief Main の転送待機中に Update が失敗しても取消通知で待機を解除し、主原因を保持する
int test_main_wait_cancellation(cue::WindowsThreadServices &a_services)
{
    std::mutex mutex;
    std::condition_variable_any changed;
    bool isMainWaiting = false;
    bool wasCancelled = false;
    cue::FrameController controller({2, true, 0}, *a_services.clock, *a_services.waiter, *a_services.threadFactory);
    auto registered = controller.register_callbacks(
        [&](std::uint64_t, std::stop_token)
        {
            std::unique_lock lock(mutex);
            if (!changed.wait_for(lock, std::chrono::seconds(2), [&]() { return isMainWaiting; }))
            {
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.update.timeout"});
            }
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.update.failure"});
        },
        [](std::uint64_t, std::stop_token) { return cue::Result<void>::success(); },
        [&](std::uint64_t a_frame, std::stop_token a_token)
        {
            if (a_frame == 0)
            {
                return cue::Result<void>::success();
            }
            std::unique_lock lock(mutex);
            isMainWaiting = true;
            changed.notify_all();
            [[maybe_unused]] const bool ready =
                changed.wait_for(lock, a_token, std::chrono::seconds(2), []() { return false; });
            wasCancelled = a_token.stop_requested();
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.main.cancelled"});
        });
    if (!registered.has_value() || !controller.start().has_value() || !controller.advance().has_value())
    {
        return 1;
    }
    auto result = controller.advance();
    auto stopped = controller.stop();
    return wasCancelled && !result.has_value() && !stopped.has_value() &&
                   result.try_error()->operation == "Test.update.failure" &&
                   stopped.try_error()->operation == "Test.update.failure"
               ? 0
               : 2;
}

/// @brief FrameControllerの有界実行、Fallback、失敗伝播を確認する
int run_tests()
{
    auto servicesResult = cue::create_windows_thread_services();
    if (!servicesResult.has_value())
    {
        return 1;
    }
    auto services = servicesResult.take_value();
    if (const int result = test_frame_start_order(services); result != 0)
    {
        return 140 + result;
    }
    if (const int result = test_main_wait_cancellation(services); result != 0)
    {
        return 130 + result;
    }
    if (const int result = test_main_stage(services); result != 0)
    {
        return 110 + result;
    }
    if (const int result = test_main_failure(services); result != 0)
    {
        return 120 + result;
    }
    if (const int result = test_worker_frames(services); result != 0)
    {
        return 10 + result;
    }
    if (const int result = test_single_thread(services); result != 0)
    {
        return 20 + result;
    }
    if (const int result = test_callback_registration(services); result != 0)
    {
        return 100 + result;
    }
    if (const int result = test_fps_limit(services); result != 0)
    {
        return 70 + result;
    }
    if (const int result = test_worker_fps_limit(services); result != 0)
    {
        return 80 + result;
    }
    if (const int result = test_stop_during_fps_limit(services); result != 0)
    {
        return 90 + result;
    }
    if (const int result = test_failure(services); result != 0)
    {
        return 30 + result;
    }
    if (const int result = test_render_failure(services); result != 0)
    {
        return 40 + result;
    }
    if (const int result = test_start_rollback(services); result != 0)
    {
        return 50 + result;
    }
    if (const int result = test_stop_during_callback(services); result != 0)
    {
        return 60 + result;
    }
    return 0;
}
} // namespace

/// @brief 実Workerと単一ThreadのFrame順序を検証する
int main()
{
    return run_tests();
}
