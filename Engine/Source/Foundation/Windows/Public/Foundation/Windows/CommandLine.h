#pragma once

#include <span>
#include <string>
#include <string_view>

#include <Foundation/Result.h>

namespace cue
{
/// @brief CreateProcessW に渡す UTF-16 Command Line を組み立てる
/// 実行 File 名と UTF-8 引数を呼出中だけ借用する。埋め込み NUL と不正 UTF-8 は拒否する
[[nodiscard]] Result<std::wstring> make_windows_command_line(std::wstring_view a_executable,
                                                             std::span<const std::string> a_arguments = {});
} // namespace cue
