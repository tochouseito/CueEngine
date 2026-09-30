#pragma once

#include <chrono>
#include <cstdint>
#include <stop_token>

namespace cue
{
enum class WaitStatus
{
    Notified,
    TimedOut,
    Stopped,
};

/// @brief 通知世代を使って起床の取りこぼしを防ぐ待機点を公開する
///
/// Hostが一意所有し、待機中のThreadをjoinしてから破棄する。全操作はThread-safe
/// generation取得後に状態を確認し、変化しなければwait_for_changeを呼ぶ
class Waiter
{
public:
    /// @brief 待機点の資源を解放する
    virtual ~Waiter() = default;

    /// @brief 現在の通知世代を返す
    [[nodiscard]] virtual std::uint64_t generation() const noexcept = 0;

    /// @brief 通知世代を進めて待機中のThreadを起こす
    virtual void notify_all() noexcept = 0;

    /// @brief 世代変更、時間切れ、停止要求のいずれかまで待つ
    ///
    /// a_observedGenerationより新しい通知が既にあれば直ちにNotifiedを返す
    /// a_durationが負なら待機せずTimedOutを返す。停止要求を優先する
    [[nodiscard]] virtual WaitStatus wait_for_change(std::uint64_t a_observedGeneration,
                                                      std::chrono::nanoseconds a_duration,
                                                      std::stop_token a_stopToken) noexcept = 0;

    /// @brief 通知に左右されず指定時間または停止要求まで待つ
    ///
    /// OS Schedulerのため待機時間の厳密な長さは保証しない
    [[nodiscard]] virtual WaitStatus sleep_for(std::chrono::nanoseconds a_duration,
                                               std::stop_token a_stopToken) noexcept = 0;
};
} // namespace cue
