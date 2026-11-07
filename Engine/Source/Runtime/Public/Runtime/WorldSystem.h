#pragma once

#include <chrono>
#include <cstdint>

#include <Foundation/Result.h>

namespace cue
{
class World;

/// @brief World の更新入力。時間の計測・FPS 待機は呼出側が行う
struct WorldUpdateContext final
{
    std::chrono::nanoseconds deltaTime{};
    std::uint64_t tick = 0;
};

/// @brief World が一意所有する System。Callback は World の Owner Thread で直列に実行する
/// World の借用は Callback 中だけ有効。構造追加は defer を使い、値の変更は直接行える
/// Callback の失敗後は World を再更新せず shutdown する。部分初期化の回収も shutdown が担う
class WorldSystem
{
  public:
    /// @brief 所有する派生 System を破棄する
    virtual ~WorldSystem() = default;
    /// @brief 更新前に一度だけ初期化する
    [[nodiscard]] virtual Result<void> initialize(World &)
    {
        return Result<void>::success();
    }
    /// @brief 時間入力を使って World の状態を更新する
    [[nodiscard]] virtual Result<void> update(World &, const WorldUpdateContext &) = 0;
    /// @brief 初期化を開始した System を逆順に回収する。失敗しても再呼出しは行わない
    [[nodiscard]] virtual Result<void> shutdown(World &)
    {
        return Result<void>::success();
    }
};
} // namespace cue
