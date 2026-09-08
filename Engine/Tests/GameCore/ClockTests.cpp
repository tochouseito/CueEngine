#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/Error.h>

#include "ClockInternals.h"

#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
static_assert(!std::is_constructible_v<cue::game_core::UpdateContext, cue::game_core::FrameTiming>);

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Clock Test中の契約違反を固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(77);
    }

    /// @brief Message付き契約違反を固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(78);
    }
};

class ScriptedClock final : public cue::game_core::MonotonicClock
{
  public:
    /// @brief 呼出し順に返すTest Sample列を非所有で保持する
    explicit ScriptedClock(std::span<const std::int64_t> a_samples) noexcept : m_samples(a_samples)
    {
    }

    /// @brief Scripted Sampleを順に返し、列を超えた呼出しを診断する
    [[nodiscard]] cue::Result<cue::game_core::MonotonicClockSample> sample(
        const cue::AssertContext &a_assertContext) noexcept override
    {
        if (m_nextIndex == m_samples.size())
        {
            return cue::Result<cue::game_core::MonotonicClockSample>::failure(cue::game_core::make_game_core_error(
                a_assertContext, cue::game_core::GameCoreError::InvalidClockSample, "Test clock sample exhausted"));
        }
        return cue::Result<cue::game_core::MonotonicClockSample>::success(
            cue::game_core::MonotonicClockSample{m_samples[m_nextIndex++]});
    }

  private:
    std::span<const std::int64_t> m_samples;
    std::size_t m_nextIndex = 0;
};

class OverflowClock final : public cue::game_core::MonotonicClock
{
  public:
    /// @brief Native Clock変換OverflowをTest Errorとして返す
    [[nodiscard]] cue::Result<cue::game_core::MonotonicClockSample> sample(
        const cue::AssertContext &a_assertContext) noexcept override
    {
        return cue::Result<cue::game_core::MonotonicClockSample>::failure(cue::game_core::make_game_core_error(
            a_assertContext, cue::game_core::GameCoreError::ClockOverflow, "Injected clock overflow"));
    }
};

/// @brief Test用Loggerを追加Sinkなしで生成する
[[nodiscard]] std::unique_ptr<cue::Logger> create_logger(TestFatalHandler &a_handler)
{
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    return std::make_unique<cue::Logger>(a_handler, std::move(sinks));
}

/// @brief Result Errorが期待するGameCore Codeの場合にtrueを返す
template <typename T>
[[nodiscard]] bool has_error_code(const cue::Result<T> &a_result, cue::game_core::GameCoreError a_code) noexcept
{
    return !a_result && a_result.try_error()->code().domain() == "Cue.GameCore" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_code);
}

/// @brief 同じSample列からFrame Index、Clamp、Simulation Timeを決定的に生成することを検証する
[[nodiscard]] bool test_deterministic_timing(cue::AssertContext &a_context) noexcept
{
    constexpr std::array<std::int64_t, 4> k_samples = {100, 110, 200, 225};
    ScriptedClock firstSource(k_samples);
    ScriptedClock secondSource(k_samples);
    cue::Result<cue::game_core::GameClock> firstResult = cue::game_core::GameClock::create(firstSource, 50, a_context);
    cue::Result<cue::game_core::GameClock> secondResult =
        cue::game_core::GameClock::create(secondSource, 50, a_context);
    if (!firstResult || !secondResult)
    {
        return false;
    }

    cue::game_core::GameClock first = std::move(*firstResult.try_value());
    cue::game_core::GameClock second = std::move(*secondResult.try_value());
    if (!first.reset() || !second.reset())
    {
        return false;
    }

    for (std::uint64_t index = 0; index < 3; ++index)
    {
        cue::Result<cue::game_core::UpdateContext> firstFrame = first.advance_frame();
        cue::Result<cue::game_core::UpdateContext> secondFrame = second.advance_frame();
        if (!firstFrame || !secondFrame)
        {
            return false;
        }

        const cue::game_core::FrameTiming &firstTiming = firstFrame.try_value()->timing();
        const cue::game_core::FrameTiming &secondTiming = secondFrame.try_value()->timing();
        if (firstTiming.frameIndex != index || firstTiming.frameIndex != secondTiming.frameIndex ||
            firstTiming.observedDeltaNanoseconds != secondTiming.observedDeltaNanoseconds ||
            firstTiming.simulationDeltaNanoseconds != secondTiming.simulationDeltaNanoseconds ||
            firstTiming.simulationTimeNanoseconds != secondTiming.simulationTimeNanoseconds ||
            firstTiming.wasDeltaClamped != secondTiming.wasDeltaClamped)
        {
            return false;
        }
    }

    return first.simulation_time_nanoseconds() == 85 && first.next_frame_index() == 3;
}

/// @brief 長時間Frameを上限へClampし、秒変換がSimulation Deltaを使用することを検証する
[[nodiscard]] bool test_delta_clamp(cue::AssertContext &a_context) noexcept
{
    constexpr std::array<std::int64_t, 2> k_samples = {1'000'000'000, 3'000'000'000};
    ScriptedClock source(k_samples);
    cue::Result<cue::game_core::GameClock> clockResult =
        cue::game_core::GameClock::create(source, 250'000'000, a_context);
    if (!clockResult)
    {
        return false;
    }
    cue::game_core::GameClock clock = std::move(*clockResult.try_value());
    if (!clock.reset())
    {
        return false;
    }

    cue::Result<cue::game_core::UpdateContext> frame = clock.advance_frame();
    if (!frame)
    {
        return false;
    }
    const cue::game_core::FrameTiming &timing = frame.try_value()->timing();
    return timing.observedDeltaNanoseconds == 2'000'000'000 && timing.simulationDeltaNanoseconds == 250'000'000 &&
           timing.simulationTimeNanoseconds == 250'000'000 && timing.wasDeltaClamped &&
           frame.try_value()->delta_seconds() == 0.25 && frame.try_value()->simulation_seconds() == 0.25;
}

/// @brief Pause中とResume直前までの時間をSimulationへ加算しないことを検証する
[[nodiscard]] bool test_pause_resume(cue::AssertContext &a_context) noexcept
{
    constexpr std::array<std::int64_t, 5> k_samples = {100, 110, 1'000, 5'000, 5'007};
    ScriptedClock source(k_samples);
    cue::Result<cue::game_core::GameClock> clockResult = cue::game_core::GameClock::create(source, 100, a_context);
    if (!clockResult)
    {
        return false;
    }
    cue::game_core::GameClock clock = std::move(*clockResult.try_value());
    if (!clock.reset() || !clock.advance_frame() || !clock.pause() || !clock.is_paused())
    {
        return false;
    }

    cue::Result<cue::game_core::UpdateContext> pausedFrame = clock.advance_frame();
    if (!pausedFrame || pausedFrame.try_value()->timing().simulationDeltaNanoseconds != 0 ||
        pausedFrame.try_value()->timing().simulationTimeNanoseconds != 10 ||
        !pausedFrame.try_value()->timing().isPaused || !clock.resume() || clock.is_paused())
    {
        return false;
    }

    cue::Result<cue::game_core::UpdateContext> resumedFrame = clock.advance_frame();
    return resumedFrame && resumedFrame.try_value()->timing().frameIndex == 2 &&
           resumedFrame.try_value()->timing().observedDeltaNanoseconds == 7 &&
           resumedFrame.try_value()->timing().simulationDeltaNanoseconds == 7 &&
           resumedFrame.try_value()->timing().simulationTimeNanoseconds == 17;
}

/// @brief 設定不正、未Reset、負値、逆行、Clock Overflowを分類して状態を保持することを検証する
[[nodiscard]] bool test_invalid_values(cue::AssertContext &a_context) noexcept
{
    constexpr std::array<std::int64_t, 1> k_validSamples = {100};
    ScriptedClock invalidConfigSource(k_validSamples);
    cue::Result<cue::game_core::GameClock> invalidConfig =
        cue::game_core::GameClock::create(invalidConfigSource, 0, a_context);
    if (!has_error_code(invalidConfig, cue::game_core::GameCoreError::InvalidClockConfiguration))
    {
        return false;
    }

    ScriptedClock uninitializedSource(k_validSamples);
    cue::Result<cue::game_core::GameClock> uninitializedResult =
        cue::game_core::GameClock::create(uninitializedSource, 10, a_context);
    if (!uninitializedResult)
    {
        return false;
    }
    cue::game_core::GameClock uninitialized = std::move(*uninitializedResult.try_value());
    cue::Result<cue::game_core::UpdateContext> beforeReset = uninitialized.advance_frame();
    cue::Result<void> pauseBeforeReset = uninitialized.pause();
    cue::Result<void> resumeBeforeReset = uninitialized.resume();
    if (!has_error_code(beforeReset, cue::game_core::GameCoreError::InvalidClockState) ||
        !has_error_code(pauseBeforeReset, cue::game_core::GameCoreError::InvalidClockState) ||
        !has_error_code(resumeBeforeReset, cue::game_core::GameCoreError::InvalidClockState))
    {
        return false;
    }

    constexpr std::array<std::int64_t, 1> k_negativeSamples = {-1};
    ScriptedClock negativeSource(k_negativeSamples);
    cue::Result<cue::game_core::GameClock> negativeResult =
        cue::game_core::GameClock::create(negativeSource, 10, a_context);
    if (!negativeResult)
    {
        return false;
    }
    cue::game_core::GameClock negativeClock = std::move(*negativeResult.try_value());
    cue::Result<void> negativeReset = negativeClock.reset();
    if (!has_error_code(negativeReset, cue::game_core::GameCoreError::InvalidClockSample) ||
        negativeClock.is_initialized())
    {
        return false;
    }

    constexpr std::array<std::int64_t, 3> k_backwardSamples = {100, 90, 110};
    ScriptedClock backwardSource(k_backwardSamples);
    cue::Result<cue::game_core::GameClock> backwardResult =
        cue::game_core::GameClock::create(backwardSource, 50, a_context);
    if (!backwardResult)
    {
        return false;
    }
    cue::game_core::GameClock backwardClock = std::move(*backwardResult.try_value());
    if (!backwardClock.reset())
    {
        return false;
    }
    cue::Result<cue::game_core::UpdateContext> backwardFrame = backwardClock.advance_frame();
    cue::Result<cue::game_core::UpdateContext> recoveredFrame = backwardClock.advance_frame();
    if (!has_error_code(backwardFrame, cue::game_core::GameCoreError::InvalidClockSample) || !recoveredFrame ||
        recoveredFrame.try_value()->timing().frameIndex != 0 ||
        recoveredFrame.try_value()->timing().simulationTimeNanoseconds != 10)
    {
        return false;
    }

    OverflowClock overflowSource;
    cue::Result<cue::game_core::GameClock> overflowResult =
        cue::game_core::GameClock::create(overflowSource, 10, a_context);
    if (!overflowResult)
    {
        return false;
    }
    cue::game_core::GameClock overflowClock = std::move(*overflowResult.try_value());
    cue::Result<void> overflowReset = overflowClock.reset();
    return has_error_code(overflowReset, cue::game_core::GameCoreError::ClockOverflow) &&
           !overflowClock.is_initialized();
}

/// @brief GameClock移動後に移動元がClock Sourceを再消費できないことを検証する
[[nodiscard]] bool test_move_invalidates_source(cue::AssertContext &a_context) noexcept
{
    constexpr std::array<std::int64_t, 2> k_samples = {100, 110};
    ScriptedClock source(k_samples);
    cue::Result<cue::game_core::GameClock> clockResult = cue::game_core::GameClock::create(source, 50, a_context);
    if (!clockResult || !clockResult.try_value()->reset())
    {
        return false;
    }

    cue::game_core::GameClock moved = std::move(*clockResult.try_value());
    cue::Result<void> movedFromReset = clockResult.try_value()->reset();
    if (clockResult.try_value()->is_initialized() ||
        !has_error_code(movedFromReset, cue::game_core::GameCoreError::InvalidClockState))
    {
        return false;
    }

    return moved.is_initialized() && moved.advance_frame().has_value();
}

/// @brief 初期化済みClockの再Resetが逆行Sampleで既存状態を失わないことを検証する
[[nodiscard]] bool test_reset_preserves_state_on_backward_sample(cue::AssertContext &a_context) noexcept
{
    constexpr std::array<std::int64_t, 4> k_samples = {100, 110, 90, 120};
    ScriptedClock source(k_samples);
    cue::Result<cue::game_core::GameClock> clockResult = cue::game_core::GameClock::create(source, 50, a_context);
    if (!clockResult)
    {
        return false;
    }

    cue::game_core::GameClock clock = std::move(*clockResult.try_value());
    if (!clock.reset() || !clock.advance_frame())
    {
        return false;
    }

    cue::Result<void> resetResult = clock.reset();
    if (!has_error_code(resetResult, cue::game_core::GameCoreError::InvalidClockSample) ||
        clock.next_frame_index() != 1 || clock.simulation_time_nanoseconds() != 10)
    {
        return false;
    }

    cue::Result<cue::game_core::UpdateContext> recovered = clock.advance_frame();
    return recovered && recovered.try_value()->timing().frameIndex == 1 &&
           recovered.try_value()->timing().simulationTimeNanoseconds == 20;
}

/// @brief Simulation TimeとFrame Indexの内部Overflow分岐が状態Commit前に拒否されることを検証する
[[nodiscard]] bool test_internal_overflow_paths(cue::AssertContext &a_context) noexcept
{
    cue::game_core::details::ClockAdvanceState simulationState = {
        100, std::numeric_limits<std::int64_t>::max() - 5, 7, false, false,
    };
    cue::Result<cue::game_core::details::ClockAdvance> simulationOverflow =
        cue::game_core::details::calculate_frame_advance(simulationState, 110, 20, a_context);
    if (!has_error_code(simulationOverflow, cue::game_core::GameCoreError::ClockOverflow) ||
        simulationState.lastSampleNanoseconds != 100 ||
        simulationState.simulationTimeNanoseconds != std::numeric_limits<std::int64_t>::max() - 5 ||
        simulationState.nextFrameIndex != 7)
    {
        return false;
    }

    cue::game_core::details::ClockAdvanceState frameState = {
        100, 0, std::numeric_limits<std::uint64_t>::max(), false, false,
    };
    cue::Result<cue::game_core::details::ClockAdvance> lastFrame =
        cue::game_core::details::calculate_frame_advance(frameState, 101, 20, a_context);
    if (!lastFrame || lastFrame.try_value()->timing.frameIndex != std::numeric_limits<std::uint64_t>::max() ||
        !lastFrame.try_value()->state.isFrameIndexExhausted)
    {
        return false;
    }

    cue::Result<cue::game_core::details::ClockAdvance> frameOverflow =
        cue::game_core::details::calculate_frame_advance(lastFrame.try_value()->state, 102, 20, a_context);
    return has_error_code(frameOverflow, cue::game_core::GameCoreError::ClockOverflow);
}

/// @brief Standard steady_clock Adapterが非負かつ非逆行のSampleを返すことを検証する
[[nodiscard]] bool test_steady_clock(cue::AssertContext &a_context) noexcept
{
    cue::game_core::SteadyMonotonicClock source;
    cue::Result<cue::game_core::MonotonicClockSample> first = source.sample(a_context);
    cue::Result<cue::game_core::MonotonicClockSample> second = source.sample(a_context);
    return first && second && first.try_value()->nanoseconds >= 0 &&
           second.try_value()->nanoseconds >= first.try_value()->nanoseconds;
}
} // namespace

/// @brief Game Clockの単調時間、Clamp、Pause、診断、決定性を検証する
int main()
{
    TestFatalHandler handler;
    std::unique_ptr<cue::Logger> logger = create_logger(handler);
    cue::AssertContext context(*logger, handler);
    return test_deterministic_timing(context) && test_delta_clamp(context) && test_pause_resume(context) &&
                   test_invalid_values(context) && test_move_invalidates_source(context) &&
                   test_reset_preserves_state_on_backward_sample(context) && test_internal_overflow_paths(context) &&
                   test_steady_clock(context)
               ? 0
               : 1;
}
