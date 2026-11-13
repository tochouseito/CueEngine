#include "EditorGitHead.h"

#include <algorithm>
#include <span>
#include <string_view>
#include <utility>

#include <Platform/FileSystem.h>

namespace cue
{
namespace
{
/// @brief Git の管理ファイルから行末だけを取り除く
std::string_view git_line(std::string_view a_text)
{
    if (a_text.ends_with('\n'))
        a_text.remove_suffix(1);
    if (a_text.ends_with('\r'))
        a_text.remove_suffix(1);
    return a_text;
}

/// @brief ローカルの Git 管理ファイルを上限付きで読む
Result<std::string> read_git_file(IFileSystem &a_files, const Path &a_path)
{
    auto info = a_files.stat(a_path);
    if (!info.has_value())
        return Result<std::string>::failure(*info.try_error());
    if (!*info.try_value() || (**info.try_value()).type != FileType::Regular)
        return Result<std::string>::failure({ErrorCategory::InvalidState, "EditorGit.metadata_file"});
    // Git が HEAD を置き換える操作を、Windows の File 共有モードで妨げない
    auto opened = a_files.open(a_path, {FileAccess::Read, FileCreation::OpenExisting, true, true, true});
    if (!opened.has_value())
        return Result<std::string>::failure(*opened.try_error());
    auto file = opened.take_value();
    auto size = file->size();
    if (!size.has_value())
        return Result<std::string>::failure(*size.try_error());
    if (*size.try_value() > 64 * 1024)
        return Result<std::string>::failure({ErrorCategory::InvalidArgument, "EditorGit.metadata_size"});
    std::string text(static_cast<std::size_t>(*size.try_value()), '\0');
    auto remaining = std::as_writable_bytes(std::span(text));
    while (!remaining.empty())
    {
        auto count = file->read(remaining);
        if (!count.has_value())
            return Result<std::string>::failure(*count.try_error());
        if (*count.try_value() == 0 || *count.try_value() > remaining.size())
            return Result<std::string>::failure({ErrorCategory::InvalidState, "EditorGit.metadata_changed"});
        remaining = remaining.subspan(*count.try_value());
    }
    auto closed = file->close();
    return closed.has_value() ? Result<std::string>::success(std::move(text))
                              : Result<std::string>::failure(*closed.try_error());
}

/// @brief 通常の作業ツリーと gitfile を使う作業ツリーから HEAD の配置を解決する
Result<Path> git_directory(IFileSystem &a_files, const Path &a_entry, FileType a_type)
{
    if (a_type == FileType::Directory)
        return Result<Path>::success(a_entry);
    auto file = read_git_file(a_files, a_entry);
    if (!file.has_value())
        return Result<Path>::failure(*file.try_error());
    const auto line = git_line(*file.try_value());
    constexpr std::string_view k_prefix = "gitdir: ";
    if (!line.starts_with(k_prefix) || line.find_first_of("\r\n") != std::string_view::npos)
        return Result<Path>::failure({ErrorCategory::InvalidState, "EditorGit.gitdir"});
    return a_entry.parent().join(line.substr(k_prefix.size()));
}
} // namespace

Result<std::optional<std::string>> read_editor_git_head(IFileSystem &a_files, const Path &a_root)
{
    using headResult = Result<std::optional<std::string>>;
    try
    {
        for (auto root = a_root;;)
        {
            auto entry = root.join(".git");
            if (!entry.has_value())
                return headResult::failure(*entry.try_error());
            auto info = a_files.stat(*entry.try_value());
            if (!info.has_value())
                return headResult::failure(*info.try_error());
            if (*info.try_value())
            {
                auto directory = git_directory(a_files, *entry.try_value(), (**info.try_value()).type);
                if (!directory.has_value())
                    return headResult::failure(*directory.try_error());
                auto head = directory.try_value()->join("HEAD");
                auto lock = directory.try_value()->join("index.lock");
                if (!head.has_value() || !lock.has_value())
                    return headResult::failure({ErrorCategory::InvalidState, "EditorGit.metadata_path"});
                auto text = read_git_file(a_files, *head.try_value());
                if (!text.has_value())
                    return headResult::failure(*text.try_error());
                // Checkout が作業ファイルを更新している間は再読込も保存も開始しない
                auto locked = a_files.exists(*lock.try_value());
                if (!locked.has_value())
                    return headResult::failure(*locked.try_error());
                if (*locked.try_value())
                    return headResult::failure({ErrorCategory::InvalidState, "EditorGit.index_locked"});
                const auto line = git_line(*text.try_value());
                constexpr std::string_view k_branchPrefix = "ref: refs/heads/";
                const bool isBranch = line.starts_with(k_branchPrefix) && line.size() > k_branchPrefix.size() &&
                                      is_valid_utf8_text(line) && line.find_first_of("\r\n") == std::string_view::npos;
                const bool isDetached = (line.size() == 40 || line.size() == 64) &&
                                        std::all_of(line.begin(), line.end(), [](char a_char)
                                                    { return (a_char >= '0' && a_char <= '9') ||
                                                             (a_char >= 'a' && a_char <= 'f'); });
                if (!isBranch && !isDetached)
                    return headResult::failure({ErrorCategory::InvalidState, "EditorGit.head"});
                return headResult::success(std::string(line));
            }
            auto parent = root.parent();
            if (parent.utf8() == root.utf8())
                return headResult::success(std::nullopt);
            root = std::move(parent);
        }
    }
    catch (...)
    {
        return headResult::failure({ErrorCategory::PlatformFailure, "EditorGit.read"});
    }
}
} // namespace cue
