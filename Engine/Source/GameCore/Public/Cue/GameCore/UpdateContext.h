#pragma once

#include <cstdint>

namespace cue::game_core
{
class GameClock;

/// @brief 一Frameで観測した単調時間とSimulationへ適用した時間を保持する値
struct FrameTiming final
{
    std::uint64_t frameIndex = 0;
    std::int64_t observedDeltaNanoseconds = 0;
    std::int64_t simulationDeltaNanoseconds = 0;
    std::int64_t simulationTimeNanoseconds = 0;
    bool isPaused = false;
    bool wasDeltaClamped = false;
};

/// @brief Runtime Systemへ一Frame中不変として渡すPortableな更新値
///
/// Contextは値を自己所有し、Clock、Renderer、Audio、Native APIへの参照を保持しない
class UpdateContext final
{
  public:
    /// @brief Frame更新値を独立して複製する
    UpdateContext(const UpdateContext &) noexcept = default;
    /// @brief Frame更新値を独立して複製代入する
    UpdateContext &operator=(const UpdateContext &) noexcept = default;
    /// @brief Frame更新値を移動する
    UpdateContext(UpdateContext &&) noexcept = default;
    /// @brief Frame更新値を移動代入する
    UpdateContext &operator=(UpdateContext &&) noexcept = default;
    /// @brief 値だけを保持するContextを破棄する
    ~UpdateContext() noexcept = default;

    /// @brief 現在Frameの検証済みTimingを返す
    [[nodiscard]] const FrameTiming &timing() const noexcept;
    /// @brief Simulationへ適用するDeltaを秒単位へ変換して返す
    [[nodiscard]] double delta_seconds() const noexcept;
    /// @brief Session開始からのSimulation Timeを秒単位へ変換して返す
    [[nodiscard]] double simulation_seconds() const noexcept;

  private:
    friend class GameClock;

    /// @brief GameClockだけが検証済みFrame Timingから更新値を生成する
    explicit UpdateContext(FrameTiming a_timing) noexcept;

    FrameTiming m_timing;
};
} // namespace cue::game_core
