#pragma once

#include <Cue/EditorCore/EditorController.h>
#include <Cue/EditorCore/Error.h>
#include <Cue/Foundation/Result.h>
#include <Cue/ProjectFiles/Service.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::editor_core
{
/// @brief Files Workspaceの列挙、検索、Watcher、Copy検証上限
struct FilesWorkspaceLimits final
{
    TraversalLimits traversal;
    ContentVerificationLimits contentVerification;
    WorkspaceWatchLimits watch;

    /// @brief 全ての下位Service上限が有効か判定する
    [[nodiscard]] bool is_valid() const noexcept;
};

/// @brief 同期Workspace OperationのUI向け終端状態
enum class FilesOperationState : std::uint8_t
{
    Idle,
    InProgress,
    Succeeded,
    Failed,
    ReconciliationRequired
};

/// @brief Native Resourceを持たないProject Area相対のFiles UI状態
class FilesViewModel final
{
  public:
    /// @brief Files ViewModelの複製を禁止する
    FilesViewModel(const FilesViewModel &) = delete;
    /// @brief Files ViewModelのCopy代入を禁止する
    FilesViewModel &operator=(const FilesViewModel &) = delete;
    /// @brief Files ViewModelの所有値を移動する
    FilesViewModel(FilesViewModel &&) noexcept = default;
    /// @brief Files ViewModelの所有値を移動代入する
    FilesViewModel &operator=(FilesViewModel &&) noexcept = default;
    /// @brief Files ViewModelの所有Snapshotを解放する
    ~FilesViewModel() = default;

    /// @brief 最後に完全適用したViewModel Generationを返す
    [[nodiscard]] std::uint64_t generation() const noexcept;
    /// @brief 外部変更または不完全列挙により再走査が必要か返す
    [[nodiscard]] bool is_stale() const noexcept;
    /// @brief Rootと展開済みDirectoryの所有Snapshotを返す
    [[nodiscard]] std::span<const project_files::ProjectFileDirectorySnapshot> directories() const noexcept;
    /// @brief Project Area相対の展開済みDirectory Locatorを返す
    [[nodiscard]] std::span<const std::string> expanded_directories() const noexcept;
    /// @brief 選択中のProject Area相対Locatorを返す
    [[nodiscard]] std::optional<std::string_view> selection() const noexcept;
    /// @brief 現在のASCII大小文字非依存Search Filterを返す
    [[nodiscard]] std::string_view search_filter() const noexcept;
    /// @brief Search中だけ現在の所有結果を返す
    [[nodiscard]] const project_files::ProjectFileSearchResult *try_search_result() const noexcept;
    /// @brief 最後のWorkspace Operation状態を返す
    [[nodiscard]] FilesOperationState operation_state() const noexcept;
    /// @brief 最後にProjectFileServiceが確定したOperation結果を返す
    [[nodiscard]] const project_files::ProjectFileOperationResult *try_last_operation() const noexcept;
    /// @brief 現在表示すべきErrorを返す
    [[nodiscard]] const Error *try_error() const noexcept;
    /// @brief 最後に再構築したProject-local Trash Catalogを返す
    [[nodiscard]] std::span<const project_files::RecoveryEntry> recovery_entries() const noexcept;

  private:
    friend class FilesWorkspaceService;

    /// @brief 空のFiles UI状態を構築する
    FilesViewModel() noexcept = default;

    std::uint64_t m_generation = 0U;
    bool m_isStale = true;
    std::vector<project_files::ProjectFileDirectorySnapshot> m_directories;
    std::vector<std::string> m_expandedDirectories;
    std::optional<std::string> m_selection;
    std::string m_searchFilter;
    std::optional<project_files::ProjectFileSearchResult> m_searchResult;
    FilesOperationState m_operationState = FilesOperationState::Idle;
    std::optional<project_files::ProjectFileOperationResult> m_lastOperation;
    std::optional<Error> m_error;
    std::vector<project_files::RecoveryEntry> m_recoveryEntries;
};

/// @brief ProjectFileServiceとEditorDocument Guardを直列化するOwner Thread Files Service
/// @details Factoryと全Member関数はEditorControllerおよびProjectFileServiceと同じOwner Threadで呼ぶ。
/// EditorControllerとAssertContextの診断依存はこのServiceより長く生存させる。
class FilesWorkspaceService final
{
  private:
    /// @brief Factory構築だけを許可する非公開Key
    struct ConstructionKey final
    {
      private:
        friend class FilesWorkspaceService;

        /// @brief FilesWorkspaceService FactoryだけがKeyを生成する
        ConstructionKey() noexcept = default;
    };

  public:
    /// @brief 必須Dependencyなしの構築を禁止する
    FilesWorkspaceService() = delete;
    /// @brief ServiceとWatcherの一意所有を保つためCopy構築を禁止する
    FilesWorkspaceService(const FilesWorkspaceService &) = delete;
    /// @brief ServiceとWatcherの一意所有を保つためCopy代入を禁止する
    FilesWorkspaceService &operator=(const FilesWorkspaceService &) = delete;
    /// @brief Owner ThreadとController参照を固定するためMove構築を禁止する
    FilesWorkspaceService(FilesWorkspaceService &&) = delete;
    /// @brief Owner ThreadとController参照を固定するためMove代入を禁止する
    FilesWorkspaceService &operator=(FilesWorkspaceService &&) = delete;
    /// @brief Watcherを停止してFiles状態を解放する
    ~FilesWorkspaceService() noexcept;

    /// @brief Project File Service、Editor Controller、上限から初期SnapshotとWatcherを構築する
    /// @param a_projectFiles Factory呼出ThreadをOwnerとするServiceの所有権
    /// @param a_editorController このServiceより長く生存し同じOwner Threadで生成されたController
    /// @param a_limits 全列挙、検証、Watcherの非Zero上限
    /// @param a_assertContext 内部Copyより長くLoggerとFatalHandlerを生存させる診断Context
    [[nodiscard]] static Result<std::unique_ptr<FilesWorkspaceService>> create(
        project_files::ProjectFileService a_projectFiles, EditorController &a_editorController,
        FilesWorkspaceLimits a_limits, const AssertContext &a_assertContext) noexcept;

    /// @brief Factory Passkeyと検証前DependencyからService本体を構築する
    FilesWorkspaceService(ConstructionKey, project_files::ProjectFileService a_projectFiles,
                          EditorController &a_editorController, FilesWorkspaceLimits a_limits,
                          const AssertContext &a_assertContext) noexcept;

    /// @brief 次のMutationまで有効なNative Resource非依存ViewModelを返す
    [[nodiscard]] const FilesViewModel &view_model() const noexcept;
    /// @brief Rootと展開済みDirectoryを再列挙して成功時だけSnapshotを置換する
    [[nodiscard]] Result<void> refresh() noexcept;
    /// @brief Directory展開状態を更新し、展開時は子Snapshotを再列挙する
    [[nodiscard]] Result<void> set_expanded(std::string_view a_locator, bool a_expanded) noexcept;
    /// @brief 現在Snapshotに存在する操作可能Entryを選択する
    [[nodiscard]] Result<void> select(std::string_view a_locator) noexcept;
    /// @brief Files Selectionを空にする
    [[nodiscard]] Result<void> clear_selection() noexcept;
    /// @brief Search Filterを更新し、空でなければProjectFileServiceへBounded Searchを要求する
    [[nodiscard]] Result<void> set_search_filter(std::string_view a_filter) noexcept;

    /// @brief ProjectFileService経由でDirectoryをCreate-newする
    [[nodiscard]] Result<project_files::ProjectFileOperationOutcome> create_directory(
        std::string_view a_destination) noexcept;
    /// @brief ProjectFileService経由で初期Content付きFileをCreate-newする
    [[nodiscard]] Result<project_files::ProjectFileOperationOutcome> create_file(
        std::string_view a_destination, std::span<const std::byte> a_bytes) noexcept;
    /// @brief Open Document Guard確認後に同じ親内でEntryをRenameする
    [[nodiscard]] Result<project_files::ProjectFileOperationOutcome> rename(std::string_view a_source,
                                                                            std::string_view a_destination) noexcept;
    /// @brief Open Document Guard確認後に同じArea内でEntryをMoveする
    [[nodiscard]] Result<project_files::ProjectFileOperationOutcome> move(std::string_view a_source,
                                                                          std::string_view a_destination) noexcept;
    /// @brief Sourceを保持して同じArea内へCreate-new Copyする
    [[nodiscard]] Result<project_files::ProjectFileOperationOutcome> copy(std::string_view a_source,
                                                                          std::string_view a_destination) noexcept;
    /// @brief Open Document Guard確認後にEntryをProject-local Trashへ移す
    [[nodiscard]] Result<project_files::ProjectFileOperationOutcome> delete_entry(std::string_view a_source) noexcept;
    /// @brief Operation IDに対応するTrash Entryを元のArea相対Pathへ復元する
    [[nodiscard]] Result<project_files::ProjectFileOperationOutcome> restore(std::string_view a_operationId) noexcept;

    /// @brief Watcher HintをDrainし、影響Document再検査後にAuthoritative Snapshotを更新する
    [[nodiscard]] Result<bool> poll_external_changes() noexcept;
    /// @brief WatcherをOwner Threadで明示停止するIdempotent終了処理
    [[nodiscard]] Result<void> stop() noexcept;

  private:
    /// @brief Project Area相対文字列を検証済みRelativePathへ変換する
    [[nodiscard]] Result<RelativePath> parse_locator(std::string_view a_locator) noexcept;
    /// @brief Sourceが開いているSceneまたはその親Directoryか検査する
    [[nodiscard]] Result<void> guard_open_documents(const RelativePath &a_source) noexcept;
    /// @brief ProjectFileServiceのOperation結果をViewModelへ適用して影響Directoryを再列挙する
    [[nodiscard]] Result<project_files::ProjectFileOperationOutcome> apply_operation(
        Result<project_files::ProjectFileOperationResult> a_operation) noexcept;
    /// @brief Mutation要求の事前検証失敗をViewModelへ保持して返す
    [[nodiscard]] Result<project_files::ProjectFileOperationOutcome> reject_operation(
        Error a_error, EditorCoreError a_code, std::string_view a_summary) noexcept;
    /// @brief Recovery Catalogを再構築し、成功時だけViewModelへCopyする
    [[nodiscard]] Result<void> refresh_recovery_catalog() noexcept;
    /// @brief 現在SnapshotまたはSearch結果に操作可能Locatorが存在するか判定する
    [[nodiscard]] bool contains_operable_entry(std::string_view a_locator) const noexcept;
    /// @brief 外部変更BatchがScene Locatorまたは親Directoryへ影響するか判定する
    [[nodiscard]] bool affects_document(const WorkspaceChangeBatch &a_batch,
                                        std::string_view a_documentLocator) const noexcept;
    /// @brief ErrorをViewModelへ保持し、Callerへ返す別Errorを生成する
    [[nodiscard]] Error retain_error(Error a_error, EditorCoreError a_code, std::string_view a_summary) noexcept;
    /// @brief Transient Errorを消し、対応Operation結果がないFailed状態をIdleへ戻す
    void dismiss_error() noexcept;
    /// @brief Owner Thread違反を回復可能Errorとして返す
    [[nodiscard]] Result<void> require_owner_thread() const noexcept;
    /// @brief Allocation失敗をFatal境界へ変換する
    [[noreturn]] void terminate_allocation() const noexcept;

    project_files::ProjectFileService m_projectFiles;
    EditorController *m_editorController;
    FilesWorkspaceLimits m_limits;
    AssertContext m_assertContext;
    std::thread::id m_ownerThread;
    std::unique_ptr<WorkspaceWatcher> m_watcher;
    std::optional<WorkspaceChangeBatch> m_pendingExternalChanges;
    FilesViewModel m_view;
};
} // namespace cue::editor_core
