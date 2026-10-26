#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace cue
{
/// @brief Native 型を含めず表示設定を所有する
struct PresentationConfig final
{
    // SwapChain が所有する Back Buffer 数。CPU Snapshot の容量とは別の設定
    std::uint32_t bufferCount = 2;
    // Present 時に垂直同期を要求するか
    bool isVSyncEnabled = false;
    // 対応環境で Tearing を許可するか。VSync 有効時は併用しない
    bool isTearingAllowed = false;
    std::array<float, 4> clearColor{0.2f, 0.4f, 0.6f, 1.0f};
    bool isWaitableEnabled = true;
    // Waitable に設定する表示側 Queue の上限
    std::uint32_t maxQueuedFrames = 1;

    /// @brief 表示設定を値で比較する
    bool operator==(const PresentationConfig &) const = default;
};

/// @brief 成功した Present と表示統計を区別して保持する
struct PresentationTiming final
{
    // 成功した Present を識別する累計値
    std::uint64_t presentCount = 0;
    std::uint64_t displayedPresentCount = 0;
    // 最後に成功した Present の共通 Clock 上の時刻
    std::chrono::steady_clock::time_point lastPresent;
    std::chrono::steady_clock::time_point lastDisplay;
    // 有効な表示統計から求めた Refresh の周期
    std::chrono::nanoseconds refreshInterval{};
    bool hasDisplayTiming = false;
    // SyncQPCTime は表示 Frame 自体の時刻ではなく、この Refresh の QPC 標本
    std::chrono::steady_clock::time_point lastRefresh;
    bool hasRefreshTiming = false;
    // lastDisplay が Native の直接値ではなく推定値か
    bool isDisplayTimeEstimated = false;
    // Monitor 変更や統計の不連続で更新する世代
    std::uint64_t statisticsEpoch = 0;
    // 最後に成功した Native Present の呼出前後。表示や GPU 完了の時刻ではない
    std::chrono::steady_clock::time_point presentStarted;
    std::chrono::steady_clock::time_point presentReturned;
    std::chrono::nanoseconds statisticsDuration{};
};
} // namespace cue
