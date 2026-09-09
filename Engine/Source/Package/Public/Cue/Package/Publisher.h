#pragma once

#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Result.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/Package/Manifest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::package
{
/// @brief Package公開処理が最後に到達したStage
enum class PackagePublishStage : std::uint8_t
{
    ValidateInput,
    CreateStaging,
    WriteContent,
    WriteManifest,
    ValidateStaging,
    Publish,
    ValidatePublished,
    Completed
};

/// @brief 最終Destinationの可視性と成功可否を表すPackage公開結果
enum class PackagePublishOutcome : std::uint8_t
{
    Committed,
    NotPublished,
    PublishedButDurabilityUnknown
};

/// @brief Package Manifestの主要IdentityとInventory規模を所有する診断Summary
struct PackageManifestSummary final
{
    std::string projectId;
    EngineVersion engineVersion;
    BuildConfiguration configuration;
    std::string startupSceneAssetId;
    std::size_t fileCount;
    std::uint64_t inventoryBytes;
};

/// @brief Package公開のStage、Outcome、Destination、Manifest Summary、失敗診断を所有するReport
struct PackagePublishReport final
{
    PackagePublishStage stage;
    PackagePublishOutcome outcome;
    std::string destination;
    PackageManifestSummary manifest;
    std::optional<Error> error;
    std::optional<StagingArea> recoveryStaging;

    /// @brief 最終Destinationを再検証してCommit済みか返す
    [[nodiscard]] bool succeeded() const noexcept;
};

/// @brief Manifest Entryと一致する不変Byte Snapshotを所有するPackage入力File
class PackageFilePayload final
{
  public:
    /// @brief 未検証Payloadを作らせないため既定構築を禁止する
    PackageFilePayload() = delete;
    /// @brief 大容量Byte Snapshotの暗黙複製を禁止する
    PackageFilePayload(const PackageFilePayload &) = delete;
    /// @brief 大容量Byte Snapshotの暗黙複製代入を禁止する
    PackageFilePayload &operator=(const PackageFilePayload &) = delete;
    /// @brief EntryとByte Snapshotの所有権を移動する
    PackageFilePayload(PackageFilePayload &&) noexcept = default;
    /// @brief EntryとByte Snapshotの所有権を移動代入する
    PackageFilePayload &operator=(PackageFilePayload &&) noexcept = default;
    /// @brief 所有Byte Snapshotを解放する
    ~PackageFilePayload() = default;

    /// @brief Role、Path、Byte列を検証しSHA-256付きの不変入力Snapshotを構築する
    [[nodiscard]] static Result<PackageFilePayload> create(PackageFileRole a_role, std::string a_relativePath,
                                                           std::vector<std::byte> a_bytes,
                                                           const AssertContext &a_assertContext) noexcept;

    /// @brief Manifestへ渡せる検証済みEntryを返す
    [[nodiscard]] const PackageFileEntry &entry() const noexcept;
    /// @brief Stagingへ書き込む所有Byte列を返す
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

  private:
    /// @brief 検証済みEntryと対応Byte Snapshotを所有する
    PackageFilePayload(PackageFileEntry a_entry, std::vector<std::byte> a_bytes) noexcept;

    PackageFileEntry m_entry;
    std::vector<std::byte> m_bytes;
};

/// @brief Package公開の取消要求をThread間で伝える共有Flag
class PackageCancellation final
{
  public:
    /// @brief 未取消状態を構築する
    PackageCancellation() noexcept = default;
    /// @brief 共有取消Flagの複製を禁止する
    PackageCancellation(const PackageCancellation &) = delete;
    /// @brief 共有取消Flagの複製代入を禁止する
    PackageCancellation &operator=(const PackageCancellation &) = delete;
    /// @brief 実行中Operationが参照する取消Flagの移動を禁止する
    PackageCancellation(PackageCancellation &&) = delete;
    /// @brief 実行中Operationが参照する取消Flagの移動代入を禁止する
    PackageCancellation &operator=(PackageCancellation &&) = delete;
    /// @brief 取消Flagを解放する
    ~PackageCancellation() = default;

    /// @brief 最終Rename前に観測される取消要求を設定する
    void request_cancel() noexcept;
    /// @brief 取消が要求済みか返す
    [[nodiscard]] bool is_cancel_requested() const noexcept;

  private:
    std::atomic_bool m_cancelRequested = false;
};

/// @brief 検証済みManifestと不変PayloadをOperation所有Stagingから最終Destinationへ一度だけ公開する
///
/// Filesystem、Destination、Manifest、Payload、Cancellation、AssertContextは呼出中だけ借用する。同一Filesystem
/// Instanceへの並行呼出しは行わない。Publish前失敗と取消ではDestinationを作らずStagingだけをRollbackする。
/// Publish後のDurabilityUnknownまたは再検証失敗ではDestinationを削除せず成功扱いにしない。
[[nodiscard]] PackagePublishReport publish_runtime_package(
    FilesystemRoot &a_filesystem, const RelativePath &a_destination, const PackageManifest &a_manifest,
    std::span<const PackageFilePayload> a_payloads, const PackageCancellation &a_cancellation,
    const AssertContext &a_assertContext) noexcept;
} // namespace cue::package
