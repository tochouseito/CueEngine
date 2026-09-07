#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/IO/FilesystemIdentity.h>
#include <Cue/IO/RelativePath.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace cue
{
class AssertContext;

/// @brief Root 配下の Entry 種別を Platform 固有属性なしで表す
enum class EntryType : std::uint8_t
{
    Missing,
    RegularFile,
    Directory,
    UnsupportedEntry
};

/// @brief Fileの存在状態とContentを決定的に比較するPortable Snapshot
struct FileFingerprint final
{
    bool exists = false;
    std::uint64_t byteSize = 0U;
    std::uint64_t contentDigest = 0U;

    /// @brief Fingerprintの全要素を比較する
    [[nodiscard]] bool operator==(const FileFingerprint &) const noexcept = default;
};

/// @brief Platform実装が所有するFile Write Leaseの破棄境界
class FileWriteLeaseState
{
  public:
    /// @brief Native Lease Resourceを実装側で解放する
    virtual ~FileWriteLeaseState() = default;

  protected:
    FileWriteLeaseState() noexcept = default;
};

/// @brief 一つのDestinationを直列化する取得Thread限定Move-only Lease
///
/// Move、Conditional Write、破棄は取得Thread上だけで行う
class FileWriteLease final
{
  public:
    FileWriteLease() = delete;
    FileWriteLease(const FileWriteLease &) = delete;
    FileWriteLease &operator=(const FileWriteLease &) = delete;
    FileWriteLease(FileWriteLease &&) noexcept;
    FileWriteLease &operator=(FileWriteLease &&) noexcept;
    ~FileWriteLease();

  private:
    friend class FilesystemRoot;

    explicit FileWriteLease(std::unique_ptr<FileWriteLeaseState> a_state) noexcept;

    std::unique_ptr<FileWriteLeaseState> m_state;
};

/// @brief Publish 前の Operation 所有 Staging Directory を偽造不能な Token と共に保持する
class StagingArea final
{
  public:
    /// @brief 所有権のない Staging 値を作らせないため既定構築を禁止する
    StagingArea() = delete;
    /// @brief Operation 所有権を一意に保つため Copy 構築を禁止する
    StagingArea(const StagingArea &) = delete;
    /// @brief Operation 所有権を一意に保つため Copy 代入を禁止する
    StagingArea &operator=(const StagingArea &) = delete;
    /// @brief Staging 所有 Token を移動し、移動元を無効にする
    StagingArea(StagingArea &&a_other) noexcept;
    /// @brief 両方の Operation 所有権を失わないよう Path と Token を交換する
    ///
    /// 移動元は代入先が以前所有していた Staging を保持するため、必要なら明示 Rollback する
    StagingArea &operator=(StagingArea &&a_other) noexcept;
    /// @brief 値だけを破棄し、Filesystem 変更を暗黙実行しない
    ~StagingArea() = default;

    /// @brief Staging Directory の Root 相対 Path を返す
    [[nodiscard]] const RelativePath &path() const noexcept;

  private:
    friend class FilesystemRoot;

    /// @brief Filesystem 実装が発行した Path と Token から Staging 所有値を構築する
    StagingArea(RelativePath &&a_path, std::uint64_t a_token) noexcept;

    RelativePath m_path;
    std::uint64_t m_token;
};

/// @brief 検証済み Root 内だけを操作する Platform 非依存 Filesystem 契約
///
/// Instance は Thread-safe ではなく、同一 Instance の並行利用は呼び出し側が同期する
class FilesystemRoot
{
  public:
    /// @brief Root Object の一意所有を保つため Copy 構築を禁止する
    FilesystemRoot(const FilesystemRoot &) = delete;
    /// @brief Root Object の一意所有を保つため Copy 代入を禁止する
    FilesystemRoot &operator=(const FilesystemRoot &) = delete;
    /// @brief Polymorphic Root の Native Resource を実装側で解放する
    virtual ~FilesystemRoot() = default;

    /// @brief PathやHandleを公開せずBinding済みRootの比較用Identityを返す
    [[nodiscard]] virtual Result<FilesystemIdentity> root_identity() const noexcept = 0;

    /// @brief Entry を Follow せず Portable 種別として返す
    [[nodiscard]] virtual Result<EntryType> query_entry(const RelativePath &a_path) noexcept = 0;
    /// @brief 上限内の Regular File 全体を所有 Byte 列として返す
    [[nodiscard]] virtual Result<std::vector<std::byte>> read_file(const RelativePath &a_path,
                                                                   std::size_t a_maxBytes) noexcept = 0;
    /// @brief Root 配下に不足する Directory を親から作成する
    [[nodiscard]] virtual Result<void> create_directories(const RelativePath &a_path) noexcept = 0;
    /// @brief 同一 Directory の Temporary File を用いて File Content を Atomic に公開する
    [[nodiscard]] virtual Result<void> write_file_atomic(const RelativePath &a_path,
                                                         std::span<const std::byte> a_bytes) noexcept = 0;
    /// @brief Portable Locator上限を再適用せずDestinationのSibling Recovery BackupをAtomic公開する
    [[nodiscard]] virtual Result<void> write_recovery_backup_atomic(const RelativePath &a_destination,
                                                                     std::span<const std::byte> a_bytes,
                                                                     const AssertContext &a_assertContext) noexcept = 0;
    /// @brief Destinationを対象とするCross-process Write Leaseを取得する
    [[nodiscard]] virtual Result<FileWriteLease> acquire_file_write_lease(const RelativePath &a_path) noexcept = 0;
    /// @brief Lease保持中に期待Fingerprintを再検査し、一致時だけAtomic Publishする
    [[nodiscard]] virtual Result<void> write_file_atomic_if_unchanged(FileWriteLease &a_lease,
                                                                      const RelativePath &a_path,
                                                                      FileFingerprint a_expected,
                                                                      std::size_t a_maximumExpectedBytes,
                                                                      std::span<const std::byte> a_bytes) noexcept = 0;
    /// @brief Regular Fileだけを削除する。Entryが存在しない場合は成功とする
    [[nodiscard]] virtual Result<void> remove_file(const RelativePath &a_path) noexcept = 0;
    /// @brief 最終 Destination の Sibling に Operation 所有 Staging Directory を排他的に作成する
    [[nodiscard]] virtual Result<StagingArea> create_staging_area(const RelativePath &a_destination) noexcept = 0;
    /// @brief Operation 所有 Staging を既存 Destination へ上書きせず一度だけ公開する
    ///
    /// Publish 前失敗では Destination を変更せず Token を有効に保ち、Rollback を再試行できる
    /// Publish 後の DurabilityUnknown では Destination が公開済みで Token は無効になる
    /// 成功時は Destination が公開済みになり Token は無効になる
    [[nodiscard]] virtual Result<void> publish_staging_area(StagingArea &&a_staging,
                                                            const RelativePath &a_destination) noexcept = 0;
    /// @brief Operation 所有 Staging だけを再帰削除し、Token を無効化する
    ///
    /// 削除失敗では Staging と Token を保持し、診断後に Rollback を再試行できる
    /// 成功時だけ Staging を削除して Token を無効にする
    [[nodiscard]] virtual Result<void> rollback_staging_area(StagingArea &&a_staging) noexcept = 0;

  protected:
    /// @brief Platform Provider ScopeとEntry値から比較専用Identityを構築する
    [[nodiscard]] static FilesystemIdentity make_filesystem_identity(std::uint64_t a_providerScope,
                                                                     std::uint64_t a_entry) noexcept;
    /// @brief Platform Provider、Volume Object、Entryの完全な値から比較専用Identityを構築する
    [[nodiscard]] static FilesystemIdentity make_filesystem_identity(std::uint64_t a_provider,
                                                                     std::uint64_t a_volumeHigh,
                                                                     std::uint64_t a_volumeLow,
                                                                     std::uint64_t a_entryHigh,
                                                                     std::uint64_t a_entryLow) noexcept;
    /// @brief Derived 実装だけが Staging 所有値を発行できるよう Path と Token を束ねる
    [[nodiscard]] static StagingArea make_staging_area(RelativePath &&a_path, std::uint64_t a_token) noexcept;
    /// @brief Platform実装が所有するStateからWrite Leaseを構築する
    [[nodiscard]] static FileWriteLease make_file_write_lease(std::unique_ptr<FileWriteLeaseState> a_state) noexcept;
    /// @brief Derived実装がLease所有Stateを検証する
    [[nodiscard]] static FileWriteLeaseState *file_write_lease_state(FileWriteLease &a_lease) noexcept;
    /// @brief Derived 実装が Staging 所有 Token を検証するため値を返す
    [[nodiscard]] static std::uint64_t staging_token(const StagingArea &a_staging) noexcept;
    /// @brief Derived 実装が Commit 済み Token を再利用不能にする
    static void invalidate_staging(StagingArea &a_staging) noexcept;

    /// @brief Abstract Root の初期化だけを Derived 実装へ許可する
    FilesystemRoot() noexcept = default;
    /// @brief Abstract Root を直接移動させず Derived 所有権で管理する
    FilesystemRoot(FilesystemRoot &&) noexcept = default;
    /// @brief Abstract Root を直接移動代入させず Derived 所有権で管理する
    FilesystemRoot &operator=(FilesystemRoot &&) noexcept = default;
};

/// @brief Byte列の決定的FNV-1a 64-bit Digestを返す
[[nodiscard]] std::uint64_t file_content_digest(std::span<const std::byte> a_bytes) noexcept;
/// @brief Root内Fileを上限付きで読込みPortable Fingerprintを返す
[[nodiscard]] Result<FileFingerprint> fingerprint_file(FilesystemRoot &a_filesystem, const RelativePath &a_path,
                                                       std::size_t a_maxBytes,
                                                       const AssertContext &a_assertContext) noexcept;
} // namespace cue
