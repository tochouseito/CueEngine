#pragma once

#include <functional>
#include <string>

#include <Foundation/Path.h>

namespace cue
{
class IFileSystem;

/// @brief 初期 Scene と Source を一緒に生成する正規 Template
enum class ProjectTemplate
{
    Empty3D,
    RotateCube
};

/// @brief 作成要求を値で所有する。directory は未存在の絶対 Path とし、親は既存 Directory を要求する
struct ProjectCreateDesc final
{
    Path directory;
    std::string name;
    std::string applicationName;
    std::string engineVersion;
    ProjectTemplate projectTemplate = ProjectTemplate::Empty3D;
};

/// @brief 公開済み Project の所有識別値。FileSystem や一時 Staging への借用を含まない
struct ProjectCreation final
{
    Path projectFile;
    std::string projectId;
    std::string name;
    std::string engineVersion;
};

using projectIdGenerator = std::function<Result<std::string>()>;

/// @brief Template を検証して所有 Staging を生成し、上書き不可 Rename の成功後だけ結果を公開する
/// FileSystem と UUID Callback は呼出中だけ借用する。同じ保存先の外部変更を並行させず、再入しない
/// 失敗では既存の保存先を変更せず、自分が作った既知 File だけを回収する。不明 File は残して診断する
[[nodiscard]] Result<ProjectCreation> create_project(IFileSystem &a_files, const Path &a_templates,
                                                     const ProjectCreateDesc &a_desc,
                                                     const projectIdGenerator &a_generateId);
} // namespace cue
