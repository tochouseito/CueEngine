#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <Profiler/TimingSamples.h>
#include <RHI/Presentation.h>
#include <RHI/Queue.h>

namespace cue
{
/// @brief GPU 完了後に確定した Pass の経過時間。未対応 Queue は未計測として保持する
struct GpuPassTiming final
{
    std::string name;
    QueueType queue = QueueType::Graphics;
    std::chrono::nanoseconds duration{};
    // GPU 完了後に有効な Timestamp が回収できたか
    bool isAvailable = false;
    TimingStatistics statistics;
};

/// @brief Graph の CPU 時間と完了済み GPU Pass 時間を所有 Snapshot として返す
struct MainFrameGraphPerformance final
{
    // CPU が Graph の Command を記録した時間
    TimingStatistics record;
    // 同じ GPU 資源枠を再利用するまでの待機時間
    TimingStatistics frameWait;
    TimingStatistics present;
    std::vector<GpuPassTiming> gpuPasses;
    // GPU 完了と計測結果の回収が済んだ Frame の累計
    std::uint64_t completedGpuFrames = 0;
    // GPU 未完了 Frame 上限による待機時間
    TimingStatistics gpuQueueWait;
    // Waitable の表示準備待機時間
    TimingStatistics presentationReadyWait;
    TimingStatistics presentInterval;
    TimingStatistics displayInterval;
    // 観測した未完了 GPU Frame 数の最大値
    std::uint32_t peakUnfinishedGpuFrames = 0;
    std::uint32_t unfinishedGpuFrames = 0;
    PresentationTiming presentation;
    // Present API 本体の開始から復帰までの時間
    TimingStatistics presentApi;
    // Present 復帰後に表示統計を取得した時間
    TimingStatistics presentationStatistics;
    // Main が Frame 採用の準備確認に使った全時間
    TimingStatistics admissionWait;
    TimingStatistics mainGpuWait;
    TimingStatistics mainPresentationWait;
    TimingStatistics mainResourceWait;
};
} // namespace cue
