#pragma once

#include <ProjectBuild/BuildTarget.h>

namespace cue::detail
{
/// @brief 実装済み Windows Platform の不変な登録情報を返す
[[nodiscard]] const GameBuildPlatform &windows_game_build_platform() noexcept;
/// @brief 実行 Binary の Native 対象を Platform 実装側で判定し、未対応なら明示失敗する
[[nodiscard]] Result<GameBuildTarget> native_game_build_target(GameBuildConfiguration a_configuration);
} // namespace cue::detail
