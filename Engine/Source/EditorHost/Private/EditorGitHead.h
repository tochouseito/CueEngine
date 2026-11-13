#pragma once

#include <optional>
#include <string>

#include <Foundation/Path.h>

namespace cue
{
class IFileSystem;

/// @brief 最寄りの作業ツリーのブランチ参照、または detached HEAD のコミットを読む
/// FileSystem と Path は呼出中だけ借用する。Checkout 中は Error を返し、Git の情報は変更しない
[[nodiscard]] Result<std::optional<std::string>> read_editor_git_head(IFileSystem &a_files, const Path &a_root);
} // namespace cue
