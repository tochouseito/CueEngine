#pragma once

#include <optional>

#include <Foundation/Path.h>
#include <ScriptPlay/PlayClient.h>

namespace cue
{
class IFileSystem;
class Project;

/// @brief Editor の Play 接続先を明示指定または検証済み Project 成果物から解決する
/// Game DLL を読み込まない。FileSystem / Project は呼出中だけ借用し、所有 Path を返す
/// 明示指定は両方必須。未指定なら現在構成の Manifest を検証し、既定の別 Project へは戻らない
[[nodiscard]] Result<PlayClientConfig> resolve_editor_play_config(IFileSystem &a_files, const Project *a_project,
                                                                  const std::optional<Path> &a_executable,
                                                                  const std::optional<Path> &a_module);
} // namespace cue
