#pragma once

#include <string_view>

#include <Foundation/Path.h>
#include <Foundation/Result.h>

namespace cue
{
class IFileSystem;
class Project;
/// @brief Source/ScriptSources.cueide から型 ID の Source を解決する。FileSystem は呼出中だけ借用する
/// 不明 ID・絶対 Path・Traversal・Link・Source 外を拒否し、File を作成・変更しない
[[nodiscard]] Result<Path> resolve_script_source(IFileSystem &a_files, const Project &a_project,
                                                 std::string_view a_typeId);
} // namespace cue
