#pragma once

#include <chrono>

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
} // namespace cue
