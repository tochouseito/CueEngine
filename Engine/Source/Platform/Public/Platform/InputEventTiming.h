#pragma once

#include <chrono>
#include <cstdint>

namespace cue
{
/// @brief Window が入力 Message を受け取った時刻と Window 内の連番を所有する
///
/// OS Queue への投入や物理操作の時刻ではない。Window Owner が Frame Snapshot へ移して使う
struct InputEventTiming final
{
    // Window 内で単調増加する入力 ID。0 は未割当て
    std::uint64_t eventId = 0;
    // OS の Message が Engine に到達した共通 Clock の時刻
    std::chrono::steady_clock::time_point received;
};
} // namespace cue
