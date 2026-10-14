#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace cue
{
/// @brief Native 型を含めず表示設定を所有する
struct PresentationConfig final
{
    std::uint32_t bufferCount = 2;
    bool isVSyncEnabled = false;
    bool isTearingAllowed = false;
    std::array<float, 4> clearColor{0.2f, 0.4f, 0.6f, 1.0f};
    bool isWaitableEnabled = true;
    std::uint32_t maxQueuedFrames = 1;

    /// @brief 表示設定を値で比較する
    bool operator==(const PresentationConfig &) const = default;
};

/// @brief 成功した Present と表示統計を区別して保持する
struct PresentationTiming final
{
    std::uint64_t presentCount = 0;
    std::uint64_t displayedPresentCount = 0;
    std::chrono::steady_clock::time_point lastPresent;
    std::chrono::steady_clock::time_point lastDisplay;
    std::chrono::nanoseconds refreshInterval{};
    bool hasDisplayTiming = false;
    // SyncQPCTime は表示 Frame 自体の時刻ではなく、この Refresh の QPC 標本
    std::chrono::steady_clock::time_point lastRefresh;
    bool hasRefreshTiming = false;
    bool isDisplayTimeEstimated = false;
    std::uint64_t statisticsEpoch = 0;
};
} // namespace cue
