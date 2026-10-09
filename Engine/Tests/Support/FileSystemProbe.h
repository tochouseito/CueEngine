#pragma once

#include <functional>
#include <string>

#include <Platform/FileSystem.h>

namespace cue::tests
{
/// @brief 実 FileSystem を借用し、利用側の失敗経路とアクセスを検証する
struct FileSystemProbe : public IFileSystem
{
  public:
    /// @brief Test Fixture が所有する実装を借用する
    explicit FileSystemProbe(IFileSystem &a_files) : files(a_files)
    {
    }
    /// @brief 配置 Path の取得失敗を注入できる
    Result<Path> executable_directory() override
    {
        return failDirectory ? Result<Path>::failure({ErrorCategory::PlatformFailure, "Probe.executable_directory"})
                             : files.executable_directory();
    }
    /// @brief OS 保存先の取得失敗を注入できる
    Result<Path> local_data_directory() override
    {
        return failDirectory ? Result<Path>::failure({ErrorCategory::PlatformFailure, "Probe.local_data_directory"})
                             : files.local_data_directory();
    }
    /// @brief 問い合わせを実装へ渡す
    Result<std::optional<FileInfo>> stat(const Path &a_path) override
    {
        return files.stat(a_path);
    }
    /// @brief Directory 作成失敗を注入する
    Result<void> create_directories(const Path &a_path) override
    {
        return failCreate ? Result<void>::failure({ErrorCategory::PlatformFailure, "Probe.create_directories"})
                          : files.create_directories(a_path);
    }
    /// @brief 一覧取得を実装へ渡す
    Result<std::vector<Path>> list_directory(const Path &a_path) override
    {
        return files.list_directory(a_path);
    }
    /// @brief 単一要素の削除を実装へ渡す
    Result<bool> remove(const Path &a_path) override
    {
        return files.remove(a_path);
    }
    /// @brief 非置換の移動を実装へ渡す
    Result<void> rename(const Path &a_from, const Path &a_to) override
    {
        return files.rename(a_from, a_to);
    }
    /// @brief コピーを実装へ渡す
    Result<void> copy_file(const Path &a_from, const Path &a_to, bool a_overwrite) override
    {
        return files.copy_file(a_from, a_to, a_overwrite);
    }
    /// @brief Open を観測し、特定要素または全読込みの失敗を注入する
    Result<std::unique_ptr<IFile>> open(const Path &a_path, const FileOpenDesc &a_desc = {}) override
    {
        if (onOpen)
        {
            onOpen(a_path);
        }
        if (failRead || (!failedFilename.empty() && a_path.filename() == failedFilename))
        {
            return Result<std::unique_ptr<IFile>>::failure({ErrorCategory::PlatformFailure, "Probe.open", 5});
        }
        return files.open(a_path, a_desc);
    }
    /// @brief 設定の公開失敗を利用側へ返す
    Result<FileSaveOutcome> replace_file(const Path &a_path, std::span<const std::byte> a_data) override
    {
        ++saveCount;
        return failSave ? Result<FileSaveOutcome>::failure({ErrorCategory::PlatformFailure, "Probe.replace_file"})
                        : files.replace_file(a_path, a_data);
    }
    IFileSystem &files;
    bool failDirectory = false;
    bool failCreate = false;
    bool failRead = false;
    bool failSave = false;
    std::string failedFilename;
    std::function<void(const Path &)> onOpen;
    unsigned saveCount = 0;
};
} // namespace cue::tests
