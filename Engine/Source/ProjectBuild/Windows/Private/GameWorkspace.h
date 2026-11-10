#pragma once

#include <string_view>

#include <Foundation/Path.h>
#include <Foundation/Result.h>

namespace cue
{
class IFileSystem;
class Project;

namespace detail
{
/// @brief 独立ゲームの入口とローカル SDK Preset を用意する。呼出中だけ入力を借用し、Owner Thread で直列化する
/// 既存の共有設定は保持する。管理外の UserPresets は変更せず失敗し、保存途中の失敗は新規 File だけを回収する
[[nodiscard]] Result<void> prepare_game_workspace(IFileSystem &a_files, const Project &a_project,
                                                  const Path &a_engineRoot, const Path &a_ideTree,
                                                  const Path &a_appTree, std::string_view a_platform,
                                                  std::string_view a_architecture);
} // namespace detail
} // namespace cue
