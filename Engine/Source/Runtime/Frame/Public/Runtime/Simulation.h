#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>

#include <Foundation/Result.h>

namespace cue
{
/// @brief FPS と独立したゲーム更新設定。ユーザー性能設定の保存対象にしない
struct SimulationConfig final
{
    // 0 は可変周期。固定周期ではゲームロジックを SimulationCallback に登録する
    std::uint32_t frequencyHz = 0;
    std::uint32_t maxCatchUpSteps = 4;
    std::chrono::nanoseconds maxElapsed = std::chrono::milliseconds(250);

    /// @brief 開発者が要求した更新方針を値で比較する
    bool operator==(const SimulationConfig &) const = default;
};

/// @brief Render Frame ID と独立したゲーム更新の一回分
struct SimulationStep final
{
    std::uint64_t tick = 0;
    std::chrono::duration<double> deltaTime{};
    std::chrono::duration<double> simulationTime{};
};

/// @brief 一つの描画 Snapshot に属する不変の補間・過負荷情報
struct FrameSimulationInfo final
{
    std::uint64_t frameIndex = 0;
    std::uint64_t previousTick = 0;
    std::uint64_t currentTick = 0;
    std::uint32_t updateSteps = 0;
    double interpolationAlpha = 1.0;
    std::chrono::nanoseconds frameElapsed{};
    std::chrono::nanoseconds droppedElapsed{};
    bool isFixed = false;
    bool wasCanceled = false;
};

using simulationCallback = std::function<Result<void>(const SimulationStep &, std::stop_token)>;
using frameSnapshotCallback = std::function<Result<void>(const FrameSimulationInfo &, std::stop_token)>;

/// @brief 固定更新の範囲と過負荷上限を実体変更前に検証する
[[nodiscard]] Result<void> validate_simulation_config(const SimulationConfig &a_config);

/// @brief 更新 Thread が所有する時間蓄積と Tick の正本。外部から操作を直列化する
class SimulationStepper final
{
  public:
    /// @brief 固定容量の時間状態を作る。実行前に設定を検証する
    explicit SimulationStepper(SimulationConfig a_config = {}) noexcept;
    /// @brief Frame の採取時刻から更新を進める。Callback の失敗は部分 World を復元せず停止扱い
    [[nodiscard]] Result<FrameSimulationInfo> advance(std::uint64_t a_frame,
                                                      std::chrono::steady_clock::time_point a_sample,
                                                      const simulationCallback &a_update, std::stop_token a_token = {});
    /// @brief 排出・Worker join 後に方針を変更する。Tick と Simulation 時刻は維持し残り時間は捨てる
    [[nodiscard]] Result<void> configure(SimulationConfig a_config);
    /// @brief 同じ方針の再開時に停止期間だけを除外する。余り時間と Tick を維持する
    void reset_sample_time() noexcept;

  private:
    SimulationConfig m_config;
    std::optional<std::chrono::steady_clock::time_point> m_previousSample;
    // Hz * ns を蓄積し、1 Tick = 1e9 Units として 60 Hz 等の丸め誤差を累積させない
    std::uint64_t m_phaseUnits = 0;
    std::uint64_t m_tick = 0;
    std::chrono::duration<double> m_simulationTime{};
    // Callback 失敗後の部分 World を同じ Tick で再実行しない
    bool m_isFailed = false;
};
} // namespace cue
