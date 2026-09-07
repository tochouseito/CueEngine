#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/IO/RelativePath.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
class WorkspaceDirectory;
class WorkspaceEntryMutationGuard;
class WorkspaceFilesystem;

/// @brief 検証済みUser Locatorと列挙結果からだけ生成できるRoot相対内部Path
class BoundWorkspacePath final
{
  public:
    /// @brief 同じWorkspace Bindingを共有する値CapabilityとしてCopy構築する
    BoundWorkspacePath(const BoundWorkspacePath &) = default;
    /// @brief 同じWorkspace Bindingを共有する値CapabilityとしてCopy代入する
    BoundWorkspacePath &operator=(const BoundWorkspacePath &) = default;
    /// @brief Workspace Binding付きPath Valueを移動構築する
    BoundWorkspacePath(BoundWorkspacePath &&) noexcept = default;
    /// @brief Workspace Binding付きPath Valueを移動代入する
    BoundWorkspacePath &operator=(BoundWorkspacePath &&) noexcept = default;

    /// @brief Root相対のPortable Path文字列を返す
    [[nodiscard]] std::string_view text() const noexcept;

  private:
    friend class WorkspaceDirectory;
    friend class WorkspaceFilesystem;

    /// @brief 検証済みの合成済みPath文字列と発行元Workspaceを所有する
    BoundWorkspacePath(std::string a_text, std::uint64_t a_bindingToken) noexcept;

    std::string m_text;
    std::uint64_t m_bindingToken;
};

/// @brief Workspace列挙で公開するPortable Entry種別
enum class WorkspaceEntryType : std::uint8_t
{
    Directory,
    RegularFile,
    UnsupportedEntry
};

/// @brief 不完全なSnapshotで再列挙理由をPortableに分類する
enum class WorkspaceDiagnosticCode : std::uint8_t
{
    UnsupportedName,
    ReparsePoint,
    EntryDisappeared,
    PermissionDenied,
    TypeChanged,
    EnumerationFailed
};

/// @brief Directory Snapshotが完全か再列挙を要するかを表す
enum class WorkspaceSnapshotState : std::uint8_t
{
    Complete,
    RescanRequired
};

/// @brief Namespace Mutation後に確定したPortableな観測結果
enum class WorkspaceMutationOutcome : std::uint8_t
{
    Committed,
    NotCommitted,
    CommittedButDurabilityUnknown,
    ReconciliationRequired
};

/// @brief Workspace Adapterが返すMutation結果とCleanup診断
struct WorkspaceMutationResult final
{
    WorkspaceMutationOutcome outcome = WorkspaceMutationOutcome::ReconciliationRequired;
    std::optional<Error> primaryError;
    std::vector<Error> secondaryDiagnostics;
};

/// @brief 再帰処理のCaller上限を全て非Zero値で保持する
struct TraversalLimits final
{
    std::size_t maxDepth = 0U;
    std::size_t maxVisitedEntries = 0U;
    std::size_t maxResults = 0U;
    std::size_t maxMetadataBytes = 0U;

    /// @brief 全ての上限が非Zeroか判定する
    [[nodiscard]] bool is_valid() const noexcept;
};

/// @brief Content FingerprintとCopy読取りの非Zero Byte上限
struct ContentVerificationLimits final
{
    std::uint64_t maxFileBytes = 0U;
    std::uint64_t maxTotalBytes = 0U;

    /// @brief File単位とOperation合計の上限が非Zeroか判定する
    [[nodiscard]] bool is_valid() const noexcept;
};

/// @brief Regular Fileの内容とHard Link前提を再検証できるFingerprint
struct WorkspaceFileFingerprint final
{
    std::uint64_t byteSize = 0U;
    std::uint64_t contentDigest = 0U;

    /// @brief File Fingerprintの全要素を比較する
    [[nodiscard]] bool operator==(const WorkspaceFileFingerprint &) const noexcept = default;
};

/// @brief Directory Payload内の一つのEntryをRoot相対Pathで記録する
struct WorkspaceManifestEntry final
{
    std::string path;
    WorkspaceEntryType type = WorkspaceEntryType::UnsupportedEntry;
    std::optional<WorkspaceFileFingerprint> file;

    /// @brief Manifest Entryの全要素を比較する
    [[nodiscard]] bool operator==(const WorkspaceManifestEntry &) const noexcept = default;
};

/// @brief FileまたはDirectory Treeを内容まで照合するPortable Snapshot
struct WorkspaceEntryFingerprint final
{
    WorkspaceEntryType type = WorkspaceEntryType::UnsupportedEntry;
    std::optional<WorkspaceFileFingerprint> file;
    std::vector<WorkspaceManifestEntry> manifest;

    /// @brief Entry Fingerprintの全要素を比較する
    [[nodiscard]] bool operator==(const WorkspaceEntryFingerprint &) const noexcept = default;
};

/// @brief Native EntryのIdentityと変更排他をRecord確定まで保持するOpaque Guard
class WorkspaceEntryMutationGuard
{
  public:
    /// @brief Native Guardの一意所有を保つためCopy構築を禁止する
    WorkspaceEntryMutationGuard(const WorkspaceEntryMutationGuard &) = delete;
    /// @brief Native Guardの一意所有を保つためCopy代入を禁止する
    WorkspaceEntryMutationGuard &operator=(const WorkspaceEntryMutationGuard &) = delete;
    /// @brief Platform実装が所有するNative Resourceを解放する
    virtual ~WorkspaceEntryMutationGuard() = default;

  protected:
    /// @brief Platform実装だけがGuardを構築できる状態にする
    WorkspaceEntryMutationGuard() noexcept = default;
};

/// @brief Guard取得中に確定したFingerprintとNative Guardの一意所有権
struct GuardedWorkspaceEntry final
{
    WorkspaceEntryFingerprint fingerprint;
    std::unique_ptr<WorkspaceEntryMutationGuard> guard;
};

/// @brief Root自体または検証済みRoot相対Directoryを表す
class WorkspaceDirectory final
{
  public:
    /// @brief Workspace Root自体を列挙対象として返す
    [[nodiscard]] static WorkspaceDirectory root() noexcept;

    /// @brief 列挙で生成済みの合成PathをDirectory Locatorとして所有する
    [[nodiscard]] static WorkspaceDirectory from_bound_path(BoundWorkspacePath a_locator) noexcept;

    /// @brief Workspace Root自体を表すか判定する
    [[nodiscard]] bool is_root() const noexcept;

    /// @brief Root相対Locatorを返し、Root自体ならnullptrを返す
    [[nodiscard]] const BoundWorkspacePath *locator() const noexcept;

  private:
    /// @brief Optional Locatorから列挙対象を構築する
    explicit WorkspaceDirectory(std::optional<BoundWorkspacePath> a_locator) noexcept;

    std::optional<BoundWorkspacePath> m_locator;
};

/// @brief Entry単位で発生した列挙競合または拒否理由を保持する
struct WorkspaceDiagnostic final
{
    std::uint64_t generation = 0U;
    WorkspaceDiagnosticCode code = WorkspaceDiagnosticCode::EnumerationFailed;
    std::string displayName;
    std::int64_t nativeCode = 0;
};

/// @brief 正規化済みDirectory Entry Metadataを所有する
struct WorkspaceEntry final
{
    std::uint64_t parentGeneration = 0U;
    std::string displayName;
    std::string sortKey;
    std::optional<BoundWorkspacePath> locator;
    WorkspaceEntryType type = WorkspaceEntryType::UnsupportedEntry;
    std::uint64_t byteSize = 0U;
    std::optional<WorkspaceDiagnosticCode> rejection;

    /// @brief Mutationへ渡せる検証済みLocatorを保持するか判定する
    [[nodiscard]] bool is_operable() const noexcept;
};

/// @brief Directory直下を一回のGenerationで取得した所有Snapshot
struct DirectorySnapshot final
{
    std::uint64_t generation = 0U;
    WorkspaceSnapshotState state = WorkspaceSnapshotState::Complete;
    std::vector<WorkspaceEntry> entries;
    std::vector<WorkspaceDiagnostic> diagnostics;
};

/// @brief Bounded Searchの決定的な所有結果
struct WorkspaceSearchResult final
{
    WorkspaceSnapshotState state = WorkspaceSnapshotState::Complete;
    std::vector<WorkspaceEntry> entries;
    std::vector<WorkspaceDiagnostic> diagnostics;
    std::size_t visitedEntries = 0U;
};

/// @brief Root境界内の列挙CapabilityをPlatform実装へ分離する契約
///
/// InstanceはThread-safeではなく、同一Instanceの並行利用は呼び出し側が同期する
class WorkspaceFilesystem
{
  public:
    /// @brief Root Capabilityの一意所有を保つためCopy構築を禁止する
    WorkspaceFilesystem(const WorkspaceFilesystem &) = delete;
    /// @brief Root Capabilityの一意所有を保つためCopy代入を禁止する
    WorkspaceFilesystem &operator=(const WorkspaceFilesystem &) = delete;
    /// @brief Polymorphic RootのNative Resourceを実装側で解放する
    virtual ~WorkspaceFilesystem() = default;

    /// @brief Adapter固有Hard Limitを返す
    [[nodiscard]] virtual TraversalLimits hard_limits() const noexcept = 0;

    /// @brief Adapter固有Content Verification Hard Limitを返す
    [[nodiscard]] virtual ContentVerificationLimits hard_content_limits() const noexcept = 0;

    /// @brief 未検証Absolute PathをRoot境界内のPortable Capabilityへ変換する
    ///
    /// 成功は存在、Entry種別、Reparse Point不在を保証しない。利用直前に列挙またはMutationで再検証する。
    [[nodiscard]] virtual Result<BoundWorkspacePath> bind_external_path(
        std::string_view a_unverifiedAbsolutePath) noexcept = 0;

    /// @brief 検証済みUser LocatorをこのWorkspace固有のDirectory CapabilityへBindingする
    [[nodiscard]] Result<WorkspaceDirectory> bind_directory(RelativePath a_locator,
                                                            const AssertContext &a_assertContext) const noexcept;

    /// @brief 個別検証済みArea Rootと利用者Locatorを内部Pathへ合成する
    [[nodiscard]] Result<BoundWorkspacePath> bind_path(RelativePath a_areaRoot, RelativePath a_locator,
                                                       const AssertContext &a_assertContext) const noexcept;

    /// @brief 検証済みRoot相対LocatorをこのWorkspace固有の内部PathへBindingする
    [[nodiscard]] Result<BoundWorkspacePath> bind_root_path(RelativePath a_locator,
                                                            const AssertContext &a_assertContext) const noexcept;

    /// @brief 検証済み親とOperation IDから利用者Pathでは表せないHidden Staging Capabilityを発行する
    [[nodiscard]] Result<BoundWorkspacePath> bind_operation_staging_path(
        const BoundWorkspacePath &a_parent, std::string_view a_operationId, std::string_view a_kind,
        const AssertContext &a_assertContext) const noexcept;

    /// @brief 既存Bound Directoryへ個別検証済みChild Locatorを追加する
    [[nodiscard]] Result<BoundWorkspacePath> bind_child_path(const BoundWorkspacePath &a_parent, RelativePath a_child,
                                                             const AssertContext &a_assertContext) const noexcept;

    /// @brief Directory直下を決定的に列挙する
    [[nodiscard]] virtual Result<DirectorySnapshot> list_directory(const WorkspaceDirectory &a_directory,
                                                                   TraversalLimits a_limits) noexcept = 0;

    /// @brief Directory Capabilityが現在も同じRoot内の通常Directoryを指すか検証する
    [[nodiscard]] virtual Result<void> verify_directory(const WorkspaceDirectory &a_directory) noexcept = 0;

    /// @brief Root内Regular Fileを排他的な読取りSnapshotとして上限付きで取得する
    [[nodiscard]] virtual Result<std::vector<std::byte>> read_file_bounded(const BoundWorkspacePath &a_source,
                                                                           std::size_t a_maxBytes) noexcept = 0;

    /// @brief EntryのType、単一Link、Contentを上限付きで決定的にFingerprint化する
    [[nodiscard]] virtual Result<WorkspaceEntryFingerprint> fingerprint_entry(
        const BoundWorkspacePath &a_source, TraversalLimits a_traversalLimits,
        ContentVerificationLimits a_contentLimits) noexcept = 0;

    /// @brief Operation所有Sibling Directoryを経由し、既存Entryを上書きせずDirectoryを公開する
    [[nodiscard]] virtual WorkspaceMutationResult create_directory_new(const BoundWorkspacePath &a_destination,
                                                                       std::string_view a_operationId) noexcept = 0;

    /// @brief Operation所有Temporary Fileを経由し、既存Entryを上書きせずFileをAtomic公開する
    [[nodiscard]] virtual WorkspaceMutationResult create_file_new_atomic(const BoundWorkspacePath &a_destination,
                                                                         std::span<const std::byte> a_bytes,
                                                                         std::string_view a_operationId) noexcept = 0;

    /// @brief 既存Regular FileをOperation所有Temporary FileからAtomic置換する
    [[nodiscard]] virtual WorkspaceMutationResult replace_file_atomic(const BoundWorkspacePath &a_destination,
                                                                      std::span<const std::byte> a_bytes,
                                                                      std::string_view a_operationId) noexcept = 0;

    /// @brief Source Identityを保持した一回の同一Volume RenameでDestinationへ移す
    [[nodiscard]] virtual WorkspaceMutationResult rename_entry(const BoundWorkspacePath &a_source,
                                                               const BoundWorkspacePath &a_destination,
                                                               TraversalLimits a_limits) noexcept = 0;

    /// @brief EntryのIdentityと内容を固定し、Record確定まで保持できるMutation Guardを取得する
    [[nodiscard]] virtual Result<GuardedWorkspaceEntry> guard_entry(
        const BoundWorkspacePath &a_entry, TraversalLimits a_traversalLimits,
        ContentVerificationLimits a_contentLimits) noexcept = 0;

    /// @brief Entryが期待Fingerprintと一致する場合だけMutation Guardを取得する
    [[nodiscard]] virtual Result<std::unique_ptr<WorkspaceEntryMutationGuard>> guard_entry_if_matches(
        const BoundWorkspacePath &a_entry, const WorkspaceEntryFingerprint &a_expected,
        TraversalLimits a_traversalLimits, ContentVerificationLimits a_contentLimits) noexcept = 0;

    /// @brief 取得済みGuardと同じNative Entryを一回の同一Volume Renameで移動する
    [[nodiscard]] virtual WorkspaceMutationResult rename_guarded_entry(
        WorkspaceEntryMutationGuard &a_guard, const BoundWorkspacePath &a_source,
        const BoundWorkspacePath &a_destination) noexcept = 0;

    /// @brief Guard期間中の内容不変を再検証してNative Resourceを解放する
    [[nodiscard]] virtual Result<void> finish_entry_mutation_guard(
        std::unique_ptr<WorkspaceEntryMutationGuard> a_guard) noexcept = 0;

    /// @brief Sourceを保持して検証済みSibling StagingからCreate-new Copyを公開する
    [[nodiscard]] virtual WorkspaceMutationResult copy_entry_new(const BoundWorkspacePath &a_source,
                                                                 const BoundWorkspacePath &a_destination,
                                                                 TraversalLimits a_traversalLimits,
                                                                 ContentVerificationLimits a_contentLimits,
                                                                 std::string_view a_operationId) noexcept = 0;

    /// @brief Identity固定したRegular Fileまたは空Directoryだけを削除する
    [[nodiscard]] virtual WorkspaceMutationResult remove_file_or_empty_directory(
        const BoundWorkspacePath &a_entry) noexcept = 0;

  protected:
    /// @brief 発行可能なRoot相対Path長を固定してCapabilityを初期化する
    explicit WorkspaceFilesystem(std::size_t a_maxBoundPathCharacters) noexcept;

    /// @brief DirectoryがRootまたはこのWorkspaceから発行済みか判定する
    [[nodiscard]] bool owns_directory(const WorkspaceDirectory &a_directory) const noexcept;

    /// @brief 内部PathがこのWorkspaceから発行済みか判定する
    [[nodiscard]] bool owns_path(const BoundWorkspacePath &a_path) const noexcept;

    /// @brief このWorkspaceが発行した内部Pathから親Directory Capabilityを構築する
    [[nodiscard]] Result<WorkspaceDirectory> parent_directory(const BoundWorkspacePath &a_path,
                                                              const AssertContext &a_assertContext) const noexcept;

    /// @brief 親内部Pathへ個別検証済みChild Locatorを合成する
    [[nodiscard]] Result<BoundWorkspacePath> append_path(const WorkspaceDirectory &a_parent, RelativePath a_child,
                                                         const AssertContext &a_assertContext) const noexcept;

  private:
    std::uint64_t m_bindingToken;
    std::size_t m_maxBoundPathCharacters;
};

/// @brief Snapshot EntryをDirectory、File、UnsupportedのPortable順へ整列する
void sort_workspace_entries(std::vector<WorkspaceEntry> &a_entries) noexcept;

/// @brief 正規化済みSnapshotへASCII大小文字非依存Filterと結果上限を適用する
[[nodiscard]] Result<std::vector<WorkspaceEntry>> filter_workspace_snapshot(
    const DirectorySnapshot &a_snapshot, std::string_view a_filter, std::size_t a_maxResults,
    const AssertContext &a_assertContext) noexcept;

/// @brief Root境界内を決定的かつ上限制御付きで再帰検索する
[[nodiscard]] Result<WorkspaceSearchResult> search_workspace(WorkspaceFilesystem &a_filesystem,
                                                             const WorkspaceDirectory &a_startDirectory,
                                                             std::string_view a_filter, TraversalLimits a_limits,
                                                             const AssertContext &a_assertContext) noexcept;
} // namespace cue
