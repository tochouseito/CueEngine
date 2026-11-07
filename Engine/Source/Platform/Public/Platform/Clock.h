#pragma once

#include <chrono>
#include <memory>

namespace cue
{
/// @brief Frame制御に使う単調時計を公開する
///
/// Hostが所有し、利用側より長く生存させる。nowは全Threadから呼出可能で再入可能
/// 実装は時刻を戻さず、失敗を公開しない
class Clock
{
public:
    /// @brief 時計の資源を解放する
    virtual ~Clock() = default;

    /// @brief 単調時計の現在時刻を返す
    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const noexcept = 0;
};

/// @brief Process の終了まで有効な既定の単調時計を借用する
/// 全 Thread から再入可能。時計の差し替えは行わず、独自時計は利用先へ明示的に注入する
[[nodiscard]] Clock &get_default_clock() noexcept;

/// @brief 既定時計と同じ時間軸の単調時計を一意所有する
/// 利用者を停止してから破棄する。生成時のメモリ確保失敗は呼出側で処理する
[[nodiscard]] std::unique_ptr<Clock> create_clock();
} // namespace cue
