#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/GameCore/UpdateContext.h>

#include <cstdint>

namespace cue
{
class AssertContext;
}

namespace cue::game_core
{
/// @brief 任意のClock Epochからの単調経過時間を整数ナノ秒で保持する値
struct MonotonicClockSample final
{
    std::int64_t nanoseconds = 0;
};

/// @brief PlatformやWall Clockから独立して単調時間を注入する非所有境界
///
/// sampleは同じInstance内で前回値以上を返し、負値を返さない
/// 呼出しと破棄はRuntime SessionのOwner Thread上で行う
class MonotonicClock
{
  public:
    /// @brief 派生Clockを基底Pointerから安全に破棄する
    virtual ~MonotonicClock() noexcept = default;

    /// @brief 現在の単調時間を取得し、取得不能または表現不能なら診断を返す
    [[nodiscard]] virtual Result<MonotonicClockSample> sample(const AssertContext &a_assertContext) noexcept = 0;
};

/// @brief C++ Standard Libraryのsteady_clockをPortable Clock境界へ変換するAdapter
class SteadyMonotonicClock final : public MonotonicClock
{
  public:
    /// @brief 状態を持たないsteady_clock Adapterを生成する
    SteadyMonotonicClock() noexcept = default;
    /// @brief 状態を持たないAdapterを複製する
    SteadyMonotonicClock(const SteadyMonotonicClock &) noexcept = default;
    /// @brief 状態を持たないAdapterを複製代入する
    SteadyMonotonicClock &operator=(const SteadyMonotonicClock &) noexcept = default;
    /// @brief 状態を持たないAdapterを破棄する
    ~SteadyMonotonicClock() noexcept override = default;

    /// @brief steady_clockの現在値を符号付き整数ナノ秒へ検証して変換する
    [[nodiscard]] Result<MonotonicClockSample> sample(const AssertContext &a_assertContext) noexcept override;
};

/// @brief Session-localなFrame Delta、Pause、Simulation Time、Frame Indexを所有するClock
///
/// 注入ClockとAssertContextは非所有であり、本Objectより長く生存させる
/// 全操作は同じRuntime Session Owner Threadから呼び、並行Accessしない
class GameClock final
{
  public:
    /// @brief 有効なDelta上限を持つ未Reset Clockを生成する
    /// @param a_clock GameClockより長く生存し、Session間でMutable状態を共有しないClock
    /// @param a_maxDeltaNanoseconds Simulationへ一Frameで適用できる正の最大Delta
    /// @param a_assertContext GameClockと返却Errorより長く生存する診断Context
    [[nodiscard]] static Result<GameClock> create(MonotonicClock &a_clock, std::int64_t a_maxDeltaNanoseconds,
                                                  const AssertContext &a_assertContext) noexcept;

    /// @brief Session-local Clock状態の複製を禁止する
    GameClock(const GameClock &) = delete;
    /// @brief Session-local Clock状態の複製代入を禁止する
    GameClock &operator=(const GameClock &) = delete;
    /// @brief Clock状態と非所有参照を移動し、移動元を操作不能な未初期化状態にする
    GameClock(GameClock &&a_other) noexcept;
    /// @brief Clock状態と非所有参照を移動代入し、移動元を操作不能な未初期化状態にする
    GameClock &operator=(GameClock &&a_other) noexcept;
    /// @brief 非所有参照を解放せずSession-local状態だけを破棄する
    ~GameClock() noexcept = default;

    /// @brief 現在時刻をFrame基準にしてSimulation TimeとFrame Indexを0へResetする
    /// @details Sampling失敗時は既存Clock状態を変更しない
    [[nodiscard]] Result<void> reset() noexcept;
    /// @brief 次のFrameをSampleし、検証済みUpdate Contextを返す
    /// @details Sampling、逆行、Overflow失敗時はClock状態とFrame Indexを変更しない
    [[nodiscard]] Result<UpdateContext> advance_frame() noexcept;
    /// @brief 初期化済みClockをPauseし、以後のFrameでSimulation Timeを停止する
    [[nodiscard]] Result<void> pause() noexcept;
    /// @brief Pause中の経過時間を捨てる新しい基準をSampleして更新を再開する
    /// @details Sampling失敗時はPaused状態と既存基準を維持する
    [[nodiscard]] Result<void> resume() noexcept;

    /// @brief reset成功後の場合にtrueを返す
    [[nodiscard]] bool is_initialized() const noexcept;
    /// @brief 初期化済みClockがPause中の場合にtrueを返す
    [[nodiscard]] bool is_paused() const noexcept;
    /// @brief 次に成功するFrameへ割り当てるIndexを返す
    [[nodiscard]] std::uint64_t next_frame_index() const noexcept;
    /// @brief Session開始からSimulationへ適用済みの整数ナノ秒を返す
    [[nodiscard]] std::int64_t simulation_time_nanoseconds() const noexcept;

  private:
    /// @brief 検証済み構成と非所有依存から未Reset Clockを構築する
    GameClock(MonotonicClock &a_clock, std::int64_t a_maxDeltaNanoseconds,
              const AssertContext &a_assertContext) noexcept;
    /// @brief Clock Sourceの値を取得して非負値へ検証する
    [[nodiscard]] Result<MonotonicClockSample> sample_validated() noexcept;
    /// @brief 移動元のClock Sourceを切り離して二重消費を防ぐ
    void invalidate_after_move() noexcept;
    /// @brief GameClockの状態違反を診断するErrorを生成する
    [[nodiscard]] Error make_state_error(const char *a_summary) const noexcept;

    MonotonicClock *m_clock;
    const AssertContext *m_assertContext;
    std::int64_t m_maxDeltaNanoseconds;
    std::int64_t m_lastSampleNanoseconds = 0;
    std::int64_t m_simulationTimeNanoseconds = 0;
    std::uint64_t m_nextFrameIndex = 0;
    bool m_isInitialized = false;
    bool m_isPaused = false;
    bool m_isFrameIndexExhausted = false;
};
} // namespace cue::game_core
