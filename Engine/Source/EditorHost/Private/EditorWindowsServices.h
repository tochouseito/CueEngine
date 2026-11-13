#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <Foundation/Path.h>
#include <Foundation/Result.h>

namespace cue
{
class Window;
class IFileSystem;
enum class EditorLanguage;

/// @brief 非零の小文字 UUID を新規 Object ID として生成する。任意 Thread で呼べる
[[nodiscard]] Result<std::string> create_editor_object_id();

/// @brief Owner Window 付き Scene 選択 Dialog を表示し、キャンセル時は空の成功を返す
/// Window 生成 Thread 専用。Path は呼出中だけ借用し、返却 Path の Project 内検証は呼出側が行う
/// COM 初期化と解放はこの呼出の範囲で釣り合わせ、異なる Apartment で初期化済みなら失敗する
[[nodiscard]] Result<std::optional<Path>> choose_scene_file(Window &a_window, bool a_isSave, const Path *a_initialFile,
                                                            const Path *a_contentRoot, EditorLanguage a_language);

/// @brief Window 生成 Thread で Project Folder を選び、キャンセルは空の成功値にする
/// 初期 Path と Window は呼出中だけ借用し、Filesystem 上の選択 Path を所有値として返す
[[nodiscard]] Result<std::optional<Path>> choose_project_folder(Window &a_window, const Path *a_initialFolder,
                                                                EditorLanguage a_language);

/// @brief Folder 直下の唯一の通常 .cueproj File を返す。空・複数・Link は推測で選択せず拒否する
/// FileSystem と Path は呼出中だけ借用し、失敗時に Filesystem を変更しない
[[nodiscard]] Result<Path> find_project_file(IFileSystem &a_files, const Path &a_folder);

/// @brief Window 生成 Thread で選択 Project の表示名を Title に反映する。空名は未選択の Title とする
/// 名前は呼出中だけ借用する。失敗は Window の表示更新だけを表し、Project 所有状態は変更しない
[[nodiscard]] Result<void> set_editor_project_title(Window &a_window, std::string_view a_projectName);

/// @brief Window 生成 Thread 上で Close Message を予約し、通常の Host 終了経路へ渡す
[[nodiscard]] Result<void> request_editor_close(Window &a_window);
} // namespace cue
