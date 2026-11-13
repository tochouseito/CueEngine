#pragma once

#include <string_view>

#include <Foundation/Path.h>

namespace cue
{
class IFileSystem;
class Project;

/// @brief Portable な Script Class / 登録 Symbol として使える ASCII 識別子だけを許可する
[[nodiscard]] bool is_valid_script_name(std::string_view a_name) noexcept;

/// @brief Source に <Name>.h と <Name>.cpp を新規作成し、対応表を公開して cpp の Path を返す
/// Owner Thread で直列実行し、Project / FileSystem は呼出中だけ借用する
/// Project 内の排他 File を保持し、既存 File は上書きしない。失敗時は未変更の新規 File だけを回収する
/// 旧 v1 / 手書き CMake の Project は移行が必要な Error を返し、既存 Source / Scene を変更しない
[[nodiscard]] Result<Path> create_script_source(IFileSystem &a_files, const Project &a_project,
                                                std::string_view a_className);
} // namespace cue
