#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Foundation/Path.h>
#include <Foundation/Result.h>

namespace cue
{
class IFileSystem;
class Project;
/// @brief 対応表が宣言する型と Source の絶対 Path。Source の欠落時も登録は保持する
struct ScriptSourceEntry final
{
    std::string typeId;
    Path source;
};

/// @brief Source Map の原文と検証済みの対応表を所有する
struct ScriptSourceMap final
{
    Path file;
    std::string text;
    std::uint32_t version = 0;
    std::vector<ScriptSourceEntry> entries;
};

/// @brief Source Map を検証して返す。FileSystem は同期呼出中のみ借用し、書き込みは行わない
[[nodiscard]] Result<ScriptSourceMap> load_script_source_map(IFileSystem &a_files, const Project &a_project);

/// @brief Source/ScriptSources.cueide から型 ID の Source を解決する。FileSystem は呼出中だけ借用する
/// 不明 ID・絶対 Path・Traversal・Link・Source 外を拒否し、File を作成・変更しない
[[nodiscard]] Result<Path> resolve_script_source(IFileSystem &a_files, const Project &a_project,
                                                 std::string_view a_typeId);
} // namespace cue
