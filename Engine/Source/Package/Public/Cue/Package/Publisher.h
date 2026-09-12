#pragma once

#include <Cue/Build/Service.h>
#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Result.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/Package/Manifest.h>
#include <Cue/Package/RuntimeData.h>

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

/// @brief Package Manifestの主要IdentityとRun前再照合用Inventoryを所有する診断Summary
struct PackageManifestSummary final
{
    std::string projectId;
    EngineVersion engineVersion;
    BuildConfiguration configuration;
    PackageExecutionModel executionModel;
    std::string startupSceneAssetId;
    std::optional<std::string> applicationExecutable;
    std::optional<ShippingTrustMode> trustMode;
    std::optional<std::string> publisherKeyId;
    bool publicDistributionReady;
    std::vector<PackageFileEntry> files;
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
    /// @brief Rollback再試行に必要な元Filesystem Instance専用のStaging所有Token
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

/// @brief 一つのPackage公開でCancel受理と最終Publish開始を原子的に直列化する共有状態
class PackageCancellation final : public StagingPublishAuthorization, public BuildArtifactReadCancellation
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
    /// @brief 取消状態とPublish Authorization基底を解放する
    ~PackageCancellation() override = default;

    /// @brief Publish開始が確定する前なら取消を設定し、確定後の要求は現在Operationへ影響させない
    void request_cancel() noexcept;
    /// @brief 取消が要求済みか返す
    [[nodiscard]] bool is_cancel_requested() const noexcept override;

  private:
    /// @brief Cancel受理前だけNative Publish開始を原子的に確定する
    [[nodiscard]] bool try_authorize() const noexcept override;

    /// @brief 一つのPackage公開における取消とPublishの排他的状態
    enum class State : std::uint8_t
    {
        Active,
        CancelRequested,
        PublishStarted
    };

    mutable std::atomic<State> m_state = State::Active;
};

/// @brief 検証済みManifestと不変PayloadをOperation所有Stagingから最終Destinationへ一度だけ公開する
///
/// Destination、Manifest、Payload、Cancellation、AssertContextは呼出中だけ借用する。Filesystemは少なくとも呼出中
/// 借用し、ReportがrecoveryStagingを返した場合は、そのTokenを同じFilesystem InstanceでRollbackし終えるまでInstanceを
/// 存続させる。同一Filesystem Instanceへの並行呼出しは行わない。PackageCancellationはOperationごとに新規作成する。
/// Publish前失敗と取消ではDestinationを作らずStagingだけをRollbackする。Publish後のDurabilityUnknownまたは再検証失敗では
/// Destinationを削除せず成功扱いにしない。
[[nodiscard]] PackagePublishReport publish_runtime_package(FilesystemRoot &a_filesystem,
                                                           const RelativePath &a_destination,
                                                           const PackageManifest &a_manifest,
                                                           std::span<const PackageFilePayload> a_payloads,
                                                           const PackageCancellation &a_cancellation,
                                                           const AssertContext &a_assertContext) noexcept;

/// @brief Release Shipping ArtifactとRuntime DataからManifest v2 Monolithic Packageを公開する
///
/// Filesystem、Reader、Artifact、Runtime Data、Destination、Cancellation、AssertContextは呼出中だけ借用する。
/// Project RootはFilesystemのRoot Identityと入口で照合し、Artifact Locator変換とStaging／公開後の完全列挙に使用する。
/// Rootの絶対PathはPackageへ保存しない。ArtifactのShared Read Leaseが示すProject IdentityとRuntime Dataを照合した状態で
/// 固定Executableを一度だけ読み、InventoryのSizeとSHA-256へ照合後にLeaseを解放する。UnsignedLocal ShippingProductだけを
/// 受理し、PublisherSignedは外部署名、Detached Manifest Signature、外部Trust Anchorが実装されるまで拒否する。
/// 入力組立失敗はError、公開処理へ到達後の結果はPackagePublishReportとして返す。同一FilesystemとReaderへの並行呼出しは
/// 行わず、失敗または取消では既存Artifactを変更しない。
[[nodiscard]] Result<PackagePublishReport> publish_monolithic_runtime_package(
    FilesystemRoot &a_projectFilesystem, std::string_view a_projectRoot, BuildArtifactReader &a_artifactReader,
    const BuildArtifactInventory &a_artifact, EngineVersion a_engineVersion,
    const MinimalRuntimeDataPublication &a_runtimeData, const RelativePath &a_destination,
    const PackageCancellation &a_cancellation, const AssertContext &a_assertContext) noexcept;
} // namespace cue::package
