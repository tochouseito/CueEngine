#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <Foundation/Path.h>

namespace cue
{
enum class FileType
{
    Regular,
    Directory,
    Link
};
struct FileInfo final
{
    FileType type = FileType::Regular;
    std::uint64_t size = 0;
    // Unix Epoch からの ns。Native の精度以上の精度を保証しない
    std::int64_t modifiedNanoseconds = 0;
};
enum class FileAccess
{
    Read,
    Write,
    ReadWrite
};
enum class FileCreation
{
    OpenExisting,
    OpenAlways,
    CreateNew,
    CreateAlways,
    TruncateExisting
};
enum class SeekOrigin
{
    Begin,
    Current,
    End
};
struct FileOpenDesc final
{
    FileAccess access = FileAccess::Read;
    FileCreation creation = FileCreation::OpenExisting;
    bool isReadShared = true;
    bool isWriteShared = false;
    bool isDeleteShared = false;
};

/// @brief 同期 File Handle を一意所有し、結果を呼出側へ渡す
///
/// 同一 Object の全操作・破棄は外部で直列化し、再入しない。入力 Span は呼出中だけ借用する
/// read の 0 は EOF。成功した部分転送数を返す。失敗時は Buffer / Cursor が一部変更され得る
/// close は成功後の再呼出しも成功し、以後の他操作は InvalidState。Destructor は最後の回収を行う
class IFile
{
  public:
    /// @brief 派生実装の Handle を構築する
    IFile() = default;
    /// @brief 残っている Native Handle を回収する
    virtual ~IFile() = default;
    /// @brief Handle 所有権の複製を禁止する
    IFile(const IFile &) = delete;
    /// @brief Handle 所有権の複製を禁止する
    IFile &operator=(const IFile &) = delete;
    /// @brief EOF または成功した部分読込み数を返す
    [[nodiscard]] virtual Result<std::size_t> read(std::span<std::byte> a_destination) = 0;
    /// @brief 成功した部分書込み数を返す
    [[nodiscard]] virtual Result<std::size_t> write(std::span<const std::byte> a_source) = 0;
    /// @brief 指定基準から移動し、新しい位置を返す
    [[nodiscard]] virtual Result<std::uint64_t> seek(std::int64_t a_offset, SeekOrigin a_origin) = 0;
    /// @brief 現在の位置を返す
    [[nodiscard]] virtual Result<std::uint64_t> tell() = 0;
    /// @brief 現在の File Size を返す
    [[nodiscard]] virtual Result<std::uint64_t> size() = 0;
    /// @brief OS の File Buffer を Flush し、失敗を報告する
    [[nodiscard]] virtual Result<void> flush() = 0;
    /// @brief Handle を閉じ、失敗時は回収を再試行できる状態を保つ
    [[nodiscard]] virtual Result<void> close() = 0;
};

/// @brief 公開済み内容と電源断耐性の確認結果を区別する
struct FileSaveOutcome final
{
    // 成功結果は公開済み。false は Directory Entry の永続化を保証できないことを表す
    bool isDurabilityConfirmed = false;
};

/// @brief Platform の同期 File 操作と保存先取得を抽象化する
///
/// Factory の結果は呼出側が一意所有し、借用者と開いた IFile より長く維持する
/// 同時呼出しの可否は具体実装が定める。Path は呼出中だけ借用し、返却値は所有する
/// rename は上書きせず、remove は空 Directory / 単一 File のみ。再帰削除は提供しない
/// 存在確認は後続操作の成功を保証しない。相対 Path の意味は実装の作業 Directory に従う
/// read_all は Allocation 失敗も Error にする。他の値構築の Allocation 失敗は例外として伝播する
class IFileSystem
{
  public:
    /// @brief 派生実装を構築する
    IFileSystem() = default;
    /// @brief 全利用者の停止後に実装を破棄する
    virtual ~IFileSystem() = default;
    /// @brief Service 所有権の複製を禁止する
    IFileSystem(const IFileSystem &) = delete;
    /// @brief Service 所有権の複製を禁止する
    IFileSystem &operator=(const IFileSystem &) = delete;
    /// @brief 実行 File の配置 Directory を絶対 Path で返す
    [[nodiscard]] virtual Result<Path> executable_directory() = 0;
    /// @brief ユーザー単位のローカルデータ Directory を絶対 Path で返す
    [[nodiscard]] virtual Result<Path> local_data_directory() = 0;
    /// @brief 未存在は空の成功値、それ以外の問い合わせ失敗は Error を返す
    [[nodiscard]] virtual Result<std::optional<FileInfo>> stat(const Path &a_path) = 0;
    /// @brief stat と同じ失敗分類で存在有無を返す
    [[nodiscard]] Result<bool> exists(const Path &a_path);
    /// @brief 必要な親を含め Directory を作る。途中失敗では作成済みの親が残り得る
    [[nodiscard]] virtual Result<void> create_directories(const Path &a_path) = 0;
    /// @brief 直下の要素の所有 Path を返す。順序は未定義で途中失敗は部分一覧を公開しない
    [[nodiscard]] virtual Result<std::vector<Path>> list_directory(const Path &a_path) = 0;
    /// @brief 単一 File / Link / 空 Directory を削除する。未存在は false を返す
    [[nodiscard]] virtual Result<bool> remove(const Path &a_path) = 0;
    /// @brief 同一 FileSystem 内で移動し、既存の移動先は置換しない
    [[nodiscard]] virtual Result<void> rename(const Path &a_from, const Path &a_to) = 0;
    /// @brief 単一 File をコピーする。失敗時は移動先が部分変更され得る
    [[nodiscard]] virtual Result<void> copy_file(const Path &a_from, const Path &a_to, bool a_overwrite) = 0;
    /// @brief File を開き、成功時のみ一意所有の Handle を公開する
    [[nodiscard]] virtual Result<std::unique_ptr<IFile>> open(const Path &a_path, const FileOpenDesc &a_desc = {}) = 0;
    /// @brief 上限まで読み、EOF 前の Size 変化と Close 失敗を Error にする
    [[nodiscard]] Result<std::vector<std::byte>> read_all(const Path &a_path,
                                                          std::size_t a_maxBytes = 64 * 1024 * 1024);
    /// @brief 完全転送・Flush・Close を確認する。既存 File を直接変更するため正本の保存には使わない
    [[nodiscard]] Result<void> write_all(const Path &a_path, std::span<const std::byte> a_data,
                                         bool a_createParents = false);
    /// @brief 同じ Directory の一意な一時 File を完成後に公開する
    ///
    /// 公開前の失敗は旧保存先を維持する。Cleanup 失敗は Error の診断へ含める
    /// 成功値は公開済みで、Durability 未確認は通常の失敗と区別する。ACL / Remote FS の保証は実装依存
    [[nodiscard]] virtual Result<FileSaveOutcome> replace_file(const Path &a_path,
                                                               std::span<const std::byte> a_data) = 0;
};
} // namespace cue
