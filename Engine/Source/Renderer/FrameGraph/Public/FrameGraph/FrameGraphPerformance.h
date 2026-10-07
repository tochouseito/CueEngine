#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <Foundation/TimingSamples.h>
#include <RHI/Queue.h>

namespace cue
{
/// @brief GPU 完了後に確定した Pass の経過時間。未対応 Queue は未計測として保持する
struct GpuPassTiming final
{
    std::string name;
    QueueType queue = QueueType::Graphics;
    std::chrono::nanoseconds duration{};
    bool isAvailable = false;
    TimingStatistics statistics;
};

/// @brief Graph の CPU 時間と完了済み GPU Pass 時間を所有 Snapshot として返す
struct MainFrameGraphPerformance final
{
    TimingStatistics record;
    TimingStatistics frameWait;
    TimingStatistics present;
    std::vector<GpuPassTiming> gpuPasses;
    std::uint64_t completedGpuFrames = 0;
};
} // namespace cue
