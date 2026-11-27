#pragma once

#include <functional>
#include <optional>
#include <string>

#include <Foundation/Path.h>
#include <Foundation/Result.h>

namespace cue
{
class IFileSystem;

/// @brief 保存済みのモデルと割り当てに使う Material の永続 ID
struct ImportedModel final
{
    std::string meshId;
    std::optional<std::string> materialId;
};

/// @brief Content 内の OBJ とその MTL / PNG を読み、単一 Material のモデルとして登録する
/// 同じ Content の書込みは呼出側で直列化する。FileSystem と ID Generator は呼出中だけ借りる
/// 同名の .cuemodel を再 Import すると Mesh と Material の永続 ID を維持する
[[nodiscard]] Result<ImportedModel> import_obj_model(IFileSystem &a_files, const Path &a_contentRoot,
                                                     const Path &a_source,
                                                     const std::function<Result<std::string>()> &a_generateId);
} // namespace cue
