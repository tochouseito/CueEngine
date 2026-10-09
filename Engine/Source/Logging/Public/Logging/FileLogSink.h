#pragma once

#include <memory>

#include <Foundation/Logging.h>
#include <Platform/FileSystem.h>

namespace cue
{
/// @brief 一つの File を開いたまま保持し、UTF-8 の追記を行う
/// Factory は FileSystem を同期で借用する。FileSystem は Sink より長く維持する
/// 全操作を外部で直列化し再入しない。書込み失敗後は追記を停止し、部分行を再送しない
class FileLogSink final : public ILogSink
{
    struct CreateToken final
    {
    };

  public:
    /// @brief 開いた File の所有権を受け取る
    FileLogSink(CreateToken, std::unique_ptr<IFile> a_file);
    /// @brief 残っている File Handle を回収する
    ~FileLogSink() override;
    /// @brief 親 Directory を用意し、一意な新規 File または既存 File の末尾を開く
    /// a_append=false は CreateNew で既存 File を保全する。true の同時 Writer はサポートしない
    [[nodiscard]] static Result<std::unique_ptr<FileLogSink>> create(IFileSystem &a_files, const Path &a_path,
                                                                     bool a_append = false);
    /// @brief 部分書込みを繰り返し、一行を完全に転送する
    [[nodiscard]] Result<void> write(std::string_view a_line) override;
    /// @brief OS の File Buffer を Flush する。電源断耐性は保証しない
    [[nodiscard]] Result<void> flush() override;
    /// @brief Flush と Close を試み、最初の失敗を返す
    [[nodiscard]] Result<void> close() override;

  private:
    std::unique_ptr<IFile> m_file;
    bool m_hasWriteFailure = false;
};
} // namespace cue
