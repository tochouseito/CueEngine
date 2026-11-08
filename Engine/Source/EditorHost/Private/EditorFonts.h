#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <Foundation/Result.h>

struct ImGuiIO;

namespace cue
{
class IFileSystem;

/// @brief Atlas が借用する TTF を所有する。ImGui Context の破棄が終わるまで生存させる
struct EditorFonts final
{
    std::array<std::vector<std::byte>, 4> data;
};

/// @brief 指定 Directory、未指定なら実行 File 隣から Font を読む。Context の排他取得前に呼ぶ
[[nodiscard]] Result<EditorFonts> read_editor_fonts(IFileSystem &a_files, const std::string &a_directory);

/// @brief UI / 日本語 / Icon とコード用 Font を登録する。a_fonts の TTF を停止まで借用する
[[nodiscard]] Result<void> initialize_editor_fonts(ImGuiIO &a_io, EditorFonts &a_fonts, float a_uiSize, float a_codeSize);
} // namespace cue
