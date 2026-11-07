#pragma once

#include <span>
#include <string_view>

#include <Foundation/Path.h>

namespace cue
{
/// @brief 起動時の Project 指定を所有する。空 Path は未選択、相対 Path は読込時に一度だけ解決する
struct EditorLaunchOptions final
{
    Path projectFile;
};

/// @brief 実行 File 名を除いた引数から任意の --project <path> を一組だけ受け取る
///
/// 入力は呼出中だけ借用し、結果は所有値。未知引数・重複・空指定は部分結果を返さず拒否する
/// 独立した入力で同時呼出し可能。Allocation 失敗も Result にする
[[nodiscard]] Result<EditorLaunchOptions> parse_editor_arguments(std::span<const std::string_view> a_arguments);
} // namespace cue
