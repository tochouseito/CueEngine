#include <Cue/GameCore/Clock.h>

#include "ClockInternals.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/GameCore/Error.h>

#include <chrono>
#include <limits>
#include <utility>

namespace
{
constexpr long double k_nanosecondsPerSecond = 1'000'000'000.0L;

/// @brief Error Resultから所有Errorを移動し、上位処理で同じ診断を保持する
template <typename T> [[nodiscard]] cue::Error take_error(cue::Result<T> &a_result) noexcept
{
    return std::move(*a_result.try_error());
}
} // namespace

namespace cue::game_core
{
UpdateContext::UpdateContext(FrameTiming a_timing) noexcept : m_timing(a_timing)
{
}

const FrameTiming &UpdateContext::timing() const noexcept
{
    return m_timing;
}

double UpdateContext::delta_seconds() const noexcept
{
    return static_cast<double>(m_timing.simulationDeltaNanoseconds) / static_cast<double>(k_nanosecondsPerSecond);
}

double UpdateContext::simulation_seconds() const noexcept
{
    return static_cast<double>(m_timing.simulationTimeNanoseconds) / static_cast<double>(k_nanosecondsPerSecond);
}

Result<MonotonicClockSample> SteadyMonotonicClock::sample(const AssertContext &a_assertContext) noexcept
{
    const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const std::chrono::duration<long double, std::nano> nanoseconds = elapsed;
    const long double value = nanoseconds.count();
    if (value < 0.0L || value > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
    {
        return Result<MonotonicClockSample>::failure(make_game_core_error(
            a_assertContext, GameCoreError::ClockOverflow, "Steady monotonic clock sample cannot be represented"));
    }

    return Result<MonotonicClockSample>::success(MonotonicClockSample{static_cast<std::int64_t>(value)});
}

Result<GameClock> GameClock::create(MonotonicClock &a_clock, std::int64_t a_maxDeltaNanoseconds,
                                    const AssertContext &a_assertContext) noexcept
{
    if (a_maxDeltaNanoseconds <= 0)
    {
        return Result<GameClock>::failure(make_game_core_error(
            a_assertContext, GameCoreError::InvalidClockConfiguration, "Game clock delta limit must be positive"));
    }

    return Result<GameClock>::success(GameClock(a_clock, a_maxDeltaNanoseconds, a_assertContext));
}

GameClock::GameClock(MonotonicClock &a_clock, std::int64_t a_maxDeltaNanoseconds,
                     const AssertContext &a_assertContext) noexcept
    : m_clock(&a_clock), m_assertContext(&a_assertContext), m_maxDeltaNanoseconds(a_maxDeltaNanoseconds)
{
}

GameClock::GameClock(GameClock &&a_other) noexcept
    : m_clock(a_other.m_clock), m_assertContext(a_other.m_assertContext),
      m_maxDeltaNanoseconds(a_other.m_maxDeltaNanoseconds), m_lastSampleNanoseconds(a_other.m_lastSampleNanoseconds),
      m_simulationTimeNanoseconds(a_other.m_simulationTimeNanoseconds), m_nextFrameIndex(a_other.m_nextFrameIndex),
      m_isInitialized(a_other.m_isInitialized), m_isPaused(a_other.m_isPaused),
      m_isFrameIndexExhausted(a_other.m_isFrameIndexExhausted)
{
    a_other.invalidate_after_move();
}

GameClock &GameClock::operator=(GameClock &&a_other) noexcept
{
    if (this != &a_other)
    {
        m_clock = a_other.m_clock;
        m_assertContext = a_other.m_assertContext;
        m_maxDeltaNanoseconds = a_other.m_maxDeltaNanoseconds;
        m_lastSampleNanoseconds = a_other.m_lastSampleNanoseconds;
        m_simulationTimeNanoseconds = a_other.m_simulationTimeNanoseconds;
        m_nextFrameIndex = a_other.m_nextFrameIndex;
        m_isInitialized = a_other.m_isInitialized;
        m_isPaused = a_other.m_isPaused;
        m_isFrameIndexExhausted = a_other.m_isFrameIndexExhausted;
        a_other.invalidate_after_move();
    }

    return *this;
}

Result<void> GameClock::reset() noexcept
{
    if (m_clock == nullptr)
    {
        return Result<void>::failure(make_state_error("Moved-from game clock cannot be reset"));
    }

    Result<MonotonicClockSample> sampleResult = sample_validated();
    if (!sampleResult)
    {
        return Result<void>::failure(take_error(sampleResult));
    }

    const std::int64_t sampleNanoseconds = sampleResult.try_value()->nanoseconds;
    if (m_isInitialized && sampleNanoseconds < m_lastSampleNanoseconds)
    {
        return Result<void>::failure(make_game_core_error(*m_assertContext, GameCoreError::InvalidClockSample,
                                                          "Monotonic clock sample moved backwards on reset"));
    }

    m_lastSampleNanoseconds = sampleNanoseconds;
    m_simulationTimeNanoseconds = 0;
    m_nextFrameIndex = 0;
    m_isInitialized = true;
    m_isPaused = false;
    m_isFrameIndexExhausted = false;
    return Result<void>::success();
}

Result<UpdateContext> GameClock::advance_frame() noexcept
{
    if (!m_isInitialized)
    {
        return Result<UpdateContext>::failure(make_state_error("Game clock must be reset before advancing"));
    }
    if (m_isFrameIndexExhausted)
    {
        return Result<UpdateContext>::failure(make_game_core_error(*m_assertContext, GameCoreError::ClockOverflow,
                                                                   "Game clock frame index is exhausted"));
    }

    Result<MonotonicClockSample> sampleResult = sample_validated();
    if (!sampleResult)
    {
        return Result<UpdateContext>::failure(take_error(sampleResult));
    }

    details::ClockAdvanceState state = {
        m_lastSampleNanoseconds, m_simulationTimeNanoseconds, m_nextFrameIndex, m_isPaused, m_isFrameIndexExhausted,
    };
    Result<details::ClockAdvance> advanceResult = details::calculate_frame_advance(
        state, sampleResult.try_value()->nanoseconds, m_maxDeltaNanoseconds, *m_assertContext);
    if (!advanceResult)
    {
        return Result<UpdateContext>::failure(take_error(advanceResult));
    }

    details::ClockAdvance &advance = *advanceResult.try_value();
    m_lastSampleNanoseconds = advance.state.lastSampleNanoseconds;
    m_simulationTimeNanoseconds = advance.state.simulationTimeNanoseconds;
    m_nextFrameIndex = advance.state.nextFrameIndex;
    m_isFrameIndexExhausted = advance.state.isFrameIndexExhausted;
    return Result<UpdateContext>::success(UpdateContext(advance.timing));
}

Result<void> GameClock::pause() noexcept
{
    if (!m_isInitialized)
    {
        return Result<void>::failure(make_state_error("Game clock must be reset before pausing"));
    }

    m_isPaused = true;
    return Result<void>::success();
}

Result<void> GameClock::resume() noexcept
{
    if (!m_isInitialized)
    {
        return Result<void>::failure(make_state_error("Game clock must be reset before resuming"));
    }
    if (!m_isPaused)
    {
        return Result<void>::success();
    }

    Result<MonotonicClockSample> sampleResult = sample_validated();
    if (!sampleResult)
    {
        return Result<void>::failure(take_error(sampleResult));
    }
    if (sampleResult.try_value()->nanoseconds < m_lastSampleNanoseconds)
    {
        return Result<void>::failure(make_game_core_error(*m_assertContext, GameCoreError::InvalidClockSample,
                                                          "Monotonic clock sample moved backwards on resume"));
    }

    m_lastSampleNanoseconds = sampleResult.try_value()->nanoseconds;
    m_isPaused = false;
    return Result<void>::success();
}

bool GameClock::is_initialized() const noexcept
{
    return m_isInitialized;
}

bool GameClock::is_paused() const noexcept
{
    return m_isPaused;
}

std::uint64_t GameClock::next_frame_index() const noexcept
{
    return m_nextFrameIndex;
}

std::int64_t GameClock::simulation_time_nanoseconds() const noexcept
{
    return m_simulationTimeNanoseconds;
}

Result<MonotonicClockSample> GameClock::sample_validated() noexcept
{
    Result<MonotonicClockSample> sampleResult = m_clock->sample(*m_assertContext);
    if (!sampleResult)
    {
        return Result<MonotonicClockSample>::failure(take_error(sampleResult));
    }
    if (sampleResult.try_value()->nanoseconds < 0)
    {
        return Result<MonotonicClockSample>::failure(make_game_core_error(
            *m_assertContext, GameCoreError::InvalidClockSample, "Monotonic clock sample must not be negative"));
    }
    return Result<MonotonicClockSample>::success(std::move(*sampleResult.try_value()));
}

void GameClock::invalidate_after_move() noexcept
{
    m_clock = nullptr;
    m_maxDeltaNanoseconds = 0;
    m_lastSampleNanoseconds = 0;
    m_simulationTimeNanoseconds = 0;
    m_nextFrameIndex = 0;
    m_isInitialized = false;
    m_isPaused = false;
    m_isFrameIndexExhausted = false;
}

Error GameClock::make_state_error(const char *a_summary) const noexcept
{
    return make_game_core_error(*m_assertContext, GameCoreError::InvalidClockState, a_summary);
}

namespace details
{
Result<ClockAdvance> calculate_frame_advance(ClockAdvanceState a_state, std::int64_t a_sampleNanoseconds,
                                             std::int64_t a_maxDeltaNanoseconds,
                                             const AssertContext &a_assertContext) noexcept
{
    if (a_state.isFrameIndexExhausted)
    {
        return Result<ClockAdvance>::failure(
            make_game_core_error(a_assertContext, GameCoreError::ClockOverflow, "Game clock frame index is exhausted"));
    }
    if (a_sampleNanoseconds < a_state.lastSampleNanoseconds)
    {
        return Result<ClockAdvance>::failure(make_game_core_error(a_assertContext, GameCoreError::InvalidClockSample,
                                                                  "Monotonic clock sample moved backwards"));
    }

    const std::int64_t observedDeltaNanoseconds = a_sampleNanoseconds - a_state.lastSampleNanoseconds;
    const bool wasDeltaClamped = !a_state.isPaused && observedDeltaNanoseconds > a_maxDeltaNanoseconds;
    const std::int64_t simulationDeltaNanoseconds =
        a_state.isPaused ? 0 : (wasDeltaClamped ? a_maxDeltaNanoseconds : observedDeltaNanoseconds);
    if (simulationDeltaNanoseconds > std::numeric_limits<std::int64_t>::max() - a_state.simulationTimeNanoseconds)
    {
        return Result<ClockAdvance>::failure(make_game_core_error(a_assertContext, GameCoreError::ClockOverflow,
                                                                  "Game clock simulation time overflowed"));
    }

    const FrameTiming timing = {
        a_state.nextFrameIndex,     observedDeltaNanoseconds,
        simulationDeltaNanoseconds, a_state.simulationTimeNanoseconds + simulationDeltaNanoseconds,
        a_state.isPaused,           wasDeltaClamped,
    };
    a_state.lastSampleNanoseconds = a_sampleNanoseconds;
    a_state.simulationTimeNanoseconds = timing.simulationTimeNanoseconds;
    if (a_state.nextFrameIndex == std::numeric_limits<std::uint64_t>::max())
    {
        a_state.isFrameIndexExhausted = true;
    }
    else
    {
        ++a_state.nextFrameIndex;
    }

    return Result<ClockAdvance>::success(ClockAdvance{timing, a_state});
}
} // namespace details
} // namespace cue::game_core
