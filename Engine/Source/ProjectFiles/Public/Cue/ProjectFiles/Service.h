#pragma once

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Result.h>
#include <Cue/IO/WorkspaceFilesystem.h>
#include <Cue/Project/Descriptor.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cue::project_files
{
/// @brief Project Descriptorで定義された役割別Area
enum class ProjectFileArea : std::uint8_t
{
    SourceAssets,
    RuntimeAssets,
    Generated,
    Saved
};

/// @brief Native Dialog選択を再検証するときに要求するEntry意味
enum class ProjectFileSelectionPurpose : std::uint8_t
{
    OpenExistingFile,
    SaveFileDestination,
    SelectExistingDirectory
};

/// @brief Project File操作の意味分類
enum class ProjectFileOperationKind : std::uint8_t
{
    DirectoryCreation,
    FileCreation,
    Rename,
    Move,
    Copy,
    RecoverableDelete,
    Restore
};

/// @brief Project File操作が到達した処理段階
enum class ProjectFileOperationStage : std::uint8_t
{
    ValidateRequest,
    BindSource,
    BindDestination,
    PrepareRecovery,
    NativePublish,
    UpdateRecoveryRecord,
    Verify,
    Complete
};

/// @brief Project File操作後に確定した観測結果
enum class ProjectFileOperationOutcome : std::uint8_t
{
    Committed,
    NotCommitted,
    CommittedButDurabilityUnknown,
    ReconciliationRequired
};

/// @brief Trash Catalog Entryが通常復元可能か手動照合を要するか表す
enum class RecoveryEntryState : std::uint8_t
{
    Recoverable,
    ReconciliationRequired
};

/// @brief Project内TrashからUIへ公開する復元候補Metadata
struct RecoveryEntry final
{
    std::string operationId;
    ProjectFileArea originalArea = ProjectFileArea::SourceAssets;
    std::string originalPath;
    WorkspaceEntryType entryType = WorkspaceEntryType::UnsupportedEntry;
    std::uint64_t byteSize = 0U;
    std::size_t descendantCount = 0U;
    RecoveryEntryState state = RecoveryEntryState::ReconciliationRequired;
};

/// @brief Operation ID発行をPlatformまたは決定的Test Doubleへ分離する境界
class ProjectFileOperationIdSource
{
  public:
    /// @brief Operation ID Sourceの一意所有を保つためCopy構築を禁止する
    ProjectFileOperationIdSource(const ProjectFileOperationIdSource &) = delete;
    /// @brief Operation ID Sourceの一意所有を保つためCopy代入を禁止する
    ProjectFileOperationIdSource &operator=(const ProjectFileOperationIdSource &) = delete;
    /// @brief 実装固有Resourceを派生型で解放する
    virtual ~ProjectFileOperationIdSource() = default;

    /// @brief lowercase UUID Version 4候補を返す
    [[nodiscard]] virtual Result<std::string> next_operation_id() noexcept = 0;

  protected:
    /// @brief 派生型だけがOperation ID Sourceを構築できるようにする
    ProjectFileOperationIdSource() noexcept = default;
};

/// @brief 用途別Areaの読取り・Mutation権限を固定する不変Policy
class ProjectFileAccessPolicy final
{
  public:
    /// @brief M13 Editor用のSource Assets限定Policyを返す
    [[nodiscard]] static ProjectFileAccessPolicy editor_default() noexcept;

    /// @brief Files UIへ表示可能なAreaか判定する
    [[nodiscard]] bool can_list(ProjectFileArea a_area) const noexcept;
    /// @brief 通常File Mutationを許可するAreaか判定する
    [[nodiscard]] bool can_mutate(ProjectFileArea a_area) const noexcept;

  private:
    /// @brief 固定Editor Policyを内部Factoryだけで構築する
    ProjectFileAccessPolicy() noexcept = default;
};

/// @brief Operation ID、結果、Primary Error、Cleanup診断、再列挙対象を所有する
class ProjectFileOperationResult final
{
  public:
    /// @brief Operation IDを持たない結果の構築を禁止する
    ProjectFileOperationResult() = delete;
    /// @brief Error所有権の重複を防ぐためCopy構築を禁止する
    ProjectFileOperationResult(const ProjectFileOperationResult &) = delete;
    /// @brief Error所有権の重複を防ぐためCopy代入を禁止する
    ProjectFileOperationResult &operator=(const ProjectFileOperationResult &) = delete;
    /// @brief Operation結果の所有権を移動する
    ProjectFileOperationResult(ProjectFileOperationResult &&) noexcept = default;
    /// @brief 現在の結果を解放してOperation結果の所有権を移動する
    ProjectFileOperationResult &operator=(ProjectFileOperationResult &&) noexcept = default;
    /// @brief 所有するErrorと診断Listを解放する
    ~ProjectFileOperationResult() = default;

    /// @brief 呼出し全体を関連付けるOperation IDを返す
    [[nodiscard]] std::string_view operation_id() const noexcept;
    /// @brief 実行したSemantic Operation種別を返す
    [[nodiscard]] ProjectFileOperationKind kind() const noexcept;
    /// @brief 操作対象のProject Areaを返す
    [[nodiscard]] ProjectFileArea area() const noexcept;
    /// @brief Area Root相対Source Locatorが存在する場合だけ返す
    [[nodiscard]] std::optional<std::string_view> source() const noexcept;
    /// @brief Area Root相対Destination Locatorを返す
    [[nodiscard]] std::string_view destination() const noexcept;
    /// @brief 操作が到達した最終Stageを返す
    [[nodiscard]] ProjectFileOperationStage stage() const noexcept;
    /// @brief Filesystem観測から確定したOperation結果を返す
    [[nodiscard]] ProjectFileOperationOutcome outcome() const noexcept;
    /// @brief Primary Errorが存在する場合だけ参照を返す
    [[nodiscard]] const Error *try_primary_error() const noexcept;
    /// @brief RollbackまたはCleanupのSecondary診断を返す
    [[nodiscard]] std::span<const Error> secondary_diagnostics() const noexcept;
    /// @brief 呼出し側が再列挙すべきDirectory Locatorを返す
    [[nodiscard]] std::span<const std::string> rescan_directories() const noexcept;

  private:
    friend class ProjectFileService;
    /// @brief Serviceが確定したOperation結果を全Field所有で構築する
    ProjectFileOperationResult(std::string a_operationId, ProjectFileOperationKind a_kind, ProjectFileArea a_area,
                               std::optional<std::string> a_source, std::string a_destination,
                               ProjectFileOperationStage a_stage, ProjectFileOperationOutcome a_outcome,
                               std::optional<Error> a_primaryError, std::vector<Error> a_secondaryDiagnostics,
                               std::vector<std::string> a_rescanDirectories) noexcept;

    std::string m_operationId;
    ProjectFileOperationKind m_kind;
    ProjectFileArea m_area;
    std::optional<std::string> m_source;
    std::string m_destination;
    ProjectFileOperationStage m_stage;
    ProjectFileOperationOutcome m_outcome;
    std::optional<Error> m_primaryError;
    std::vector<Error> m_secondaryDiagnostics;
    std::vector<std::string> m_rescanDirectories;
};

/// @brief Project Root、Area Policy、Operation ID、単一Mutationを所有するApplication Service
///
/// Instanceは作成Thread限定で、移動後も同じOwner Threadからだけ使用する
class ProjectFileService final
{
  public:
    /// @brief 必須Dependencyを持たないServiceの構築を禁止する
    ProjectFileService() = delete;
    /// @brief Root CapabilityとOperation Sourceの一意所有を保つためCopy構築を禁止する
    ProjectFileService(const ProjectFileService &) = delete;
    /// @brief Root CapabilityとOperation Sourceの一意所有を保つためCopy代入を禁止する
    ProjectFileService &operator=(const ProjectFileService &) = delete;
    /// @brief Project File Serviceの所有権を移動する
    ProjectFileService(ProjectFileService &&) noexcept = default;
    /// @brief 現在のServiceを解放してProject File Serviceの所有権を移動する
    ProjectFileService &operator=(ProjectFileService &&) noexcept = default;
    /// @brief WorkspaceとOperation ID Sourceを解放する
    ~ProjectFileService() = default;

    /// @brief Descriptor SnapshotとRoot-bound Workspaceを検証し、部分Serviceを公開せず構築する
    ///
    /// a_assertContextが参照するLoggerとFatalHandlerは、返されたServiceより長く生存しなければならない
    [[nodiscard]] static Result<ProjectFileService> create(
        const ProjectDescriptor &a_descriptor, std::unique_ptr<WorkspaceFilesystem> a_workspace,
        std::unique_ptr<ProjectFileOperationIdSource> a_operationIdSource,
        const AssertContext &a_assertContext) noexcept;

    /// @brief Serviceが拘束されたProject IDを返す
    [[nodiscard]] const ProjectId &project_id() const noexcept;
    /// @brief 構築時に固定したProject Root Snapshotを返す
    [[nodiscard]] const ProjectRoots &roots() const noexcept;
    /// @brief Serviceが適用するArea Access Policyを返す
    [[nodiscard]] const ProjectFileAccessPolicy &access_policy() const noexcept;

    /// @brief 未検証Absolute PathをArea境界、親Chain、Entry種別に照らして再検証する
    ///
    /// 返されたLocatorは観測時点の値であり、実際の読取りまたはMutationは利用直前に再検証する。
    [[nodiscard]] Result<RelativePath> revalidate_external_selection(ProjectFileArea a_area,
                                                                     std::string_view a_unverifiedAbsolutePath,
                                                                     ProjectFileSelectionPurpose a_purpose) noexcept;

    /// @brief 指定AreaへCreate-new FolderをAtomic公開する
    [[nodiscard]] Result<ProjectFileOperationResult> create_directory(ProjectFileArea a_area,
                                                                      RelativePath a_destination) noexcept;

    /// @brief 指定Areaへ初期Content付きFileをAtomic Create-newする
    [[nodiscard]] Result<ProjectFileOperationResult> create_file(ProjectFileArea a_area, RelativePath a_destination,
                                                                 std::span<const std::byte> a_bytes) noexcept;

    /// @brief 同じ親Directory内でSource Entryの名前だけを変更する
    [[nodiscard]] Result<ProjectFileOperationResult> rename(ProjectFileArea a_area, RelativePath a_source,
                                                            RelativePath a_destination,
                                                            TraversalLimits a_limits) noexcept;

    /// @brief 同じProject Area内の別DirectoryへSource Entryを移動する
    [[nodiscard]] Result<ProjectFileOperationResult> move(ProjectFileArea a_area, RelativePath a_source,
                                                          RelativePath a_destination,
                                                          TraversalLimits a_limits) noexcept;

    /// @brief Source Entryを保持してCreate-new Copyを同じProject Areaへ公開する
    [[nodiscard]] Result<ProjectFileOperationResult> copy(ProjectFileArea a_area, RelativePath a_source,
                                                          RelativePath a_destination, TraversalLimits a_traversalLimits,
                                                          ContentVerificationLimits a_contentLimits) noexcept;

    /// @brief Source EntryをProject内Trashへ移して復元Recordを公開する
    [[nodiscard]] Result<ProjectFileOperationResult> delete_entry(ProjectFileArea a_area, RelativePath a_source,
                                                                  TraversalLimits a_traversalLimits,
                                                                  ContentVerificationLimits a_contentLimits) noexcept;

    /// @brief Trash Operation IDが記録した元PathへPayloadを上書きなしで復元する
    [[nodiscard]] Result<ProjectFileOperationResult> restore(std::string_view a_operationId,
                                                             TraversalLimits a_traversalLimits,
                                                             ContentVerificationLimits a_contentLimits) noexcept;

    /// @brief Project内Trashを再走査し、安全に照合できたRecovery Catalogを更新する
    [[nodiscard]] Result<void> refresh_recovery_catalog(TraversalLimits a_traversalLimits,
                                                        ContentVerificationLimits a_contentLimits) noexcept;

    /// @brief 最後に更新したRecovery Catalog Entryを返す
    [[nodiscard]] std::span<const RecoveryEntry> recovery_entries() const noexcept;

    /// @brief Catalog再構築で保持したEntry単位診断を返す
    [[nodiscard]] std::span<const Error> recovery_diagnostics() const noexcept;

  private:
    /// @brief 検証済みProject SnapshotとDependencyの所有権を受け取る
    ProjectFileService(ProjectId a_projectId, ProjectRoots a_roots, ProjectFileAccessPolicy a_policy,
                       std::unique_ptr<WorkspaceFilesystem> a_workspace,
                       std::unique_ptr<ProjectFileOperationIdSource> a_operationIdSource,
                       const AssertContext &a_assertContext) noexcept;

    /// @brief Create要求の共通検証、Native Publish、結果変換を実行する
    [[nodiscard]] Result<ProjectFileOperationResult> execute_create(ProjectFileOperationKind a_kind,
                                                                    ProjectFileArea a_area, RelativePath a_destination,
                                                                    std::span<const std::byte> a_bytes) noexcept;
    /// @brief Transfer要求の共通検証、Native実行、結果変換を行う
    [[nodiscard]] Result<ProjectFileOperationResult> execute_transfer(
        ProjectFileOperationKind a_kind, ProjectFileArea a_area, RelativePath a_source, RelativePath a_destination,
        TraversalLimits a_traversalLimits, ContentVerificationLimits a_contentLimits) noexcept;
    /// @brief Areaに対応する固定Root Locatorを返す
    [[nodiscard]] const RelativePath &area_root(ProjectFileArea a_area) const noexcept;

    /// @brief Saved Root内のTrash Directory Chainを存在確認付きで準備する
    [[nodiscard]] WorkspaceMutationResult ensure_trash_root(std::string_view a_operationId) noexcept;
    /// @brief Operation IDに対応するRecord Byte列を上限付きで読み込む
    [[nodiscard]] Result<std::vector<std::byte>> read_trash_record_bytes(std::string_view a_operationId) noexcept;

    ProjectId m_projectId;
    ProjectRoots m_roots;
    ProjectFileAccessPolicy m_policy;
    std::unique_ptr<WorkspaceFilesystem> m_workspace;
    std::unique_ptr<ProjectFileOperationIdSource> m_operationIdSource;
    AssertContext m_assertContext;
    std::thread::id m_ownerThread;
    std::vector<RecoveryEntry> m_recoveryEntries;
    std::vector<Error> m_recoveryDiagnostics;
    bool m_isBusy = false;
};
} // namespace cue::project_files
