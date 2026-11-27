#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Foundation/Path.h>

namespace cue
{
enum class EditorAssetKind
{
    File,
    Scene,
    Script,
    Texture,
    Material,
    ModelSource,
    Model,
};

/// @brief Asset の表示に必要な所有値。Script の Path は代表の cpp を指す
struct EditorAsset final
{
    Path path;
    std::string name;
    EditorAssetKind kind = EditorAssetKind::File;
    std::string scriptTypeId;
    bool isMissing = false;
};

/// @brief FileSystem の列挙結果を UI 専用のツリーにする
struct EditorAssetFolder final
{
    Path path;
    std::string name;
    std::vector<EditorAssetFolder> folders;
    std::vector<EditorAsset> assets;
};

/// @brief Inspector が選択肢に使う Material の永続 ID と表示名
struct EditorMaterialChoice final
{
    std::string assetId;
    std::string name;
};

/// @brief Host が検証した削除対象と影響範囲を Frame 中だけ借用する
struct EditorScriptRemovalView final
{
    std::string_view name;
    std::span<const std::string> files;
    std::span<const std::string> uses;
    bool requiresSceneSave = false;
    bool canDelete = true;
};
} // namespace cue
