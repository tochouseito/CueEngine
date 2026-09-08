#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/GameCore/UpdateContext.h>

#include <cstdint>

namespace cue
{
class AssertContext;
}

namespace cue::game_core::details
{
/// @brief 一Frameの計算前後でGameClockが保持する整数状態
struct ClockAdvanceState final
{
    std::int64_t lastSampleNanoseconds = 0;
    std::int64_t simulationTimeNanoseconds = 0;
    std::uint64_t nextFrameIndex = 0;
    bool isPaused = false;
    bool isFrameIndexExhausted = false;
};

/// @brief 検証済みFrame TimingとCommit可能な次状態
struct ClockAdvance final
{
    FrameTiming timing;
    ClockAdvanceState state;
};

/// @brief 現在状態とSampleからOverflow検証済みのFrame遷移を副作用なしで計算する
[[nodiscard]] Result<ClockAdvance> calculate_frame_advance(ClockAdvanceState a_state, std::int64_t a_sampleNanoseconds,
                                                           std::int64_t a_maxDeltaNanoseconds,
                                                           const AssertContext &a_assertContext) noexcept;
} // namespace cue::game_core::details
