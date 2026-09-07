#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/IO/RelativePath.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cue
{
/// @brief File Watcher内部QueueとBatch確定時間の非Zero上限
struct WorkspaceWatchLimits final
{
    std::size_t maxQueuedEvents = 0U;
    std::size_t maxQueuedNameBytes = 0U;
    std::size_t maxBatchChanges = 0U;
    std::uint32_t debounceMilliseconds = 0U;
    std::uint32_t maximumBatchDelayMilliseconds = 0U;

    /// @brief 全上限が非Zeroで最大待機時間がDebounce以上か判定する
    [[nodiscard]] bool is_valid() const noexcept;
};

/// @brief 再Query対象となるWorkspace Entry変更のPortable分類
enum class WorkspaceChangeHintKind : std::uint8_t
{
    Created,
    Modified,
    Removed,
    Renamed
};

/// @brief Watcher Batchが再Query可能か全再走査を要するか表す
enum class WorkspaceChangeBatchState : std::uint8_t
{
    ChangesAvailable,
    RescanRequired
};

/// @brief Watcherが個別Eventを安全に公開できない理由
enum class WorkspaceWatchDiagnosticCode : std::uint8_t
{
    NativeBufferOverflow,
    QueueOverflow,
    InvalidName,
    MalformedNotification,
    IncompleteRename,
    ChangeSequenceConflict,
    WatchedDirectoryChanged,
    NativeFailure
};

/// @brief Eventが正本ではないことを明示するRoot相対再Query Hint
struct WorkspaceChangeHint final
{
    WorkspaceChangeHintKind kind;
    RelativePath locator;
    std::optional<RelativePath> previousLocator;

    /// @brief 正規化済み変更種別と現在Path、任意のRename元Pathを所有する
    WorkspaceChangeHint(WorkspaceChangeHintKind a_kind, RelativePath a_locator,
                        std::optional<RelativePath> a_previousLocator = {}) noexcept;
};

/// @brief Rescan理由とNative診断をPortableに保持する
struct WorkspaceWatchDiagnostic final
{
    WorkspaceWatchDiagnosticCode code = WorkspaceWatchDiagnosticCode::NativeFailure;
    std::string displayName;
    std::int64_t nativeCode = 0;
};

/// @brief 一回のDrainで確定した変更HintまたはRescan要求
struct WorkspaceChangeBatch final
{
    std::uint64_t generation = 0U;
    WorkspaceChangeBatchState state = WorkspaceChangeBatchState::ChangesAvailable;
    std::vector<WorkspaceChangeHint> changes;
    std::vector<WorkspaceWatchDiagnostic> diagnostics;
};

/// @brief Native File WatcherのLifetime、非Blocking Drain、停止を表す
///
/// Eventは再QueryのHintに限り、Filesystem状態の正本ではない。
/// drain_changesとstopは生成Thread限定で、Destructorだけは任意Threadから停止できる。
/// Factoryが使用するAssertContext内のLoggerとFatalHandlerはWatcherより長く生存しなければならない。
/// 生成元WorkspaceFilesystemまたはProjectFileServiceはWatcherより先に破棄できる。
/// Watcher生存中はRootから監視DirectoryまでのLocationを固定し、そのDirectory chainのRenameと削除を拒否する。
/// Recoverable overflowはRescanRequired後も監視を継続し、NativeFailureまたは監視Directory移動後は停止する。
class WorkspaceWatcher
{
  public:
    /// @brief Native Resourceの一意所有を保つためCopy構築を禁止する
    WorkspaceWatcher(const WorkspaceWatcher &) = delete;
    /// @brief Native Resourceの一意所有を保つためCopy代入を禁止する
    WorkspaceWatcher &operator=(const WorkspaceWatcher &) = delete;
    /// @brief Platform実装が所有するWorkerとHandleを停止して解放する
    virtual ~WorkspaceWatcher() = default;

    /// @brief 生成ThreadでDebounce済みBatchを非Blocking取得し、未確定なら空Optionalを返す
    [[nodiscard]] virtual Result<std::optional<WorkspaceChangeBatch>> drain_changes() noexcept = 0;
    /// @brief 生成ThreadでPending IOを取消してWorker終了まで待つIdempotent停止を行う
    [[nodiscard]] virtual Result<void> stop() noexcept = 0;
    /// @brief Watcherが新しいNative通知を受理できる状態か返す
    [[nodiscard]] virtual bool is_running() const noexcept = 0;

  protected:
    /// @brief Platform実装だけがWatcher基底を構築できる状態にする
    WorkspaceWatcher() noexcept = default;
};
} // namespace cue
