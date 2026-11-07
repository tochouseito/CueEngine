#pragma once

#include <cstdint>
#include <optional>

#include <Foundation/Path.h>
#include <SceneRender/RenderScene.h>

namespace cue
{
class IFileSystem;

/// @brief 起動時だけ受け取る製品検証用の明示設定。通常起動では無制限・Hardware 描画
struct PlayerValidationOptions final
{
    std::uint64_t maxFrames = 0;
    bool isWarp = false;
    bool isValidateOnly = false;
    std::optional<Path> report;
    bool isRequested = false;
};

/// @brief 計測中は所有値だけを保持し、終了後に一度だけ書き出す
struct PlayerValidationMetrics final
{
    std::uint64_t completedSimulationTicks = 0;
    std::uint64_t completedRenders = 0;
    std::uint64_t meshCount = 0;
    bool hasCamera = false;
    bool isStaticRegistration = false;
    bool hasNormalShutdown = false;
    std::optional<math::float4x4> initialWorld;
    std::optional<math::float4x4> finalWorld;
};

/// @brief Windows 標準の引数分割で、重複・未知・範囲外を拒否して所有値へ変換する
[[nodiscard]] Result<PlayerValidationOptions> read_player_validation_options();
/// @brief 描画前の Snapshot を借り、最初と最後の Mesh 行列を値として記録する
void observe_player_snapshot(PlayerValidationMetrics &a_metrics, const SceneRenderSnapshot &a_snapshot);
/// @brief 明示された検証先にだけ、一度の原子的保存を行う。未指定時は何も出力しない
[[nodiscard]] Result<void> write_player_validation_report(IFileSystem &a_files,
                                                          const PlayerValidationOptions &a_options,
                                                          const PlayerValidationMetrics &a_metrics);
} // namespace cue
