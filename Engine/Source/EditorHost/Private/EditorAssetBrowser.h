#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <EditorUi/EditorAssets.h>
#include <Foundation/Result.h>

namespace cue
{
class IFileSystem;
class Project;
class SceneDocument;

/// @brief 削除の確定・復元に使う所有 Snapshot。空の置換値は File の削除を表す
struct EditorAssetFileChange final
{
    Path path;
    std::string original;
    std::optional<std::string> replacement;
};

/// @brief Script 参照の解決と更新前の File 内容を一つの確認単位で保持する
struct EditorScriptRemoval final
{
    std::string typeId;
    std::string name;
    std::vector<std::string> files;
    std::vector<std::string> uses;
    std::vector<EditorAssetFileChange> changes;
    std::optional<Path> activeScene;
    bool requiresSceneSave = false;
};

/// @brief Project の公開 Root だけを一覧にする。FileSystem は Owner Thread の呼出中だけ借用する
[[nodiscard]] Result<std::vector<EditorAssetFolder>> scan_editor_assets(IFileSystem &a_files, const Project &a_project);

/// @brief 更新はせず、登録・Source・Scene の削除内容を確認用に作る
[[nodiscard]] Result<EditorScriptRemoval> inspect_script_removal(IFileSystem &a_files, const Project &a_project,
                                                                 const SceneDocument &a_document,
                                                                 std::string_view a_typeId);

/// @brief 確認時 Snapshot と再照合して適用する。通常の失敗時は適用済みの変更を復元する
/// Owner Thread 専用。a_reload は File 公開後に呼び、失敗時は編集正本を変更してはならない
[[nodiscard]] Result<void> execute_script_removal(IFileSystem &a_files, const Project &a_project,
                                                  const SceneDocument &a_document, const EditorScriptRemoval &a_removal,
                                                  const std::function<Result<void>()> &a_reload);
} // namespace cue
