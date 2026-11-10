#pragma once

#include <ProjectBuild/BuildTarget.h>

namespace cue
{
class IFileSystem;
class Project;

/// @brief Project ローカルの Game 構成を読む。File が未存在なら Development を返す
/// FileSystem / Project は呼出中だけ借用する。最大 1 KiB と Identity を検証し、失敗時は書込みをしない
/// Editor の Owner Thread で他の設定操作と直列化し、Scene / .cueproj は変更しない
[[nodiscard]] Result<GameBuildConfiguration> load_editor_game_configuration(IFileSystem &a_files,
                                                                            const Project &a_project);

/// @brief Intermediate/Editor/Settings.cueuser に構成を原子的に保存する
/// Owner Thread で直列化する。公開前の失敗では既存 File を保持するが、生成した親 Directory は残り得る
/// 明示した Project のみに保存し、Scene / .cueproj と製品 Build の設定は変更しない
[[nodiscard]] Result<void> save_editor_game_configuration(IFileSystem &a_files, const Project &a_project,
                                                          GameBuildConfiguration a_configuration);
} // namespace cue
