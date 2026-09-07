#include <Cue/IO/Windows/WindowsWorkspaceFilesystem.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/Error.h>

#include <Windows.h>
#include <winioctl.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_maxWindowsPathLength = 32767U;
constexpr cue::TraversalLimits k_windowsHardLimits{64U, 1'000'000U, 1'000'000U, 64U * 1024U * 1024U};
constexpr cue::ContentVerificationLimits k_windowsContentHardLimits{256ULL * 1024ULL * 1024ULL,
                                                                    1024ULL * 1024ULL * 1024ULL};

/// @brief Win32 Handleを一意所有して全終了経路でCloseする
class UniqueHandle final
{
  public:
    /// @brief 無効Handleを所有しない状態で生成する
    UniqueHandle() noexcept = default;
    /// @brief Native HandleのClose責務を受け取る
    explicit UniqueHandle(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    /// @brief Handleの二重Closeを防ぐためCopy構築を禁止する
    UniqueHandle(const UniqueHandle &) = delete;
    /// @brief Handleの二重Closeを防ぐためCopy代入を禁止する
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    /// @brief Native Handleの所有権を移動する
    UniqueHandle(UniqueHandle &&a_other) noexcept : m_handle(a_other.release())
    {
    }
    /// @brief 現在のHandleを閉じて新しい所有権を移動する
    UniqueHandle &operator=(UniqueHandle &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset(a_other.release());
        }
        return *this;
    }
    /// @brief 所有Handleを閉じる
    ~UniqueHandle()
    {
        reset();
    }

    /// @brief Native APIへ渡すHandleを返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }

    /// @brief HandleがNative API呼出しに使用可能か判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

    /// @brief CloseせずHandle所有権を返す
    [[nodiscard]] HANDLE release() noexcept
    {
        HANDLE handle = m_handle;
        m_handle = INVALID_HANDLE_VALUE;
        return handle;
    }

    /// @brief 現在のHandleを閉じて指定Handleを所有する
    void reset(HANDLE a_handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (is_valid())
        {
            CloseHandle(m_handle);
        }
        m_handle = a_handle;
    }

  private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

/// @brief Workspace File置換をCueEngine Process間で直列化するNamed Mutex所有権
class WorkspaceReplacementMutexOwnership final
{
  public:
    /// @brief 取得済みMutex Handleの所有権を受け取る
    explicit WorkspaceReplacementMutexOwnership(UniqueHandle a_handle) noexcept : m_handle(std::move(a_handle))
    {
    }
    /// @brief Mutex所有権の重複を防ぐためCopy構築を禁止する
    WorkspaceReplacementMutexOwnership(const WorkspaceReplacementMutexOwnership &) = delete;
    /// @brief Mutex所有権の重複を防ぐためCopy代入を禁止する
    WorkspaceReplacementMutexOwnership &operator=(const WorkspaceReplacementMutexOwnership &) = delete;
    /// @brief 取得済みMutex Handleと解放責務を移動する
    WorkspaceReplacementMutexOwnership(WorkspaceReplacementMutexOwnership &&) noexcept = default;
    /// @brief 現在のMutexを解放して取得済み所有権を移動する
    WorkspaceReplacementMutexOwnership &operator=(WorkspaceReplacementMutexOwnership &&a_other) noexcept
    {
        if (this != &a_other)
        {
            release();
            m_handle = std::move(a_other.m_handle);
        }
        return *this;
    }
    /// @brief Mutexを解放してHandleを閉じる
    ~WorkspaceReplacementMutexOwnership()
    {
        release();
    }

  private:
    /// @brief 所有中のNamed Mutexを一度だけ解放する
    void release() noexcept
    {
        if (m_handle.is_valid())
        {
            ReleaseMutex(m_handle.get());
            m_handle.reset();
        }
    }

    UniqueHandle m_handle;
};

/// @brief noexcept処理中のAllocation失敗をProject Fatal Policyへ接続する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Windows workspace filesystem allocation failed");
    std::abort();
}

/// @brief Win32 ErrorをPortable IO分類へ変換する
[[nodiscard]] cue::IoError classify_windows_error(DWORD a_code) noexcept
{
    switch (a_code)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return cue::IoError::NotFound;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return cue::IoError::AlreadyExists;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return cue::IoError::PermissionDenied;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return cue::IoError::CapacityExceeded;
    default:
        return cue::IoError::IoFailure;
    }
}

/// @brief Win32 ErrorをNative Code付きErrorへ変換する
[[nodiscard]] cue::Error make_windows_error(const cue::AssertContext &a_assertContext, DWORD a_code,
                                            std::string_view a_summary) noexcept
{
    return cue::make_io_error(a_assertContext, classify_windows_error(a_code), a_summary,
                              static_cast<std::int64_t>(a_code));
}

/// @brief CueEngineが行うWorkspace File置換をProcess境界で直列化する
[[nodiscard]] cue::Result<WorkspaceReplacementMutexOwnership> acquire_workspace_replacement_mutex(
    const cue::AssertContext &a_assertContext) noexcept
{
    constexpr wchar_t k_mutexName[] = L"Local\\CueEngine.WorkspaceFileReplacement.v1";
    UniqueHandle mutex(CreateMutexW(nullptr, FALSE, k_mutexName));
    if (!mutex.is_valid())
    {
        return cue::Result<WorkspaceReplacementMutexOwnership>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace replacement mutex creation failed"));
    }
    const DWORD wait = WaitForSingleObject(mutex.get(), INFINITE);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    {
        return cue::Result<WorkspaceReplacementMutexOwnership>::failure(
            make_windows_error(a_assertContext, wait == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA,
                               "Workspace replacement mutex acquisition failed"));
    }
    return cue::Result<WorkspaceReplacementMutexOwnership>::success(
        WorkspaceReplacementMutexOwnership(std::move(mutex)));
}

/// @brief UTF-8を厳密なUTF-16へ変換する
[[nodiscard]] cue::Result<std::wstring> to_utf16(std::string_view a_text,
                                                 const cue::AssertContext &a_assertContext) noexcept
{
    std::wstring converted;
    const cue::WindowsUtfConversionResult conversion =
        cue::convert_utf8_to_windows_utf16(a_text, converted, a_assertContext.fatal_handler());
    if (conversion.status != cue::WindowsUtfConversionStatus::Success)
    {
        const cue::IoError code = conversion.status == cue::WindowsUtfConversionStatus::InputTooLong
                                      ? cue::IoError::CapacityExceeded
                                      : cue::IoError::InvalidPath;
        return cue::Result<std::wstring>::failure(
            cue::make_io_error(a_assertContext, code, "Workspace root UTF-8 conversion failed", conversion.nativeCode));
    }
    return cue::Result<std::wstring>::success(std::move(converted));
}

/// @brief UTF-16 Entry Nameを厳密なUTF-8へ変換する
[[nodiscard]] cue::Result<std::string> to_utf8(std::wstring_view a_text,
                                               const cue::AssertContext &a_assertContext) noexcept
{
    std::string converted;
    const cue::WindowsUtfConversionResult conversion =
        cue::convert_windows_utf16_to_utf8(a_text, converted, a_assertContext.fatal_handler());
    if (conversion.status != cue::WindowsUtfConversionStatus::Success)
    {
        const cue::IoError code = conversion.status == cue::WindowsUtfConversionStatus::InputTooLong
                                      ? cue::IoError::CapacityExceeded
                                      : cue::IoError::InvalidPath;
        return cue::Result<std::string>::failure(cue::make_io_error(
            a_assertContext, code, "Workspace entry UTF-16 conversion failed", conversion.nativeCode));
    }
    return cue::Result<std::string>::success(std::move(converted));
}

/// @brief ReadDirectoryChangesW一件をWorkerからOwner Drainへ渡す
struct RawWorkspaceChange final
{
    DWORD action = 0U;
    std::wstring name;
};

/// @brief Root境界内のReadDirectoryChangesWを所有しPortable Batchへ変換する
class WindowsWorkspaceWatcher final : public cue::WorkspaceWatcher
{
  public:
    /// @brief 検証済みDirectory、Event、上限を所有して未開始Watcherを生成する
    WindowsWorkspaceWatcher(UniqueHandle a_directory, std::vector<UniqueHandle> a_locationLocks,
                            std::wstring a_expectedPath, UniqueHandle a_changeEvent, UniqueHandle a_stopEvent,
                            cue::WorkspaceWatchLimits a_limits, const cue::AssertContext &a_assertContext) noexcept
        : m_directory(std::move(a_directory)), m_locationLocks(std::move(a_locationLocks)),
          m_changeEvent(std::move(a_changeEvent)), m_stopEvent(std::move(a_stopEvent)),
          m_expectedPath(std::move(a_expectedPath)), m_limits(a_limits), m_assertContext(a_assertContext),
          m_ownerThread(std::this_thread::get_id())
    {
        m_overlapped.hEvent = m_changeEvent.get();
    }
    /// @brief WorkerとNative Handleの一意所有を保つためCopy構築を禁止する
    WindowsWorkspaceWatcher(const WindowsWorkspaceWatcher &) = delete;
    /// @brief WorkerとNative Handleの一意所有を保つためCopy代入を禁止する
    WindowsWorkspaceWatcher &operator=(const WindowsWorkspaceWatcher &) = delete;
    /// @brief Object AddressをWorkerへ固定するためMove構築を禁止する
    WindowsWorkspaceWatcher(WindowsWorkspaceWatcher &&) = delete;
    /// @brief Object AddressをWorkerへ固定するためMove代入を禁止する
    WindowsWorkspaceWatcher &operator=(WindowsWorkspaceWatcher &&) = delete;
    /// @brief Pending IOを取消しWorker終了後に全Handleを解放する
    ~WindowsWorkspaceWatcher() override
    {
        (void)stop_internal();
    }

    /// @brief 最初のOverlapped監視を発行してWorkerを開始する
    [[nodiscard]] cue::Result<void> start() noexcept
    {
        const DWORD issueCode = issue_read();
        if (issueCode != ERROR_SUCCESS)
        {
            return cue::Result<void>::failure(
                make_windows_error(m_assertContext, issueCode, "Workspace watcher start failed"));
        }
        m_running.store(true, std::memory_order_release);
        try
        {
            /// @brief Native変更完了と停止Eventを待ってBounded Queueへ転送する
            m_worker = std::thread([this]() noexcept { worker_main(); });
        }
        catch (...)
        {
            m_running.store(false, std::memory_order_release);
            CancelIoEx(m_directory.get(), &m_overlapped);
            DWORD transferred = 0U;
            GetOverlappedResult(m_directory.get(), &m_overlapped, &transferred, TRUE);
            m_pending = false;
            return cue::Result<void>::failure(cue::make_io_error(m_assertContext, cue::IoError::IoFailure,
                                                                 "Workspace watcher worker could not be created"));
        }
        return cue::Result<void>::success();
    }

    /// @brief Debounce済みRaw EventをPortable Hintへ変換して非Blockingで返す
    [[nodiscard]] cue::Result<std::optional<cue::WorkspaceChangeBatch>> drain_changes() noexcept override
    {
        if (std::this_thread::get_id() != m_ownerThread)
        {
            return cue::Result<std::optional<cue::WorkspaceChangeBatch>>::failure(cue::make_io_error(
                m_assertContext, cue::IoError::PreconditionFailed, "Workspace watcher drain used another thread"));
        }

        std::deque<RawWorkspaceChange> rawChanges;
        std::vector<cue::WorkspaceWatchDiagnostic> diagnostics;
        bool rescanRequired = false;
        {
            std::lock_guard lock(m_mutex);
            if (!m_batchPending)
            {
                return cue::Result<std::optional<cue::WorkspaceChangeBatch>>::success(std::nullopt);
            }
            const auto now = std::chrono::steady_clock::now();
            const bool debounceElapsed = now - m_lastChange >= std::chrono::milliseconds(m_limits.debounceMilliseconds);
            const bool maximumDelayElapsed =
                now - m_firstChange >= std::chrono::milliseconds(m_limits.maximumBatchDelayMilliseconds);
            if (!m_rescanRequired && !debounceElapsed && !maximumDelayElapsed)
            {
                return cue::Result<std::optional<cue::WorkspaceChangeBatch>>::success(std::nullopt);
            }
            rawChanges.swap(m_rawChanges);
            diagnostics.swap(m_diagnostics);
            rescanRequired = m_rescanRequired;
            m_queuedNameBytes = 0U;
            m_batchPending = false;
            m_rescanRequired = false;
        }

        cue::WorkspaceChangeBatch batch;
        batch.generation = m_nextGeneration++;
        batch.state = rescanRequired ? cue::WorkspaceChangeBatchState::RescanRequired
                                     : cue::WorkspaceChangeBatchState::ChangesAvailable;
        batch.diagnostics = std::move(diagnostics);
        std::optional<cue::RelativePath> pendingRename;

        /// @brief 個別Hintへ安全に変換できないBatchを再走査要求へ昇格する
        const auto requireRescan =
            [&](cue::WorkspaceWatchDiagnosticCode a_code, std::string a_name, std::int64_t a_nativeCode) noexcept
        {
            batch.state = cue::WorkspaceChangeBatchState::RescanRequired;
            batch.changes.clear();
            try
            {
                batch.diagnostics.push_back(cue::WorkspaceWatchDiagnostic{a_code, std::move(a_name), a_nativeCode});
            }
            catch (...)
            {
                terminate_allocation(m_assertContext);
            }
        };
        /// @brief 同一Pathの単純な連続変更を一件へ集約する
        const auto appendChange = [&](cue::WorkspaceChangeHint a_change) noexcept
        {
            const std::string_view path = a_change.locator.text();
            for (auto iterator = batch.changes.begin(); iterator != batch.changes.end(); ++iterator)
            {
                if (iterator->locator.text() != path)
                {
                    continue;
                }
                if ((iterator->kind == cue::WorkspaceChangeHintKind::Created &&
                     a_change.kind == cue::WorkspaceChangeHintKind::Modified) ||
                    (iterator->kind == cue::WorkspaceChangeHintKind::Modified &&
                     a_change.kind == cue::WorkspaceChangeHintKind::Modified) ||
                    (iterator->kind == cue::WorkspaceChangeHintKind::Renamed &&
                     a_change.kind == cue::WorkspaceChangeHintKind::Modified))
                {
                    return;
                }
                if (iterator->kind == cue::WorkspaceChangeHintKind::Created &&
                    a_change.kind == cue::WorkspaceChangeHintKind::Removed)
                {
                    batch.changes.erase(iterator);
                    return;
                }
                if (iterator->kind == cue::WorkspaceChangeHintKind::Removed &&
                    a_change.kind == cue::WorkspaceChangeHintKind::Created)
                {
                    iterator->kind = cue::WorkspaceChangeHintKind::Modified;
                    iterator->previousLocator.reset();
                    return;
                }
                if (iterator->kind == cue::WorkspaceChangeHintKind::Renamed &&
                    a_change.kind == cue::WorkspaceChangeHintKind::Removed)
                {
                    requireRescan(cue::WorkspaceWatchDiagnosticCode::ChangeSequenceConflict, {}, 0);
                    return;
                }
                if (iterator->kind == cue::WorkspaceChangeHintKind::Renamed &&
                    a_change.kind == cue::WorkspaceChangeHintKind::Renamed)
                {
                    if (iterator->previousLocator.has_value() && a_change.previousLocator.has_value() &&
                        iterator->previousLocator->text() == a_change.previousLocator->text())
                    {
                        return;
                    }
                    requireRescan(cue::WorkspaceWatchDiagnosticCode::ChangeSequenceConflict, {}, 0);
                    return;
                }
                *iterator = std::move(a_change);
                return;
            }
            try
            {
                batch.changes.push_back(std::move(a_change));
            }
            catch (...)
            {
                terminate_allocation(m_assertContext);
            }
        };

        if (!rescanRequired)
        {
            for (RawWorkspaceChange &raw : rawChanges)
            {
                cue::Result<std::string> utf8 = to_utf8(raw.name, m_assertContext);
                if (!utf8)
                {
                    requireRescan(cue::WorkspaceWatchDiagnosticCode::InvalidName, {}, 0);
                    break;
                }
                std::replace(utf8.try_value()->begin(), utf8.try_value()->end(), '\\', '/');
                cue::Result<cue::RelativePath> path = cue::RelativePath::parse(*utf8.try_value(), m_assertContext);
                if (!path)
                {
                    requireRescan(cue::WorkspaceWatchDiagnosticCode::InvalidName, std::move(*utf8.try_value()), 0);
                    break;
                }

                if (raw.action == FILE_ACTION_RENAMED_OLD_NAME)
                {
                    if (pendingRename.has_value())
                    {
                        requireRescan(cue::WorkspaceWatchDiagnosticCode::IncompleteRename, {}, 0);
                        break;
                    }
                    pendingRename.emplace(std::move(*path.try_value()));
                    continue;
                }
                if (raw.action == FILE_ACTION_RENAMED_NEW_NAME)
                {
                    if (!pendingRename.has_value())
                    {
                        requireRescan(cue::WorkspaceWatchDiagnosticCode::IncompleteRename, {}, 0);
                        break;
                    }
                    const std::string_view previousText = pendingRename->text();
                    bool foldedCreation = false;
                    for (cue::WorkspaceChangeHint &change : batch.changes)
                    {
                        if (change.locator.text() == previousText &&
                            change.kind == cue::WorkspaceChangeHintKind::Created)
                        {
                            change.locator = std::move(*path.try_value());
                            foldedCreation = true;
                            break;
                        }
                    }
                    if (!foldedCreation)
                    {
                        appendChange(cue::WorkspaceChangeHint(cue::WorkspaceChangeHintKind::Renamed,
                                                              std::move(*path.try_value()), std::move(pendingRename)));
                    }
                    pendingRename.reset();
                }
                else
                {
                    cue::WorkspaceChangeHintKind kind = cue::WorkspaceChangeHintKind::Modified;
                    if (raw.action == FILE_ACTION_ADDED)
                    {
                        kind = cue::WorkspaceChangeHintKind::Created;
                    }
                    else if (raw.action == FILE_ACTION_REMOVED)
                    {
                        kind = cue::WorkspaceChangeHintKind::Removed;
                    }
                    appendChange(cue::WorkspaceChangeHint(kind, std::move(*path.try_value())));
                }

                if (batch.state == cue::WorkspaceChangeBatchState::RescanRequired)
                {
                    break;
                }

                if (batch.changes.size() > m_limits.maxBatchChanges)
                {
                    requireRescan(cue::WorkspaceWatchDiagnosticCode::QueueOverflow, {}, 0);
                    break;
                }
            }
            if (batch.state != cue::WorkspaceChangeBatchState::RescanRequired && pendingRename.has_value())
            {
                requireRescan(cue::WorkspaceWatchDiagnosticCode::IncompleteRename, {}, 0);
            }
        }

        std::optional<cue::WorkspaceChangeBatch> result;
        try
        {
            result.emplace(std::move(batch));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        return cue::Result<std::optional<cue::WorkspaceChangeBatch>>::success(std::move(result));
    }

    /// @brief Owner ThreadからPending IOを取消しWorkerとHandleを解放する
    [[nodiscard]] cue::Result<void> stop() noexcept override
    {
        if (std::this_thread::get_id() != m_ownerThread)
        {
            return cue::Result<void>::failure(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                                 "Workspace watcher stop used another thread"));
        }
        const DWORD stopCode = stop_internal();
        if (stopCode != ERROR_SUCCESS)
        {
            return cue::Result<void>::failure(
                make_windows_error(m_assertContext, stopCode, "Workspace watcher stop signal failed"));
        }
        return cue::Result<void>::success();
    }

    /// @brief Workerが新しいNative通知を受理できる状態か返す
    [[nodiscard]] bool is_running() const noexcept override
    {
        return m_running.load(std::memory_order_acquire);
    }

  private:
    /// @brief 次のOverlapped ReadDirectoryChangesWを発行する
    [[nodiscard]] DWORD issue_read() noexcept
    {
        if (ResetEvent(m_changeEvent.get()) == FALSE)
        {
            return GetLastError();
        }
        m_overlapped = {};
        m_overlapped.hEvent = m_changeEvent.get();
        DWORD immediateBytes = 0U;
        if (ReadDirectoryChangesW(m_directory.get(), m_buffer.data(), static_cast<DWORD>(sizeof(m_buffer)), TRUE,
                                  FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                      FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                                      FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                                  &immediateBytes, &m_overlapped, nullptr) == FALSE)
        {
            return GetLastError();
        }
        m_pending = true;
        return ERROR_SUCCESS;
    }

    /// @brief 監視Directoryが生成時と同じNative Pathに存在するか再検証する
    [[nodiscard]] DWORD verify_location(bool &a_isSameLocation) noexcept
    {
        a_isSameLocation = false;
        const DWORD pathLength =
            GetFinalPathNameByHandleW(m_directory.get(), nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (pathLength == 0U)
        {
            return GetLastError();
        }
        std::wstring path;
        try
        {
            path.resize(pathLength);
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        const DWORD pathWritten = GetFinalPathNameByHandleW(m_directory.get(), path.data(), pathLength,
                                                            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (pathWritten == 0U || pathWritten >= pathLength)
        {
            return GetLastError();
        }
        path.resize(pathWritten);
        a_isSameLocation = path.size() == m_expectedPath.size() &&
                           CompareStringOrdinal(path.data(), static_cast<int>(path.size()), m_expectedPath.data(),
                                                static_cast<int>(m_expectedPath.size()), TRUE) == CSTR_EQUAL;
        return ERROR_SUCCESS;
    }

    /// @brief Raw Queueを破棄し最初のRescan理由と任意の終端理由だけを保持する
    void mark_rescan_locked(cue::WorkspaceWatchDiagnosticCode a_code, std::int64_t a_nativeCode) noexcept
    {
        bool appendDiagnostic = !m_rescanRequired;
        const bool terminalDiagnostic = a_code == cue::WorkspaceWatchDiagnosticCode::WatchedDirectoryChanged ||
                                        a_code == cue::WorkspaceWatchDiagnosticCode::NativeFailure;
        if (!appendDiagnostic && terminalDiagnostic)
        {
            appendDiagnostic = true;
            for (const cue::WorkspaceWatchDiagnostic &diagnostic : m_diagnostics)
            {
                if (diagnostic.code == a_code)
                {
                    appendDiagnostic = false;
                    break;
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (!m_batchPending)
        {
            m_firstChange = now;
        }
        m_lastChange = now;
        m_batchPending = true;
        m_rescanRequired = true;
        m_rawChanges.clear();
        m_queuedNameBytes = 0U;
        if (!appendDiagnostic)
        {
            return;
        }
        try
        {
            m_diagnostics.push_back(cue::WorkspaceWatchDiagnostic{a_code, {}, a_nativeCode});
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
    }

    /// @brief 一件のNative ActionをBounded Queueへ追加する
    void enqueue_raw(DWORD a_action, std::wstring_view a_name) noexcept
    {
        std::lock_guard lock(m_mutex);
        if (m_rescanRequired)
        {
            return;
        }
        const std::size_t nameBytes = a_name.size() * sizeof(wchar_t);
        if (m_rawChanges.size() >= m_limits.maxQueuedEvents ||
            nameBytes > m_limits.maxQueuedNameBytes - std::min(m_queuedNameBytes, m_limits.maxQueuedNameBytes))
        {
            mark_rescan_locked(cue::WorkspaceWatchDiagnosticCode::QueueOverflow, 0);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!m_batchPending)
        {
            m_firstChange = now;
        }
        m_lastChange = now;
        m_batchPending = true;
        try
        {
            m_rawChanges.push_back(RawWorkspaceChange{a_action, std::wstring(a_name)});
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        m_queuedNameBytes += nameBytes;
    }

    /// @brief Native通知Bufferを境界検証してRaw Queueへ転送する
    void enqueue_buffer(DWORD a_bytes) noexcept
    {
        if (a_bytes == 0U)
        {
            std::lock_guard lock(m_mutex);
            mark_rescan_locked(cue::WorkspaceWatchDiagnosticCode::NativeBufferOverflow, 0);
            return;
        }

        constexpr std::size_t k_headerBytes = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        std::size_t offset = 0U;
        while (offset < a_bytes)
        {
            const std::size_t remaining = static_cast<std::size_t>(a_bytes) - offset;
            if (remaining < k_headerBytes)
            {
                std::lock_guard lock(m_mutex);
                mark_rescan_locked(cue::WorkspaceWatchDiagnosticCode::MalformedNotification, 0);
                return;
            }
            const auto *information = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(
                reinterpret_cast<const std::byte *>(m_buffer.data()) + offset);
            const std::size_t nameBytes = information->FileNameLength;
            if (nameBytes == 0U || nameBytes % sizeof(wchar_t) != 0U || k_headerBytes + nameBytes > remaining)
            {
                std::lock_guard lock(m_mutex);
                mark_rescan_locked(cue::WorkspaceWatchDiagnosticCode::MalformedNotification, 0);
                return;
            }
            const bool knownAction =
                information->Action == FILE_ACTION_ADDED || information->Action == FILE_ACTION_REMOVED ||
                information->Action == FILE_ACTION_MODIFIED || information->Action == FILE_ACTION_RENAMED_OLD_NAME ||
                information->Action == FILE_ACTION_RENAMED_NEW_NAME;
            if (!knownAction)
            {
                std::lock_guard lock(m_mutex);
                mark_rescan_locked(cue::WorkspaceWatchDiagnosticCode::MalformedNotification, 0);
                return;
            }
            enqueue_raw(information->Action, std::wstring_view(information->FileName, nameBytes / sizeof(wchar_t)));
            if (information->NextEntryOffset == 0U)
            {
                return;
            }
            if (information->NextEntryOffset < k_headerBytes + nameBytes || information->NextEntryOffset > remaining ||
                information->NextEntryOffset % sizeof(DWORD) != 0U)
            {
                std::lock_guard lock(m_mutex);
                mark_rescan_locked(cue::WorkspaceWatchDiagnosticCode::MalformedNotification, 0);
                return;
            }
            offset += information->NextEntryOffset;
        }
    }

    /// @brief Stop Eventまたは変更完了まで待ち、継続監視を発行する
    void worker_main() noexcept
    {
        const std::array<HANDLE, 2U> events{m_stopEvent.get(), m_changeEvent.get()};
        while (true)
        {
            const DWORD wait =
                WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0)
            {
                if (m_pending)
                {
                    CancelIoEx(m_directory.get(), &m_overlapped);
                    DWORD transferred = 0U;
                    GetOverlappedResult(m_directory.get(), &m_overlapped, &transferred, TRUE);
                    m_pending = false;
                }
                m_running.store(false, std::memory_order_release);
                return;
            }
            if (wait != WAIT_OBJECT_0 + 1U)
            {
                std::lock_guard lock(m_mutex);
                mark_rescan_locked(cue::WorkspaceWatchDiagnosticCode::NativeFailure,
                                   wait == WAIT_FAILED ? static_cast<std::int64_t>(GetLastError())
                                                       : static_cast<std::int64_t>(ERROR_INVALID_DATA));
                m_running.store(false, std::memory_order_release);
                return;
            }

            DWORD transferred = 0U;
            if (GetOverlappedResult(m_directory.get(), &m_overlapped, &transferred, FALSE) == FALSE)
            {
                const DWORD code = GetLastError();
                m_pending = false;
                {
                    std::lock_guard lock(m_mutex);
                    mark_rescan_locked(code == ERROR_NOTIFY_ENUM_DIR
                                           ? cue::WorkspaceWatchDiagnosticCode::NativeBufferOverflow
                                           : cue::WorkspaceWatchDiagnosticCode::NativeFailure,
                                       static_cast<std::int64_t>(code));
                }
                if (code == ERROR_NOTIFY_ENUM_DIR)
                {
                    const DWORD issueCode = issue_read();
                    if (issueCode == ERROR_SUCCESS)
                    {
                        continue;
                    }
                    std::lock_guard lock(m_mutex);
                    mark_rescan_locked(cue::WorkspaceWatchDiagnosticCode::NativeFailure,
                                       static_cast<std::int64_t>(issueCode));
                }
                m_running.store(false, std::memory_order_release);
                return;
            }
            m_pending = false;
            bool isSameLocation = false;
            const DWORD locationCode = verify_location(isSameLocation);
            if (locationCode != ERROR_SUCCESS || !isSameLocation)
            {
                std::lock_guard lock(m_mutex);
                mark_rescan_locked(locationCode == ERROR_SUCCESS
                                       ? cue::WorkspaceWatchDiagnosticCode::WatchedDirectoryChanged
                                       : cue::WorkspaceWatchDiagnosticCode::NativeFailure,
                                   static_cast<std::int64_t>(locationCode));
                m_running.store(false, std::memory_order_release);
                return;
            }
            enqueue_buffer(transferred);
            const DWORD issueCode = issue_read();
            if (issueCode != ERROR_SUCCESS)
            {
                std::lock_guard lock(m_mutex);
                mark_rescan_locked(cue::WorkspaceWatchDiagnosticCode::NativeFailure,
                                   static_cast<std::int64_t>(issueCode));
                m_running.store(false, std::memory_order_release);
                return;
            }
        }
    }

    /// @brief 任意ThreadのDestructorからもWorkerを確実に停止する
    [[nodiscard]] DWORD stop_internal() noexcept
    {
        DWORD stopCode = ERROR_SUCCESS;
        if (m_worker.joinable())
        {
            if (SetEvent(m_stopEvent.get()) == FALSE)
            {
                stopCode = GetLastError();
                CancelIoEx(m_directory.get(), &m_overlapped);
            }
            m_worker.join();
        }
        m_running.store(false, std::memory_order_release);
        {
            std::lock_guard lock(m_mutex);
            m_rawChanges.clear();
            m_diagnostics.clear();
            m_queuedNameBytes = 0U;
            m_batchPending = false;
            m_rescanRequired = false;
        }
        m_directory.reset();
        m_locationLocks.clear();
        m_changeEvent.reset();
        m_stopEvent.reset();
        return stopCode;
    }

    UniqueHandle m_directory;
    std::vector<UniqueHandle> m_locationLocks;
    UniqueHandle m_changeEvent;
    UniqueHandle m_stopEvent;
    std::wstring m_expectedPath;
    cue::WorkspaceWatchLimits m_limits;
    cue::AssertContext m_assertContext;
    std::thread::id m_ownerThread;
    std::thread m_worker;
    std::atomic_bool m_running{false};
    std::array<DWORD, (64U * 1024U) / sizeof(DWORD)> m_buffer{};
    OVERLAPPED m_overlapped{};
    bool m_pending = false;
    std::mutex m_mutex;
    std::deque<RawWorkspaceChange> m_rawChanges;
    std::vector<cue::WorkspaceWatchDiagnostic> m_diagnostics;
    std::size_t m_queuedNameBytes = 0U;
    std::chrono::steady_clock::time_point m_firstChange{};
    std::chrono::steady_clock::time_point m_lastChange{};
    std::uint64_t m_nextGeneration = 1U;
    bool m_batchPending = false;
    bool m_rescanRequired = false;
};

/// @brief Pin済みDirectoryと同じIdentityを持つ再帰Watcherを生成する
[[nodiscard]] cue::Result<std::unique_ptr<cue::WorkspaceWatcher>> create_windows_workspace_watcher(
    std::span<const HANDLE> a_expectedChain, std::wstring_view a_rootPath, std::string_view a_relativeDirectory,
    cue::WorkspaceWatchLimits a_limits, const cue::AssertContext &a_assertContext) noexcept;

/// @brief Absolute Windows PathをExtended Path表現へ変換する
[[nodiscard]] cue::Result<std::wstring> make_extended_path(std::wstring a_path,
                                                           const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        for (wchar_t &character : a_path)
        {
            if (character == L'/')
            {
                character = L'\\';
            }
        }
        if (a_path.starts_with(L"\\\\?\\"))
        {
            return cue::Result<std::wstring>::success(std::move(a_path));
        }
        if (a_path.starts_with(L"\\\\"))
        {
            a_path = L"\\\\?\\UNC\\" + a_path.substr(2U);
        }
        else
        {
            a_path = L"\\\\?\\" + a_path;
        }
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    if (a_path.size() >= k_maxWindowsPathLength)
    {
        return cue::Result<std::wstring>::failure(cue::make_io_error(a_assertContext, cue::IoError::CapacityExceeded,
                                                                     "Workspace native path limit was exceeded"));
    }
    return cue::Result<std::wstring>::success(std::move(a_path));
}

/// @brief Windows Path PrefixをOrdinalな大小文字非依存で比較する
[[nodiscard]] bool starts_with_ordinal_ignore_case(std::wstring_view a_text, std::wstring_view a_prefix) noexcept
{
    if (a_text.size() < a_prefix.size())
    {
        return false;
    }
    return CompareStringOrdinal(a_text.data(), static_cast<int>(a_prefix.size()), a_prefix.data(),
                                static_cast<int>(a_prefix.size()), TRUE) == CSTR_EQUAL;
}

/// @brief 通常またはExtended形式のUNC Pathか判定する
[[nodiscard]] bool is_unc_path(std::wstring_view a_path) noexcept
{
    const bool extendedPrefix = a_path.starts_with(L"\\\\?\\");
    return (a_path.starts_with(L"\\\\") && !extendedPrefix) || starts_with_ordinal_ignore_case(a_path, L"\\\\?\\UNC\\");
}

/// @brief Local Drive Rootで保持すべき末尾Separator込み長を返す
[[nodiscard]] std::size_t local_drive_root_length(std::wstring_view a_path) noexcept
{
    if (a_path.size() >= 7U && a_path.starts_with(L"\\\\?\\") && std::iswalpha(a_path[4U]) != 0 && a_path[5U] == L':' &&
        (a_path[6U] == L'\\' || a_path[6U] == L'/'))
    {
        return 7U;
    }
    return 3U;
}

/// @brief Invalid UTF-16 Nameを安定した表示・Sort用16進表現へ変換する
[[nodiscard]] std::string make_native_name_fallback(std::wstring_view a_name,
                                                    const cue::AssertContext &a_assertContext) noexcept
{
    constexpr std::array<char, 16> k_hex{'0', '1', '2', '3', '4', '5', '6', '7',
                                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result;
    try
    {
        result = "<invalid-utf16:";
        result.reserve(result.size() + a_name.size() * 4U + 1U);
        for (wchar_t character : a_name)
        {
            const std::uint16_t value = static_cast<std::uint16_t>(character);
            result.push_back(k_hex[(value >> 12U) & 0x0fU]);
            result.push_back(k_hex[(value >> 8U) & 0x0fU]);
            result.push_back(k_hex[(value >> 4U) & 0x0fU]);
            result.push_back(k_hex[value & 0x0fU]);
        }
        result.push_back('>');
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    return result;
}

/// @brief Entry MetadataのBounded Byte数を返す
[[nodiscard]] std::size_t metadata_bytes(const cue::WorkspaceEntry &a_entry) noexcept
{
    const std::size_t locatorBytes = a_entry.locator.has_value() ? a_entry.locator->text().size() : 0U;
    constexpr std::size_t k_fixedBytes = sizeof(cue::WorkspaceEntry);
    if (a_entry.displayName.size() > std::numeric_limits<std::size_t>::max() - k_fixedBytes ||
        k_fixedBytes + a_entry.displayName.size() > std::numeric_limits<std::size_t>::max() - a_entry.sortKey.size() ||
        k_fixedBytes + a_entry.displayName.size() + a_entry.sortKey.size() >
            std::numeric_limits<std::size_t>::max() - locatorBytes)
    {
        return std::numeric_limits<std::size_t>::max();
    }
    return k_fixedBytes + a_entry.displayName.size() + a_entry.sortKey.size() + locatorBytes;
}

/// @brief Entry診断が使用するBounded Metadata Byte数を返す
[[nodiscard]] std::size_t metadata_bytes(const cue::WorkspaceDiagnostic &a_diagnostic) noexcept
{
    if (a_diagnostic.displayName.size() > std::numeric_limits<std::size_t>::max() - sizeof(cue::WorkspaceDiagnostic))
    {
        return std::numeric_limits<std::size_t>::max();
    }
    return sizeof(cue::WorkspaceDiagnostic) + a_diagnostic.displayName.size();
}

/// @brief Portable ASCII大小文字比較、Type、元Spellingの順でManifest Entryを比較する
[[nodiscard]] bool manifest_entry_less(const cue::WorkspaceManifestEntry &a_left,
                                       const cue::WorkspaceManifestEntry &a_right) noexcept
{
    const std::size_t common = std::min(a_left.path.size(), a_right.path.size());
    for (std::size_t index = 0U; index < common; ++index)
    {
        const char left = a_left.path[index] >= 'A' && a_left.path[index] <= 'Z'
                              ? static_cast<char>(a_left.path[index] + ('a' - 'A'))
                              : a_left.path[index];
        const char right = a_right.path[index] >= 'A' && a_right.path[index] <= 'Z'
                               ? static_cast<char>(a_right.path[index] + ('a' - 'A'))
                               : a_right.path[index];
        if (left != right)
        {
            return left < right;
        }
    }
    if (a_left.path.size() != a_right.path.size())
    {
        return a_left.path.size() < a_right.path.size();
    }
    if (a_left.type != a_right.type)
    {
        return a_left.type < a_right.type;
    }
    return a_left.path < a_right.path;
}

/// @brief Unsupported Entryの表示名をSort KeyへProject Fatal Policy付きで複製する
void copy_display_sort_key(cue::WorkspaceEntry &a_entry, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        a_entry.sortKey = a_entry.displayName;
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}

/// @brief Entry単位診断をProject Fatal Policy付きでSnapshotへ追加する
void append_diagnostic(cue::DirectorySnapshot &a_snapshot, cue::WorkspaceDiagnosticCode a_code,
                       std::string_view a_displayName, std::int64_t a_nativeCode,
                       const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        a_snapshot.diagnostics.push_back(
            cue::WorkspaceDiagnostic{a_snapshot.generation, a_code, std::string(a_displayName), a_nativeCode});
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}

/// @brief Root IdentityをProcess寿命中に比較する値
struct RootIdentity final
{
    DWORD volumeSerial = 0U;
    DWORD fileIndexHigh = 0U;
    DWORD fileIndexLow = 0U;
};

/// @brief 二つのNative File Identityが同じObjectを表すか判定する
[[nodiscard]] bool same_identity(const RootIdentity &a_left, const RootIdentity &a_right) noexcept
{
    return a_left.volumeSerial == a_right.volumeSerial && a_left.fileIndexHigh == a_right.fileIndexHigh &&
           a_left.fileIndexLow == a_right.fileIndexLow;
}

/// @brief Pin済みDirectory Handleから取得したEntry MetadataとFile Identity
struct NativeDirectoryEntry final
{
    std::wstring name;
    DWORD attributes = 0U;
    LARGE_INTEGER fileId{};
};

/// @brief Directory Handle列挙の完了状態と取得済みNative Entryを所有する
struct NativeDirectoryEnumeration final
{
    std::vector<NativeDirectoryEntry> entries;
    std::optional<DWORD> interruptedCode;
};

/// @brief Pin済みDirectory HandleをFile ID付きで列挙し、途中I/O失敗では取得済みEntryを保持する
[[nodiscard]] cue::Result<NativeDirectoryEnumeration> enumerate_directory_handle(
    HANDLE a_directoryHandle, std::size_t a_maximumEntries, std::size_t a_maximumMetadataBytes,
    const cue::AssertContext &a_assertContext) noexcept
{
    constexpr std::size_t k_directoryBufferBytes = 64U * 1024U;
    alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, k_directoryBufferBytes> buffer{};
    NativeDirectoryEnumeration enumeration;
    bool restart = true;
    bool receivedBatch = false;
    std::size_t metadataBytes = 0U;

    try
    {
        while (true)
        {
            const FILE_INFO_BY_HANDLE_CLASS informationClass =
                restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
            if (GetFileInformationByHandleEx(a_directoryHandle, informationClass, buffer.data(),
                                             static_cast<DWORD>(buffer.size())) == FALSE)
            {
                const DWORD code = GetLastError();
                if (code == ERROR_NO_MORE_FILES)
                {
                    break;
                }
                if (!receivedBatch)
                {
                    return cue::Result<NativeDirectoryEnumeration>::failure(
                        make_windows_error(a_assertContext, code, "Workspace directory enumeration failed"));
                }
                enumeration.interruptedCode = code;
                break;
            }
            receivedBatch = true;
            restart = false;

            std::size_t offset = 0U;
            while (true)
            {
                constexpr std::size_t k_minimumEntryBytes = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
                if (offset > buffer.size() - k_minimumEntryBytes)
                {
                    return cue::Result<NativeDirectoryEnumeration>::failure(cue::make_io_error(
                        a_assertContext, cue::IoError::IoFailure, "Workspace directory metadata is malformed"));
                }
                const auto *data = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO *>(buffer.data() + offset);
                if ((data->FileNameLength % sizeof(wchar_t)) != 0U ||
                    data->FileNameLength > buffer.size() - offset - k_minimumEntryBytes)
                {
                    return cue::Result<NativeDirectoryEnumeration>::failure(cue::make_io_error(
                        a_assertContext, cue::IoError::IoFailure, "Workspace directory name metadata is malformed"));
                }

                const std::wstring_view name(data->FileName, data->FileNameLength / sizeof(wchar_t));
                if (name != L"." && name != L"..")
                {
                    if (enumeration.entries.size() == a_maximumEntries)
                    {
                        return cue::Result<NativeDirectoryEnumeration>::failure(
                            cue::make_io_error(a_assertContext, cue::IoError::CapacityExceeded,
                                               "Workspace listing entry limit was exceeded"));
                    }
                    if (name.size() >
                        (std::numeric_limits<std::size_t>::max() - sizeof(NativeDirectoryEntry)) / sizeof(wchar_t))
                    {
                        return cue::Result<NativeDirectoryEnumeration>::failure(
                            cue::make_io_error(a_assertContext, cue::IoError::CapacityExceeded,
                                               "Workspace native listing metadata size overflowed"));
                    }
                    const std::size_t entryBytes = sizeof(NativeDirectoryEntry) + name.size() * sizeof(wchar_t);
                    if (entryBytes > a_maximumMetadataBytes - metadataBytes)
                    {
                        return cue::Result<NativeDirectoryEnumeration>::failure(
                            cue::make_io_error(a_assertContext, cue::IoError::CapacityExceeded,
                                               "Workspace native listing metadata limit was exceeded"));
                    }
                    enumeration.entries.push_back(
                        NativeDirectoryEntry{std::wstring(name), data->FileAttributes, data->FileId});
                    metadataBytes += entryBytes;
                }

                if (data->NextEntryOffset == 0U)
                {
                    break;
                }
                if (data->NextEntryOffset < k_minimumEntryBytes || data->NextEntryOffset > buffer.size() - offset)
                {
                    return cue::Result<NativeDirectoryEnumeration>::failure(cue::make_io_error(
                        a_assertContext, cue::IoError::IoFailure, "Workspace directory entry chain is malformed"));
                }
                offset += data->NextEntryOffset;
            }
        }
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    return cue::Result<NativeDirectoryEnumeration>::success(std::move(enumeration));
}

/// @brief Directory名変更監視中に列挙し、列挙期間中のNamespace変更を拒否する
[[nodiscard]] cue::Result<NativeDirectoryEnumeration> enumerate_directory_handle_stable(
    HANDLE a_directoryHandle, std::size_t a_maximumEntries, std::size_t a_maximumMetadataBytes,
    const cue::AssertContext &a_assertContext) noexcept
{
    FILE_CASE_SENSITIVE_INFO caseSensitivity{};
    if (GetFileInformationByHandleEx(a_directoryHandle, FileCaseSensitiveInfo, &caseSensitivity,
                                     sizeof(caseSensitivity)) == FALSE)
    {
        return cue::Result<NativeDirectoryEnumeration>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace directory case policy query failed"));
    }
    if ((caseSensitivity.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) == 0U)
    {
        return enumerate_directory_handle(a_directoryHandle, a_maximumEntries, a_maximumMetadataBytes, a_assertContext);
    }

    const DWORD pathLength =
        GetFinalPathNameByHandleW(a_directoryHandle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (pathLength == 0U)
    {
        return cue::Result<NativeDirectoryEnumeration>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace directory watch path query failed"));
    }
    std::wstring path;
    try
    {
        path.resize(pathLength);
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    const DWORD pathWritten =
        GetFinalPathNameByHandleW(a_directoryHandle, path.data(), pathLength, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (pathWritten == 0U || pathWritten >= pathLength)
    {
        return cue::Result<NativeDirectoryEnumeration>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace directory watch path query failed"));
    }
    path.resize(pathWritten);

    UniqueHandle watchHandle(
        CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED, nullptr));
    if (!watchHandle.is_valid())
    {
        return cue::Result<NativeDirectoryEnumeration>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace directory watch open failed"));
    }
    BY_HANDLE_FILE_INFORMATION expectedInformation{};
    BY_HANDLE_FILE_INFORMATION watchInformation{};
    if (GetFileInformationByHandle(a_directoryHandle, &expectedInformation) == FALSE ||
        GetFileInformationByHandle(watchHandle.get(), &watchInformation) == FALSE)
    {
        return cue::Result<NativeDirectoryEnumeration>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace directory watch inspection failed"));
    }
    if (expectedInformation.dwVolumeSerialNumber != watchInformation.dwVolumeSerialNumber ||
        expectedInformation.nFileIndexHigh != watchInformation.nFileIndexHigh ||
        expectedInformation.nFileIndexLow != watchInformation.nFileIndexLow ||
        (watchInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (watchInformation.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return cue::Result<NativeDirectoryEnumeration>::failure(cue::make_io_error(
            a_assertContext, cue::IoError::OutsideRoot, "Workspace directory watch identity changed"));
    }

    UniqueHandle eventHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!eventHandle.is_valid())
    {
        return cue::Result<NativeDirectoryEnumeration>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace directory watch event creation failed"));
    }

    alignas(DWORD) std::array<std::byte, 16U * 1024U> changes{};
    OVERLAPPED overlapped{};
    overlapped.hEvent = eventHandle.get();
    DWORD immediateBytes = 0U;
    if (ReadDirectoryChangesW(watchHandle.get(), changes.data(), static_cast<DWORD>(changes.size()), FALSE,
                              FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, &immediateBytes, &overlapped,
                              nullptr) == FALSE)
    {
        return cue::Result<NativeDirectoryEnumeration>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace directory watch start failed"));
    }

    cue::Result<NativeDirectoryEnumeration> enumeration =
        enumerate_directory_handle(a_directoryHandle, a_maximumEntries, a_maximumMetadataBytes, a_assertContext);

    DWORD transferred = 0U;
    bool changed = false;
    if (GetOverlappedResult(watchHandle.get(), &overlapped, &transferred, FALSE) != FALSE)
    {
        changed = true;
    }
    else
    {
        DWORD completionCode = GetLastError();
        if (completionCode == ERROR_IO_INCOMPLETE)
        {
            const bool cancellationSucceeded = CancelIoEx(watchHandle.get(), &overlapped) != FALSE;
            const DWORD cancellationCode = cancellationSucceeded ? ERROR_SUCCESS : GetLastError();
            if (!cancellationSucceeded && cancellationCode != ERROR_NOT_FOUND)
            {
                return cue::Result<NativeDirectoryEnumeration>::failure(make_windows_error(
                    a_assertContext, cancellationCode, "Workspace directory watch cancellation failed"));
            }
            if (GetOverlappedResult(watchHandle.get(), &overlapped, &transferred, TRUE) != FALSE)
            {
                changed = true;
            }
            else
            {
                completionCode = GetLastError();
                if (completionCode != ERROR_OPERATION_ABORTED || !cancellationSucceeded)
                {
                    return cue::Result<NativeDirectoryEnumeration>::failure(make_windows_error(
                        a_assertContext, completionCode, "Workspace directory watch completion failed"));
                }
            }
        }
        else
        {
            return cue::Result<NativeDirectoryEnumeration>::failure(
                make_windows_error(a_assertContext, completionCode, "Workspace directory watch query failed"));
        }
    }

    if (!enumeration)
    {
        return cue::Result<NativeDirectoryEnumeration>::failure(std::move(*enumeration.try_error()));
    }
    if (changed)
    {
        return cue::Result<NativeDirectoryEnumeration>::failure(cue::make_io_error(
            a_assertContext, cue::IoError::Busy, "Workspace directory changed during stable enumeration"));
    }
    return cue::Result<NativeDirectoryEnumeration>::success(std::move(*enumeration.try_value()));
}

/// @brief File IDが同じDirectory Entry Objectを示すか判定する
[[nodiscard]] bool same_file_id(const LARGE_INTEGER &a_left, const LARGE_INTEGER &a_right) noexcept
{
    return a_left.QuadPart == a_right.QuadPart;
}

/// @brief Entryを操作不能へ変更し、古いLocatorとMetadataを公開しない
void reject_stale_entry(cue::WorkspaceEntry &a_entry, cue::WorkspaceDiagnosticCode a_code,
                        const cue::AssertContext &a_assertContext) noexcept
{
    copy_display_sort_key(a_entry, a_assertContext);
    a_entry.locator.reset();
    a_entry.type = cue::WorkspaceEntryType::UnsupportedEntry;
    a_entry.byteSize = 0U;
    a_entry.rejection = a_code;
}

/// @brief lower-case UUID Version 4 Operation IDを内部Temporary名へ使用可能か検証する
[[nodiscard]] bool is_valid_operation_id(std::string_view a_operationId) noexcept
{
    if (a_operationId.size() != 36U || a_operationId[8U] != '-' || a_operationId[13U] != '-' ||
        a_operationId[18U] != '-' || a_operationId[23U] != '-' || a_operationId[14U] != '4' ||
        (a_operationId[19U] != '8' && a_operationId[19U] != '9' && a_operationId[19U] != 'a' &&
         a_operationId[19U] != 'b'))
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_operationId.size(); ++index)
    {
        if (index == 8U || index == 13U || index == 18U || index == 23U)
        {
            continue;
        }
        const char character = a_operationId[index];
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
        {
            return false;
        }
    }
    return true;
}

/// @brief Bound PathをRoot配下のExtended Native Pathへ変換する
[[nodiscard]] cue::Result<std::wstring> make_native_workspace_path(std::wstring_view a_rootPath,
                                                                   std::string_view a_path,
                                                                   const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<std::wstring> converted = to_utf16(a_path, a_assertContext);
    if (!converted)
    {
        return cue::Result<std::wstring>::failure(std::move(*converted.try_error()));
    }
    try
    {
        std::replace(converted.try_value()->begin(), converted.try_value()->end(), L'/', L'\\');
        std::wstring result(a_rootPath);
        if (!result.empty() && result.back() != L'\\')
        {
            result.push_back(L'\\');
        }
        result.append(*converted.try_value());
        if (result.size() >= k_maxWindowsPathLength)
        {
            return cue::Result<std::wstring>::failure(cue::make_io_error(
                a_assertContext, cue::IoError::CapacityExceeded, "Workspace native path exceeds the host limit"));
        }
        return cue::Result<std::wstring>::success(std::move(result));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}

/// @brief Windows Handle Metadataが同じDirectory Objectを示すか判定する
[[nodiscard]] bool same_directory_information(const BY_HANDLE_FILE_INFORMATION &a_left,
                                              const BY_HANDLE_FILE_INFORMATION &a_right) noexcept
{
    constexpr DWORD k_typeMask = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    return a_left.dwVolumeSerialNumber == a_right.dwVolumeSerialNumber &&
           a_left.nFileIndexHigh == a_right.nFileIndexHigh && a_left.nFileIndexLow == a_right.nFileIndexLow &&
           (a_left.dwFileAttributes & k_typeMask) == (a_right.dwFileAttributes & k_typeMask);
}

/// @brief 元Root Pathと親Handle列挙を再照合しながらDirectory Chainを移動不可で固定する
[[nodiscard]] cue::Result<std::unique_ptr<cue::WorkspaceWatcher>> create_windows_workspace_watcher(
    std::span<const HANDLE> a_expectedChain, std::wstring_view a_rootPath, std::string_view a_relativeDirectory,
    cue::WorkspaceWatchLimits a_limits, const cue::AssertContext &a_assertContext) noexcept
{
    if (!a_limits.is_valid() || a_expectedChain.empty() || a_rootPath.empty())
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
            cue::make_io_error(a_assertContext, cue::IoError::InvalidPath, "Workspace watcher limits are invalid"));
    }

    std::vector<UniqueHandle> locationLocks;
    std::wstring path;
    UniqueHandle directory;
    try
    {
        locationLocks.reserve(a_expectedChain.size() - 1U);
        path.assign(a_rootPath);
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }

    const bool watchesRoot = a_expectedChain.size() == 1U;
    const DWORD rootFlags =
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | (watchesRoot ? FILE_FLAG_OVERLAPPED : 0U);
    UniqueHandle rootLock(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, rootFlags, nullptr));
    if (!rootLock.is_valid())
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace watcher root lock failed"));
    }
    BY_HANDLE_FILE_INFORMATION expectedRoot{};
    BY_HANDLE_FILE_INFORMATION actualRoot{};
    if (GetFileInformationByHandle(a_expectedChain.front(), &expectedRoot) == FALSE ||
        GetFileInformationByHandle(rootLock.get(), &actualRoot) == FALSE)
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace watcher root identity query failed"));
    }
    if (!same_directory_information(expectedRoot, actualRoot) ||
        (actualRoot.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (actualRoot.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
            cue::make_io_error(a_assertContext, cue::IoError::OutsideRoot, "Workspace watcher root identity changed"));
    }
    if (watchesRoot)
    {
        directory = std::move(rootLock);
    }
    else
    {
        locationLocks.push_back(std::move(rootLock));
    }

    std::string_view remaining = a_relativeDirectory;
    for (std::size_t index = 1U; index < a_expectedChain.size(); ++index)
    {
        if (remaining.empty())
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(cue::make_io_error(
                a_assertContext, cue::IoError::InvalidPath, "Workspace watcher directory chain is incomplete"));
        }
        const std::size_t separator = remaining.find('/');
        const std::string_view component = remaining.substr(0U, separator);
        cue::Result<std::wstring> nativeComponent = to_utf16(component, a_assertContext);
        if (!nativeComponent)
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
                std::move(*nativeComponent.try_error()));
        }

        const HANDLE parentHandle = locationLocks.back().get();
        cue::Result<NativeDirectoryEnumeration> before = enumerate_directory_handle_stable(
            parentHandle, k_windowsHardLimits.maxVisitedEntries, k_windowsHardLimits.maxMetadataBytes, a_assertContext);
        if (!before)
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(std::move(*before.try_error()));
        }
        if (before.try_value()->interruptedCode.has_value())
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
                make_windows_error(a_assertContext, *before.try_value()->interruptedCode,
                                   "Workspace watcher parent enumeration was interrupted"));
        }

        /// @brief 現在の親直下で要求Spellingが一意なDirectory Entryだけを返す
        const auto findComponent =
            [&](const NativeDirectoryEnumeration &a_enumeration) noexcept -> const NativeDirectoryEntry *
        {
            const NativeDirectoryEntry *match = nullptr;
            std::size_t matchingNames = 0U;
            for (const NativeDirectoryEntry &entry : a_enumeration.entries)
            {
                if (CompareStringOrdinal(entry.name.data(), static_cast<int>(entry.name.size()),
                                         nativeComponent.try_value()->data(),
                                         static_cast<int>(nativeComponent.try_value()->size()), TRUE) == CSTR_EQUAL)
                {
                    ++matchingNames;
                    if (entry.name == *nativeComponent.try_value())
                    {
                        match = &entry;
                    }
                }
            }
            return matchingNames == 1U ? match : nullptr;
        };

        BY_HANDLE_FILE_INFORMATION expected{};
        if (GetFileInformationByHandle(a_expectedChain[index], &expected) == FALSE)
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
                make_windows_error(a_assertContext, GetLastError(), "Workspace watcher identity query failed"));
        }
        LARGE_INTEGER expectedId{};
        expectedId.HighPart = static_cast<LONG>(expected.nFileIndexHigh);
        expectedId.LowPart = expected.nFileIndexLow;
        const NativeDirectoryEntry *beforeEntry = findComponent(*before.try_value());
        constexpr DWORD k_typeMask = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
        if (beforeEntry == nullptr || !same_file_id(beforeEntry->fileId, expectedId) ||
            (beforeEntry->attributes & k_typeMask) != (expected.dwFileAttributes & k_typeMask) ||
            (beforeEntry->attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (beforeEntry->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(cue::make_io_error(
                a_assertContext, cue::IoError::OutsideRoot, "Workspace watcher directory left its parent"));
        }

        std::wstring componentPath;
        try
        {
            componentPath = path;
            if (!componentPath.empty() && componentPath.back() != L'\\')
            {
                componentPath.push_back(L'\\');
            }
            componentPath.append(*nativeComponent.try_value());
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
        if (componentPath.size() >= k_maxWindowsPathLength)
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(cue::make_io_error(
                a_assertContext, cue::IoError::CapacityExceeded, "Workspace watcher path exceeds the host limit"));
        }

        const bool watchedDirectory = index + 1U == a_expectedChain.size();
        const DWORD flags =
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | (watchedDirectory ? FILE_FLAG_OVERLAPPED : 0U);
        UniqueHandle locked(CreateFileW(componentPath.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr));
        if (!locked.is_valid())
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
                make_windows_error(a_assertContext, GetLastError(), "Workspace watcher directory lock failed"));
        }
        BY_HANDLE_FILE_INFORMATION actual{};
        if (GetFileInformationByHandle(locked.get(), &actual) == FALSE)
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
                make_windows_error(a_assertContext, GetLastError(), "Workspace watcher identity query failed"));
        }
        if (!same_directory_information(expected, actual) ||
            (actual.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (actual.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
                cue::make_io_error(a_assertContext, cue::IoError::OutsideRoot, "Workspace watcher identity changed"));
        }

        cue::Result<NativeDirectoryEnumeration> after = enumerate_directory_handle_stable(
            parentHandle, k_windowsHardLimits.maxVisitedEntries, k_windowsHardLimits.maxMetadataBytes, a_assertContext);
        if (!after)
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(std::move(*after.try_error()));
        }
        if (after.try_value()->interruptedCode.has_value())
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
                make_windows_error(a_assertContext, *after.try_value()->interruptedCode,
                                   "Workspace watcher parent verification was interrupted"));
        }
        const NativeDirectoryEntry *afterEntry = findComponent(*after.try_value());
        if (afterEntry == nullptr || !same_file_id(afterEntry->fileId, expectedId) ||
            (afterEntry->attributes & k_typeMask) != (expected.dwFileAttributes & k_typeMask))
        {
            return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(cue::make_io_error(
                a_assertContext, cue::IoError::OutsideRoot, "Workspace watcher directory left its parent"));
        }

        path = std::move(componentPath);
        if (watchedDirectory)
        {
            directory = std::move(locked);
        }
        else
        {
            locationLocks.push_back(std::move(locked));
        }
        remaining = separator == std::string_view::npos ? std::string_view{} : remaining.substr(separator + 1U);
    }
    if (!remaining.empty() || !directory.is_valid())
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(cue::make_io_error(
            a_assertContext, cue::IoError::InvalidPath, "Workspace watcher directory chain is inconsistent"));
    }

    UniqueHandle changeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    UniqueHandle stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!changeEvent.is_valid() || !stopEvent.is_valid())
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace watcher event creation failed"));
    }
    std::unique_ptr<WindowsWorkspaceWatcher> watcher;
    try
    {
        watcher = std::make_unique<WindowsWorkspaceWatcher>(std::move(directory), std::move(locationLocks),
                                                            std::move(path), std::move(changeEvent),
                                                            std::move(stopEvent), a_limits, a_assertContext);
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    cue::Result<void> started = watcher->start();
    if (!started)
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(std::move(*started.try_error()));
    }
    std::unique_ptr<cue::WorkspaceWatcher> result(std::move(watcher));
    return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::success(std::move(result));
}

/// @brief Primary Error付きNotCommitted結果を構築する
[[nodiscard]] cue::WorkspaceMutationResult not_committed(cue::Error a_error) noexcept
{
    cue::WorkspaceMutationResult result;
    result.outcome = cue::WorkspaceMutationOutcome::NotCommitted;
    result.primaryError = std::move(a_error);
    return result;
}

/// @brief Primary Error付きReconciliationRequired結果を構築する
[[nodiscard]] cue::WorkspaceMutationResult reconciliation_required(cue::Error a_error) noexcept
{
    cue::WorkspaceMutationResult result;
    result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
    result.primaryError = std::move(a_error);
    return result;
}

/// @brief Cleanup ErrorをSecondary診断として保持する
void append_secondary(cue::WorkspaceMutationResult &a_result, cue::Error a_error,
                      const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        a_result.secondaryDiagnostics.push_back(std::move(a_error));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}

enum class NativeEntryObservationState : std::uint8_t
{
    Missing,
    Matches,
    Different,
    QueryFailed
};

struct NativeEntryObservation final
{
    NativeEntryObservationState state = NativeEntryObservationState::QueryFailed;
    DWORD nativeCode = ERROR_SUCCESS;
};

struct OwnedStagingChild final
{
    std::wstring path;
    RootIdentity identity;
    bool isDirectory = false;
};

/// @brief Open済みEntryの64-bit File Identityを取得する
[[nodiscard]] cue::Result<RootIdentity> read_entry_identity(HANDLE a_handle,
                                                            const cue::AssertContext &a_assertContext) noexcept
{
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(a_handle, &information) == FALSE)
    {
        return cue::Result<RootIdentity>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace temporary inspection failed"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return cue::Result<RootIdentity>::failure(cue::make_io_error(a_assertContext, cue::IoError::UnsupportedEntry,
                                                                     "Workspace temporary became a reparse point"));
    }
    return cue::Result<RootIdentity>::success(
        RootIdentity{information.dwVolumeSerialNumber, information.nFileIndexHigh, information.nFileIndexLow});
}

/// @brief 親LocatorのNamespace変更監視を完了確認まで保持する
class DirectoryChangeGuard final
{
  public:
    /// @brief 未開始の変更監視Guardを生成する
    DirectoryChangeGuard() noexcept = default;
    /// @brief Overlapped Resourceの一意所有を保つためCopy構築を禁止する
    DirectoryChangeGuard(const DirectoryChangeGuard &) = delete;
    /// @brief Overlapped Resourceの一意所有を保つためCopy代入を禁止する
    DirectoryChangeGuard &operator=(const DirectoryChangeGuard &) = delete;
    /// @brief 発行済みOverlapped Addressを固定するためMove構築を禁止する
    DirectoryChangeGuard(DirectoryChangeGuard &&) = delete;
    /// @brief 発行済みOverlapped Addressを固定するためMove代入を禁止する
    DirectoryChangeGuard &operator=(DirectoryChangeGuard &&) = delete;
    /// @brief 未完了の変更監視を取消し、完了を待ってからResourceを解放する
    ~DirectoryChangeGuard()
    {
        if (m_pending)
        {
            CancelIoEx(m_directory.get(), &m_overlapped);
            DWORD transferred = 0U;
            GetOverlappedResult(m_directory.get(), &m_overlapped, &transferred, TRUE);
        }
    }

    UniqueHandle m_directory;
    UniqueHandle m_event;
    alignas(DWORD) std::array<std::byte, 16U * 1024U> m_changes{};
    OVERLAPPED m_overlapped{};
    bool m_pending = false;
};

/// @brief Directory内容変更のOplock BreakをPublish完了まで監視する
class DirectoryOplockGuard final
{
  public:
    /// @brief 未開始のOplock Guardを生成する
    DirectoryOplockGuard() noexcept = default;
    /// @brief Overlapped Resourceの一意所有を保つためCopy構築を禁止する
    DirectoryOplockGuard(const DirectoryOplockGuard &) = delete;
    /// @brief Overlapped Resourceの一意所有を保つためCopy代入を禁止する
    DirectoryOplockGuard &operator=(const DirectoryOplockGuard &) = delete;
    /// @brief 発行済みOverlapped Addressを固定するためMove構築を禁止する
    DirectoryOplockGuard(DirectoryOplockGuard &&) = delete;
    /// @brief 発行済みOverlapped Addressを固定するためMove代入を禁止する
    DirectoryOplockGuard &operator=(DirectoryOplockGuard &&) = delete;
    /// @brief 未完了のOplock要求を取消し、完了を待ってからResourceを解放する
    ~DirectoryOplockGuard()
    {
        if (m_pending)
        {
            CancelIoEx(m_directory, &m_overlapped);
            DWORD transferred = 0U;
            GetOverlappedResult(m_directory, &m_overlapped, &transferred, TRUE);
        }
    }

    HANDLE m_directory = INVALID_HANDLE_VALUE;
    UniqueHandle m_event;
    REQUEST_OPLOCK_INPUT_BUFFER m_input{};
    REQUEST_OPLOCK_OUTPUT_BUFFER m_output{};
    OVERLAPPED m_overlapped{};
    bool m_pending = false;
};

/// @brief Recovery Mutation中のRoot、全Child、再帰変更監視を一括所有するWindows Guard
class WindowsWorkspaceEntryMutationGuard final : public cue::WorkspaceEntryMutationGuard
{
  public:
    /// @brief 検証済みEntryと全Native Guardの所有権を受け取る
    WindowsWorkspaceEntryMutationGuard(const cue::WorkspaceFilesystem *a_owner, cue::BoundWorkspacePath a_path,
                                       cue::WorkspaceEntryFingerprint a_fingerprint,
                                       cue::TraversalLimits a_traversalLimits,
                                       cue::ContentVerificationLimits a_contentLimits, UniqueHandle a_rootHandle,
                                       RootIdentity a_rootIdentity, bool a_isDirectory,
                                       std::vector<UniqueHandle> a_childHandles,
                                       std::unique_ptr<DirectoryChangeGuard> a_rootChangeGuard) noexcept
        : m_owner(a_owner), m_path(std::move(a_path)), m_fingerprint(std::move(a_fingerprint)),
          m_traversalLimits(a_traversalLimits), m_contentLimits(a_contentLimits), m_rootHandle(std::move(a_rootHandle)),
          m_rootIdentity(a_rootIdentity), m_isDirectory(a_isDirectory), m_childHandles(std::move(a_childHandles)),
          m_rootChangeGuard(std::move(a_rootChangeGuard))
    {
    }

    /// @brief 保持中のNative Handleと変更監視を解放する
    ~WindowsWorkspaceEntryMutationGuard() override = default;

    const cue::WorkspaceFilesystem *m_owner;
    cue::BoundWorkspacePath m_path;
    cue::WorkspaceEntryFingerprint m_fingerprint;
    cue::TraversalLimits m_traversalLimits;
    cue::ContentVerificationLimits m_contentLimits;
    UniqueHandle m_rootHandle;
    RootIdentity m_rootIdentity;
    bool m_isDirectory;
    std::vector<UniqueHandle> m_childHandles;
    std::unique_ptr<DirectoryChangeGuard> m_rootChangeGuard;
};

/// @brief Overlapped Directory HandleへRead-Handle Oplock監視を開始する
[[nodiscard]] cue::Result<std::unique_ptr<DirectoryOplockGuard>> begin_directory_oplock_guard(
    HANDLE a_directoryHandle, const cue::AssertContext &a_assertContext) noexcept
{
    std::unique_ptr<DirectoryOplockGuard> guard;
    try
    {
        guard = std::make_unique<DirectoryOplockGuard>();
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }

    guard->m_directory = a_directoryHandle;
    guard->m_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!guard->m_event.is_valid())
    {
        return cue::Result<std::unique_ptr<DirectoryOplockGuard>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace directory oplock event creation failed"));
    }
    guard->m_overlapped.hEvent = guard->m_event.get();
    guard->m_input.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
    guard->m_input.StructureLength = sizeof(guard->m_input);
    guard->m_input.RequestedOplockLevel = OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE;
    guard->m_input.Flags = REQUEST_OPLOCK_INPUT_FLAG_REQUEST;
    if (DeviceIoControl(a_directoryHandle, FSCTL_REQUEST_OPLOCK, &guard->m_input, sizeof(guard->m_input),
                        &guard->m_output, sizeof(guard->m_output), nullptr, &guard->m_overlapped) != FALSE)
    {
        return cue::Result<std::unique_ptr<DirectoryOplockGuard>>::failure(cue::make_io_error(
            a_assertContext, cue::IoError::Busy, "Workspace directory oplock completed without becoming pending"));
    }
    const DWORD requestCode = GetLastError();
    if (requestCode != ERROR_IO_PENDING)
    {
        return cue::Result<std::unique_ptr<DirectoryOplockGuard>>::failure(
            make_windows_error(a_assertContext, requestCode, "Workspace directory oplock request failed"));
    }
    guard->m_pending = true;
    return cue::Result<std::unique_ptr<DirectoryOplockGuard>>::success(std::move(guard));
}

/// @brief Oplock Breakの有無を確定し、監視要求を安全に完了する
[[nodiscard]] cue::Result<void> finish_directory_oplock_guard(DirectoryOplockGuard &a_guard,
                                                              const cue::AssertContext &a_assertContext) noexcept
{
    if (!a_guard.m_pending)
    {
        return cue::Result<void>::success();
    }

    DWORD transferred = 0U;
    if (GetOverlappedResult(a_guard.m_directory, &a_guard.m_overlapped, &transferred, FALSE) != FALSE)
    {
        a_guard.m_pending = false;
        return cue::Result<void>::failure(cue::make_io_error(a_assertContext, cue::IoError::Busy,
                                                             "Workspace source directory oplock broke during copy"));
    }
    DWORD completionCode = GetLastError();
    if (completionCode != ERROR_IO_INCOMPLETE)
    {
        a_guard.m_pending = false;
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, completionCode, "Workspace directory oplock query failed"));
    }

    const bool cancellationSucceeded = CancelIoEx(a_guard.m_directory, &a_guard.m_overlapped) != FALSE;
    const DWORD cancellationCode = cancellationSucceeded ? ERROR_SUCCESS : GetLastError();
    const bool cancellationUnavailable = !cancellationSucceeded && cancellationCode != ERROR_NOT_FOUND;
    const BOOL completionSucceeded =
        GetOverlappedResult(a_guard.m_directory, &a_guard.m_overlapped, &transferred, TRUE);
    completionCode = completionSucceeded != FALSE ? ERROR_SUCCESS : GetLastError();
    a_guard.m_pending = false;
    if (cancellationUnavailable)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, cancellationCode, "Workspace directory oplock cancellation failed"));
    }
    if (completionSucceeded != FALSE)
    {
        return cue::Result<void>::failure(cue::make_io_error(a_assertContext, cue::IoError::Busy,
                                                             "Workspace source directory oplock broke during copy"));
    }
    if (completionCode == ERROR_OPERATION_ABORTED && cancellationSucceeded)
    {
        return cue::Result<void>::success();
    }
    return cue::Result<void>::failure(
        make_windows_error(a_assertContext, completionCode, "Workspace directory oplock completion failed"));
}

/// @brief 複数Directory Oplockを完了し、最初のBreakまたはNative Errorを返す
[[nodiscard]] cue::Result<void> finish_directory_oplock_guards(
    std::vector<std::unique_ptr<DirectoryOplockGuard>> &a_guards, const cue::AssertContext &a_assertContext) noexcept
{
    std::optional<cue::Error> firstError;
    for (std::unique_ptr<DirectoryOplockGuard> &guard : a_guards)
    {
        cue::Result<void> finished = finish_directory_oplock_guard(*guard, a_assertContext);
        if (!finished && !firstError.has_value())
        {
            firstError.emplace(std::move(*finished.try_error()));
        }
    }
    a_guards.clear();
    if (firstError.has_value())
    {
        return cue::Result<void>::failure(std::move(*firstError));
    }
    return cue::Result<void>::success();
}

/// @brief Directory Oplockが未完了であり、現時点までBreakされていないことを確認する
[[nodiscard]] cue::Result<void> verify_directory_oplock_guard_pending(
    DirectoryOplockGuard &a_guard, const cue::AssertContext &a_assertContext) noexcept
{
    if (!a_guard.m_pending)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(a_assertContext, cue::IoError::Busy, "Workspace directory oplock is no longer pending"));
    }
    DWORD transferred = 0U;
    if (GetOverlappedResult(a_guard.m_directory, &a_guard.m_overlapped, &transferred, FALSE) != FALSE)
    {
        a_guard.m_pending = false;
        return cue::Result<void>::failure(cue::make_io_error(
            a_assertContext, cue::IoError::Busy, "Workspace source directory oplock broke before copy publish"));
    }
    const DWORD completionCode = GetLastError();
    if (completionCode == ERROR_IO_INCOMPLETE)
    {
        return cue::Result<void>::success();
    }
    a_guard.m_pending = false;
    return cue::Result<void>::failure(
        make_windows_error(a_assertContext, completionCode, "Workspace directory oplock pre-publish query failed"));
}

/// @brief 全Directory OplockがPublish直前まで未Breakであることを確認する
[[nodiscard]] cue::Result<void> verify_directory_oplock_guards_pending(
    std::vector<std::unique_ptr<DirectoryOplockGuard>> &a_guards, const cue::AssertContext &a_assertContext) noexcept
{
    for (std::unique_ptr<DirectoryOplockGuard> &guard : a_guards)
    {
        cue::Result<void> pending = verify_directory_oplock_guard_pending(*guard, a_assertContext);
        if (!pending)
        {
            return cue::Result<void>::failure(std::move(*pending.try_error()));
        }
    }
    return cue::Result<void>::success();
}

/// @brief 同一Directory IdentityへOverlapped Namespace変更監視を開始する
[[nodiscard]] cue::Result<std::unique_ptr<DirectoryChangeGuard>> begin_directory_change_guard(
    HANDLE a_directoryHandle, const cue::AssertContext &a_assertContext, bool a_watchSubtree = false,
    DWORD a_notifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME) noexcept
{
    const DWORD pathLength =
        GetFinalPathNameByHandleW(a_directoryHandle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (pathLength == 0U)
    {
        return cue::Result<std::unique_ptr<DirectoryChangeGuard>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace parent watch path query failed"));
    }

    std::wstring path;
    std::unique_ptr<DirectoryChangeGuard> guard;
    try
    {
        path.resize(pathLength);
        guard = std::make_unique<DirectoryChangeGuard>();
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    const DWORD pathWritten =
        GetFinalPathNameByHandleW(a_directoryHandle, path.data(), pathLength, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (pathWritten == 0U || pathWritten >= pathLength)
    {
        return cue::Result<std::unique_ptr<DirectoryChangeGuard>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace parent watch path query failed"));
    }
    path.resize(pathWritten);

    guard->m_directory.reset(
        CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED, nullptr));
    if (!guard->m_directory.is_valid())
    {
        return cue::Result<std::unique_ptr<DirectoryChangeGuard>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace parent watch open failed"));
    }

    BY_HANDLE_FILE_INFORMATION expectedInformation{};
    BY_HANDLE_FILE_INFORMATION watchInformation{};
    if (GetFileInformationByHandle(a_directoryHandle, &expectedInformation) == FALSE ||
        GetFileInformationByHandle(guard->m_directory.get(), &watchInformation) == FALSE)
    {
        return cue::Result<std::unique_ptr<DirectoryChangeGuard>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace parent watch inspection failed"));
    }
    if (expectedInformation.dwVolumeSerialNumber != watchInformation.dwVolumeSerialNumber ||
        expectedInformation.nFileIndexHigh != watchInformation.nFileIndexHigh ||
        expectedInformation.nFileIndexLow != watchInformation.nFileIndexLow ||
        (watchInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (watchInformation.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return cue::Result<std::unique_ptr<DirectoryChangeGuard>>::failure(
            cue::make_io_error(a_assertContext, cue::IoError::OutsideRoot, "Workspace parent watch identity changed"));
    }

    guard->m_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!guard->m_event.is_valid())
    {
        return cue::Result<std::unique_ptr<DirectoryChangeGuard>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace parent watch event creation failed"));
    }
    guard->m_overlapped.hEvent = guard->m_event.get();
    DWORD immediateBytes = 0U;
    if (ReadDirectoryChangesW(guard->m_directory.get(), guard->m_changes.data(),
                              static_cast<DWORD>(guard->m_changes.size()), a_watchSubtree ? TRUE : FALSE,
                              a_notifyFilter, &immediateBytes, &guard->m_overlapped, nullptr) == FALSE)
    {
        return cue::Result<std::unique_ptr<DirectoryChangeGuard>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace parent watch start failed"));
    }
    guard->m_pending = true;
    return cue::Result<std::unique_ptr<DirectoryChangeGuard>>::success(std::move(guard));
}

/// @brief Namespace変更の有無を確定し、Overlapped監視を安全に完了する
[[nodiscard]] cue::Result<void> finish_directory_change_guard(DirectoryChangeGuard &a_guard,
                                                              const cue::AssertContext &a_assertContext) noexcept
{
    if (!a_guard.m_pending)
    {
        return cue::Result<void>::success();
    }
    DWORD transferred = 0U;
    if (GetOverlappedResult(a_guard.m_directory.get(), &a_guard.m_overlapped, &transferred, FALSE) != FALSE)
    {
        a_guard.m_pending = false;
        return cue::Result<void>::failure(cue::make_io_error(a_assertContext, cue::IoError::Busy,
                                                             "Workspace parent directory changed during mutation"));
    }

    DWORD completionCode = GetLastError();
    if (completionCode != ERROR_IO_INCOMPLETE)
    {
        a_guard.m_pending = false;
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, completionCode, "Workspace parent watch query failed"));
    }

    const bool cancellationSucceeded = CancelIoEx(a_guard.m_directory.get(), &a_guard.m_overlapped) != FALSE;
    const DWORD cancellationCode = cancellationSucceeded ? ERROR_SUCCESS : GetLastError();
    const bool cancellationUnavailable = !cancellationSucceeded && cancellationCode != ERROR_NOT_FOUND;
    const BOOL completionSucceeded =
        GetOverlappedResult(a_guard.m_directory.get(), &a_guard.m_overlapped, &transferred, TRUE);
    completionCode = completionSucceeded != FALSE ? ERROR_SUCCESS : GetLastError();
    a_guard.m_pending = false;

    if (cancellationUnavailable)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, cancellationCode, "Workspace parent watch cancellation failed"));
    }
    if (completionSucceeded != FALSE)
    {
        return cue::Result<void>::failure(cue::make_io_error(a_assertContext, cue::IoError::Busy,
                                                             "Workspace parent directory changed during mutation"));
    }
    if (completionCode == ERROR_OPERATION_ABORTED && cancellationSucceeded)
    {
        return cue::Result<void>::success();
    }
    return cue::Result<void>::failure(
        make_windows_error(a_assertContext, completionCode, "Workspace parent watch completion failed"));
}

/// @brief Directory変更監視が未完了であり、現時点まで変更がないことを非Blockingで確認する
[[nodiscard]] cue::Result<void> verify_directory_change_guard_pending(
    DirectoryChangeGuard &a_guard, const cue::AssertContext &a_assertContext) noexcept
{
    if (!a_guard.m_pending)
    {
        return cue::Result<void>::failure(cue::make_io_error(a_assertContext, cue::IoError::Busy,
                                                             "Workspace directory change guard is no longer pending"));
    }

    DWORD transferred = 0U;
    if (GetOverlappedResult(a_guard.m_directory.get(), &a_guard.m_overlapped, &transferred, FALSE) != FALSE)
    {
        a_guard.m_pending = false;
        return cue::Result<void>::failure(cue::make_io_error(a_assertContext, cue::IoError::Busy,
                                                             "Workspace directory changed before mutation publish"));
    }

    const DWORD completionCode = GetLastError();
    if (completionCode == ERROR_IO_INCOMPLETE)
    {
        return cue::Result<void>::success();
    }
    a_guard.m_pending = false;
    return cue::Result<void>::failure(
        make_windows_error(a_assertContext, completionCode, "Workspace directory watch pre-publish query failed"));
}

/// @brief Native PathのEntryをFollowせず開き、期待Identityと種別を照合する
[[nodiscard]] NativeEntryObservation observe_native_entry(std::wstring_view a_path, const RootIdentity &a_expected,
                                                          bool a_expectDirectory) noexcept
{
    UniqueHandle handle(CreateFileW(a_path.data(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.is_valid())
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        {
            return NativeEntryObservation{NativeEntryObservationState::Missing, code};
        }
        return NativeEntryObservation{NativeEntryObservationState::QueryFailed, code};
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle.get(), &information) == FALSE)
    {
        return NativeEntryObservation{NativeEntryObservationState::QueryFailed, GetLastError()};
    }
    const bool isDirectory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    const bool isReparse = (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
    const bool identityMatches = information.dwVolumeSerialNumber == a_expected.volumeSerial &&
                                 information.nFileIndexHigh == a_expected.fileIndexHigh &&
                                 information.nFileIndexLow == a_expected.fileIndexLow;
    return NativeEntryObservation{identityMatches && isDirectory == a_expectDirectory && !isReparse
                                      ? NativeEntryObservationState::Matches
                                      : NativeEntryObservationState::Different,
                                  ERROR_SUCCESS};
}

/// @brief Operation所有Temporaryを同一HandleでIdentity確認して削除予約する
void cleanup_owned_temporary(UniqueHandle &a_ownedHandle, std::wstring_view a_path, const RootIdentity &a_identity,
                             bool a_isDirectory, cue::WorkspaceMutationResult &a_result,
                             const cue::AssertContext &a_assertContext) noexcept
{
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (a_isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0U);
    UniqueHandle reopenedHandle;
    HANDLE cleanupHandle = a_ownedHandle.get();
    if (!a_ownedHandle.is_valid())
    {
        reopenedHandle.reset(CreateFileW(a_path.data(), DELETE | FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr));
        cleanupHandle = reopenedHandle.get();
    }
    if (cleanupHandle == nullptr || cleanupHandle == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        {
            return;
        }
        append_secondary(a_result, make_windows_error(a_assertContext, code, "Workspace temporary cleanup open failed"),
                         a_assertContext);
        a_result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
        return;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(cleanupHandle, &information) == FALSE)
    {
        append_secondary(
            a_result,
            make_windows_error(a_assertContext, GetLastError(), "Workspace temporary cleanup inspection failed"),
            a_assertContext);
        a_result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
        return;
    }
    const bool isDirectory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    const bool isReparse = (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
    const bool identityMatches = information.dwVolumeSerialNumber == a_identity.volumeSerial &&
                                 information.nFileIndexHigh == a_identity.fileIndexHigh &&
                                 information.nFileIndexLow == a_identity.fileIndexLow;
    if (!identityMatches || isDirectory != a_isDirectory || isReparse)
    {
        append_secondary(
            a_result, make_windows_error(a_assertContext, ERROR_INVALID_DATA, "Workspace temporary cleanup was unsafe"),
            a_assertContext);
        a_result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
        return;
    }

    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (SetFileInformationByHandle(cleanupHandle, FileDispositionInfo, &disposition, sizeof(disposition)) == FALSE)
    {
        append_secondary(a_result,
                         make_windows_error(a_assertContext, GetLastError(), "Workspace temporary cleanup failed"),
                         a_assertContext);
        a_result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
        return;
    }
    a_ownedHandle.reset();
    reopenedHandle.reset();

    const NativeEntryObservation completion = observe_native_entry(a_path, a_identity, a_isDirectory);
    if (completion.state != NativeEntryObservationState::Missing)
    {
        const DWORD code =
            completion.state == NativeEntryObservationState::QueryFailed ? completion.nativeCode : ERROR_INVALID_DATA;
        append_secondary(
            a_result,
            make_windows_error(a_assertContext, code, "Workspace temporary cleanup completion was not confirmed"),
            a_assertContext);
        a_result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
    }
}

/// @brief Parent直下にPortable大小文字比較で衝突するEntryが存在しないことを確認する
[[nodiscard]] cue::Result<void> verify_portable_destination_absent(HANDLE a_parent, std::wstring_view a_destinationName,
                                                                   const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<NativeDirectoryEnumeration> enumeration = enumerate_directory_handle_stable(
        a_parent, k_windowsHardLimits.maxVisitedEntries, k_windowsHardLimits.maxMetadataBytes, a_assertContext);
    if (!enumeration)
    {
        return cue::Result<void>::failure(std::move(*enumeration.try_error()));
    }
    if (enumeration.try_value()->interruptedCode.has_value())
    {
        return cue::Result<void>::failure(make_windows_error(a_assertContext, *enumeration.try_value()->interruptedCode,
                                                             "Workspace destination collision check was interrupted"));
    }

    for (const NativeDirectoryEntry &entry : enumeration.try_value()->entries)
    {
        const int comparison =
            CompareStringOrdinal(entry.name.data(), static_cast<int>(entry.name.size()), a_destinationName.data(),
                                 static_cast<int>(a_destinationName.size()), TRUE);
        if (comparison == 0)
        {
            return cue::Result<void>::failure(make_windows_error(a_assertContext, GetLastError(),
                                                                 "Workspace destination collision comparison failed"));
        }
        if (comparison == CSTR_EQUAL)
        {
            return cue::Result<void>::failure(cue::make_io_error(a_assertContext, cue::IoError::AlreadyExists,
                                                                 "Workspace destination conflicts by portable name"));
        }
    }
    return cue::Result<void>::success();
}

/// @brief Publish後のDestinationがPortable比較で唯一かつ期待Identityか確認する
[[nodiscard]] cue::Result<void> verify_portable_destination_unique(HANDLE a_parent, std::wstring_view a_destinationName,
                                                                   const RootIdentity &a_expected,
                                                                   bool a_expectDirectory,
                                                                   const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<NativeDirectoryEnumeration> enumeration = enumerate_directory_handle_stable(
        a_parent, k_windowsHardLimits.maxVisitedEntries, k_windowsHardLimits.maxMetadataBytes, a_assertContext);
    if (!enumeration)
    {
        return cue::Result<void>::failure(std::move(*enumeration.try_error()));
    }
    if (enumeration.try_value()->interruptedCode.has_value())
    {
        return cue::Result<void>::failure(make_windows_error(a_assertContext, *enumeration.try_value()->interruptedCode,
                                                             "Workspace published destination check was interrupted"));
    }

    std::size_t matchingNames = 0U;
    bool expectedEntryFound = false;
    for (const NativeDirectoryEntry &entry : enumeration.try_value()->entries)
    {
        const int comparison =
            CompareStringOrdinal(entry.name.data(), static_cast<int>(entry.name.size()), a_destinationName.data(),
                                 static_cast<int>(a_destinationName.size()), TRUE);
        if (comparison == 0)
        {
            return cue::Result<void>::failure(make_windows_error(a_assertContext, GetLastError(),
                                                                 "Workspace published destination comparison failed"));
        }
        if (comparison != CSTR_EQUAL)
        {
            continue;
        }

        ++matchingNames;
        const bool isDirectory = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
        const bool isReparse = (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
        const bool exactName = entry.name == a_destinationName;
        const bool identityMatches = entry.fileId.HighPart == static_cast<LONG>(a_expected.fileIndexHigh) &&
                                     entry.fileId.LowPart == a_expected.fileIndexLow;
        expectedEntryFound =
            expectedEntryFound || (exactName && identityMatches && isDirectory == a_expectDirectory && !isReparse);
    }

    if (matchingNames != 1U || !expectedEntryFound)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(a_assertContext, cue::IoError::AlreadyExists,
                               "Workspace published destination is not unique by portable name"));
    }
    return cue::Result<void>::success();
}

/// @brief 親Directory内の完全一致EntryをIdentity固定Handleとして所有する
struct OpenedNativeEntry final
{
    UniqueHandle handle;
    RootIdentity identity;
    bool isDirectory = false;
};

/// @brief 親Handle直下のPortable一意EntryをFile IDまたは指定Native Pathで開いて再照合する
[[nodiscard]] cue::Result<OpenedNativeEntry> open_exact_entry(HANDLE a_parent, std::wstring_view a_name, DWORD a_access,
                                                              DWORD a_shareMode,
                                                              const cue::AssertContext &a_assertContext,
                                                              bool a_overlapped = false, DWORD a_additionalFlags = 0U,
                                                              const wchar_t *a_nativePath = nullptr) noexcept
{
    cue::Result<NativeDirectoryEnumeration> before = enumerate_directory_handle_stable(
        a_parent, k_windowsHardLimits.maxVisitedEntries, k_windowsHardLimits.maxMetadataBytes, a_assertContext);
    if (!before)
    {
        return cue::Result<OpenedNativeEntry>::failure(std::move(*before.try_error()));
    }
    if (before.try_value()->interruptedCode.has_value())
    {
        return cue::Result<OpenedNativeEntry>::failure(make_windows_error(
            a_assertContext, *before.try_value()->interruptedCode, "Workspace source enumeration was interrupted"));
    }

    const NativeDirectoryEntry *matched = nullptr;
    std::size_t matchingNames = 0U;
    for (const NativeDirectoryEntry &entry : before.try_value()->entries)
    {
        const int comparison = CompareStringOrdinal(entry.name.data(), static_cast<int>(entry.name.size()),
                                                    a_name.data(), static_cast<int>(a_name.size()), TRUE);
        if (comparison == 0)
        {
            return cue::Result<OpenedNativeEntry>::failure(
                make_windows_error(a_assertContext, GetLastError(), "Workspace source comparison failed"));
        }
        if (comparison == CSTR_EQUAL)
        {
            ++matchingNames;
            if (entry.name == a_name)
            {
                matched = &entry;
            }
        }
    }
    if (matchingNames != 1U || matched == nullptr)
    {
        const cue::IoError code = matchingNames == 0U ? cue::IoError::NotFound : cue::IoError::PreconditionFailed;
        return cue::Result<OpenedNativeEntry>::failure(
            cue::make_io_error(a_assertContext, code, "Workspace source is missing or ambiguous"));
    }
    if ((matched->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return cue::Result<OpenedNativeEntry>::failure(
            cue::make_io_error(a_assertContext, cue::IoError::UnsupportedEntry, "Workspace source is a reparse point"));
    }

    const bool isDirectory = (matched->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    FILE_ID_DESCRIPTOR descriptor{};
    descriptor.dwSize = sizeof(descriptor);
    descriptor.Type = FileIdType;
    descriptor.FileId = matched->fileId;
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0U) |
                        (a_overlapped && isDirectory ? FILE_FLAG_OVERLAPPED : 0U) | a_additionalFlags;
    UniqueHandle handle(a_nativePath == nullptr
                            ? OpenFileById(a_parent, &descriptor, a_access, a_shareMode, nullptr, flags)
                            : CreateFileW(a_nativePath, a_access, a_shareMode, nullptr, OPEN_EXISTING, flags, nullptr));
    if (!handle.is_valid())
    {
        return cue::Result<OpenedNativeEntry>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace source guard open failed"));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle.get(), &information) == FALSE)
    {
        return cue::Result<OpenedNativeEntry>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace source guard inspection failed"));
    }
    LARGE_INTEGER actualId{};
    actualId.HighPart = static_cast<LONG>(information.nFileIndexHigh);
    actualId.LowPart = information.nFileIndexLow;
    if (!same_file_id(actualId, matched->fileId) ||
        ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) != isDirectory ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return cue::Result<OpenedNativeEntry>::failure(cue::make_io_error(
            a_assertContext, cue::IoError::PreconditionFailed, "Workspace source identity or type changed"));
    }

    cue::Result<void> rebound = verify_portable_destination_unique(
        a_parent, a_name,
        RootIdentity{information.dwVolumeSerialNumber, information.nFileIndexHigh, information.nFileIndexLow},
        isDirectory, a_assertContext);
    if (!rebound)
    {
        return cue::Result<OpenedNativeEntry>::failure(std::move(*rebound.try_error()));
    }
    return cue::Result<OpenedNativeEntry>::success(OpenedNativeEntry{
        std::move(handle),
        RootIdentity{information.dwVolumeSerialNumber, information.nFileIndexHigh, information.nFileIndexLow},
        isDirectory});
}

/// @brief Open済みRegular Fileを先頭から上限付きByte列として読み取る
[[nodiscard]] cue::Result<std::vector<std::byte>> read_open_file(HANDLE a_file, std::uint64_t a_maxBytes,
                                                                 const cue::AssertContext &a_assertContext) noexcept
{
    LARGE_INTEGER size{};
    if (GetFileSizeEx(a_file, &size) == FALSE)
    {
        return cue::Result<std::vector<std::byte>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace guarded file size query failed"));
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > a_maxBytes ||
        static_cast<std::uint64_t>(size.QuadPart) > std::numeric_limits<std::size_t>::max())
    {
        return cue::Result<std::vector<std::byte>>::failure(cue::make_io_error(
            a_assertContext, cue::IoError::CapacityExceeded, "Workspace guarded file exceeds the content limit"));
    }
    LARGE_INTEGER beginning{};
    if (SetFilePointerEx(a_file, beginning, nullptr, FILE_BEGIN) == FALSE)
    {
        return cue::Result<std::vector<std::byte>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace guarded file seek failed"));
    }

    std::vector<std::byte> bytes;
    try
    {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    std::size_t offset = 0U;
    while (offset < bytes.size())
    {
        const DWORD request = static_cast<DWORD>(
            std::min(bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD read = 0U;
        if (ReadFile(a_file, bytes.data() + offset, request, &read, nullptr) == FALSE || read != request)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                make_windows_error(a_assertContext, GetLastError(), "Workspace guarded file read failed"));
        }
        offset += read;
    }
    return cue::Result<std::vector<std::byte>>::success(std::move(bytes));
}

/// @brief Byte列の安定した64-bit内容Digestを計算する
[[nodiscard]] std::uint64_t content_digest(std::span<const std::byte> a_bytes) noexcept;

/// @brief 排他Guardが所有する単一Link Regular Fileを同じHandleからFingerprint化する
[[nodiscard]] cue::Result<cue::WorkspaceEntryFingerprint> fingerprint_guarded_file(
    HANDLE a_file, cue::ContentVerificationLimits a_limits, const cue::AssertContext &a_assertContext) noexcept
{
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(a_file, &information) == FALSE)
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace guarded file inspection failed"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U || information.nNumberOfLinks != 1U)
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(
            cue::make_io_error(a_assertContext, cue::IoError::UnsupportedEntry,
                               "Workspace guarded file is not a single-link regular file"));
    }
    const std::uint64_t maximumBytes =
        std::min({a_limits.maxFileBytes, a_limits.maxTotalBytes, k_windowsContentHardLimits.maxFileBytes,
                  k_windowsContentHardLimits.maxTotalBytes});
    cue::Result<std::vector<std::byte>> bytes = read_open_file(a_file, maximumBytes, a_assertContext);
    if (!bytes)
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(std::move(*bytes.try_error()));
    }
    cue::WorkspaceEntryFingerprint fingerprint;
    fingerprint.type = cue::WorkspaceEntryType::RegularFile;
    fingerprint.file = cue::WorkspaceFileFingerprint{bytes.try_value()->size(), content_digest(*bytes.try_value())};
    return cue::Result<cue::WorkspaceEntryFingerprint>::success(std::move(fingerprint));
}

/// @brief Open済みFileへ全Byte列を書込み、先頭へ戻してFlushする
[[nodiscard]] cue::Result<void> write_and_flush_file(HANDLE a_file, std::span<const std::byte> a_bytes,
                                                     const cue::AssertContext &a_assertContext) noexcept
{
    std::size_t offset = 0U;
    while (offset < a_bytes.size())
    {
        const DWORD request = static_cast<DWORD>(
            std::min(a_bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0U;
        if (WriteFile(a_file, a_bytes.data() + offset, request, &written, nullptr) == FALSE || written != request)
        {
            return cue::Result<void>::failure(
                make_windows_error(a_assertContext, GetLastError(), "Workspace staged copy write failed"));
        }
        offset += written;
    }
    if (FlushFileBuffers(a_file) == FALSE)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace staged copy flush failed"));
    }
    return cue::Result<void>::success();
}

/// @brief 二つのByte列が同じFNV-1a 64-bit Content Digestを持つか判定する
[[nodiscard]] std::uint64_t content_digest(std::span<const std::byte> a_bytes) noexcept
{
    std::uint64_t digest = 14695981039346656037ULL;
    for (const std::byte value : a_bytes)
    {
        digest ^= static_cast<std::uint8_t>(value);
        digest *= 1099511628211ULL;
    }
    return digest;
}

/// @brief Open済みOperation-owned Child EntryをDelete-on-closeへ変更する
[[nodiscard]] cue::Result<void> mark_entry_for_deletion(UniqueHandle &a_entry,
                                                        const cue::AssertContext &a_assertContext) noexcept
{
    FILE_DISPOSITION_INFO_EX extendedDisposition{};
    extendedDisposition.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    if (SetFileInformationByHandle(a_entry.get(), FileDispositionInfoEx, &extendedDisposition,
                                   sizeof(extendedDisposition)) != FALSE)
    {
        a_entry.reset();
        return cue::Result<void>::success();
    }
    const DWORD extendedCode = GetLastError();
    if (extendedCode != ERROR_INVALID_PARAMETER && extendedCode != ERROR_NOT_SUPPORTED)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, extendedCode, "Workspace staged copy cleanup failed"));
    }
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (SetFileInformationByHandle(a_entry.get(), FileDispositionInfo, &disposition, sizeof(disposition)) == FALSE)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace staged copy cleanup failed"));
    }
    a_entry.reset();
    return cue::Result<void>::success();
}

/// @brief Operation-owned Staging Childを末端から削除予約しCleanup診断を保持する
void cleanup_staging_children(std::vector<UniqueHandle> &a_children, cue::WorkspaceMutationResult &a_result,
                              const cue::AssertContext &a_assertContext) noexcept
{
    for (auto iterator = a_children.rbegin(); iterator != a_children.rend(); ++iterator)
    {
        cue::Result<void> removed = mark_entry_for_deletion(*iterator, a_assertContext);
        if (!removed)
        {
            append_secondary(a_result, std::move(*removed.try_error()), a_assertContext);
            a_result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
        }
    }
    a_children.clear();
}

/// @brief Open済みStaging ChildのRollback用Path、Identity、Typeを取得する
[[nodiscard]] cue::Result<OwnedStagingChild> capture_staging_child(HANDLE a_child,
                                                                   const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<RootIdentity> identity = read_entry_identity(a_child, a_assertContext);
    if (!identity)
    {
        return cue::Result<OwnedStagingChild>::failure(std::move(*identity.try_error()));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(a_child, &information) == FALSE)
    {
        return cue::Result<OwnedStagingChild>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace staged child inspection failed"));
    }

    const DWORD required = GetFinalPathNameByHandleW(a_child, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0U)
    {
        return cue::Result<OwnedStagingChild>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace staged child path query failed"));
    }
    std::wstring path;
    try
    {
        path.resize(required);
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    const DWORD written =
        GetFinalPathNameByHandleW(a_child, path.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0U || written >= required)
    {
        return cue::Result<OwnedStagingChild>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace staged child path query failed"));
    }
    path.resize(written);

    OwnedStagingChild child;
    child.path = std::move(path);
    child.identity = *identity.try_value();
    child.isDirectory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    return cue::Result<OwnedStagingChild>::success(std::move(child));
}

/// @brief 保存済みIdentityと一致するStaging Childだけを末端から削除予約する
void cleanup_staging_child_records(std::vector<OwnedStagingChild> &a_children, cue::WorkspaceMutationResult &a_result,
                                   const cue::AssertContext &a_assertContext) noexcept
{
    for (auto iterator = a_children.rbegin(); iterator != a_children.rend(); ++iterator)
    {
        const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (iterator->isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0U);
        UniqueHandle child(CreateFileW(iterator->path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr));
        if (!child.is_valid())
        {
            const DWORD code = GetLastError();
            append_secondary(a_result,
                             make_windows_error(a_assertContext, code,
                                                code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
                                                    ? "Workspace staged child cleanup location could not be confirmed"
                                                    : "Workspace staged child cleanup open failed"),
                             a_assertContext);
            a_result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
            continue;
        }
        cue::Result<RootIdentity> observed = read_entry_identity(child.get(), a_assertContext);
        if (!observed || observed.try_value()->volumeSerial != iterator->identity.volumeSerial ||
            observed.try_value()->fileIndexHigh != iterator->identity.fileIndexHigh ||
            observed.try_value()->fileIndexLow != iterator->identity.fileIndexLow)
        {
            cue::Error error = !observed ? std::move(*observed.try_error())
                                         : cue::make_io_error(a_assertContext, cue::IoError::PreconditionFailed,
                                                              "Workspace staged child cleanup identity changed");
            append_secondary(a_result, std::move(error), a_assertContext);
            a_result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
            continue;
        }
        cue::Result<void> removed = mark_entry_for_deletion(child, a_assertContext);
        if (!removed)
        {
            append_secondary(a_result, std::move(*removed.try_error()), a_assertContext);
            a_result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
        }
    }
    a_children.clear();
}

/// @brief Identity固定中のEntryを固定済み親Handle直下の未存在名へRenameする
[[nodiscard]] DWORD rename_open_entry_impl(HANDLE a_source, HANDLE a_destinationParent,
                                           std::wstring_view a_destinationName, bool a_replaceExisting,
                                           const cue::AssertContext &a_assertContext) noexcept
{
    if (a_destinationParent == nullptr || a_destinationParent == INVALID_HANDLE_VALUE || a_destinationName.empty() ||
        a_destinationName.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max()) / sizeof(wchar_t))
    {
        return ERROR_FILENAME_EXCED_RANGE;
    }

    const std::size_t nameBytes = a_destinationName.size() * sizeof(wchar_t);
    const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
    std::vector<std::byte> storage;
    try
    {
        storage.resize(informationBytes);
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }

    auto *information = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    information->ReplaceIfExists = a_replaceExisting ? TRUE : FALSE;
    information->RootDirectory = a_destinationParent;
    information->FileNameLength = static_cast<DWORD>(nameBytes);
    std::copy(a_destinationName.begin(), a_destinationName.end(), information->FileName);
    using NtSetInformationFileFunction = NTSTATUS(NTAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
    using RtlNtStatusToDosErrorFunction = ULONG(NTAPI *)(NTSTATUS);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
    const auto ntSetInformationFile =
        reinterpret_cast<NtSetInformationFileFunction>(GetProcAddress(ntdll, "NtSetInformationFile"));
    const auto rtlNtStatusToDosError =
        reinterpret_cast<RtlNtStatusToDosErrorFunction>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (ntSetInformationFile == nullptr || rtlNtStatusToDosError == nullptr)
    {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }

    constexpr ULONG fileRenameInformationClass = 10U;
    IO_STATUS_BLOCK statusBlock{};
    const NTSTATUS status = ntSetInformationFile(a_source, &statusBlock, information,
                                                 static_cast<ULONG>(informationBytes), fileRenameInformationClass);
    if (status < 0)
    {
        return rtlNtStatusToDosError(status);
    }
    return ERROR_SUCCESS;
}

/// @brief Identity固定中のEntryを固定済み親Handle直下の未存在名へRenameする
[[nodiscard]] DWORD rename_open_entry(HANDLE a_source, HANDLE a_destinationParent, std::wstring_view a_destinationName,
                                      const cue::AssertContext &a_assertContext) noexcept
{
    return rename_open_entry_impl(a_source, a_destinationParent, a_destinationName, false, a_assertContext);
}

/// @brief Identity固定中のEntryで固定済み親Handle直下の既存Entryを置換する
[[nodiscard]] DWORD replace_open_entry(HANDLE a_source, HANDLE a_destinationParent, std::wstring_view a_destinationName,
                                       const cue::AssertContext &a_assertContext) noexcept
{
    return rename_open_entry_impl(a_source, a_destinationParent, a_destinationName, true, a_assertContext);
}

/// @brief Parent Handle直下へDirectoryを排他的に作成して同じNative操作でHandleを取得する
[[nodiscard]] DWORD create_directory_exclusive(HANDLE a_parent, std::wstring_view a_name,
                                               UniqueHandle &a_createdHandle) noexcept
{
    if (a_name.empty() ||
        a_name.size() > static_cast<std::size_t>(std::numeric_limits<USHORT>::max()) / sizeof(wchar_t))
    {
        return ERROR_FILENAME_EXCED_RANGE;
    }

    using NtCreateFileFunction = NTSTATUS(NTAPI *)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                                   PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using RtlNtStatusToDosErrorFunction = ULONG(NTAPI *)(NTSTATUS);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
    const auto ntCreateFile = reinterpret_cast<NtCreateFileFunction>(GetProcAddress(ntdll, "NtCreateFile"));
    const auto rtlNtStatusToDosError =
        reinterpret_cast<RtlNtStatusToDosErrorFunction>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (ntCreateFile == nullptr || rtlNtStatusToDosError == nullptr)
    {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }

    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(a_name.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    name.Buffer = const_cast<PWSTR>(a_name.data());

    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, a_parent, nullptr);
    IO_STATUS_BLOCK statusBlock{};
    HANDLE created = INVALID_HANDLE_VALUE;
    const NTSTATUS status = ntCreateFile(
        &created, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &statusBlock, nullptr,
        FILE_ATTRIBUTE_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH | FILE_OPEN_REPARSE_POINT, nullptr, 0U);
    if (status < 0)
    {
        return rtlNtStatusToDosError(status);
    }
    if (created == nullptr || created == INVALID_HANDLE_VALUE || statusBlock.Information != FILE_CREATED)
    {
        if (created != nullptr && created != INVALID_HANDLE_VALUE)
        {
            CloseHandle(created);
        }
        return ERROR_INVALID_DATA;
    }

    a_createdHandle.reset(created);
    return ERROR_SUCCESS;
}

/// @brief Parent Handle直下へFileを排他的に作成して同じNative操作でHandleを取得する
[[nodiscard]] DWORD create_file_exclusive(HANDLE a_parent, std::wstring_view a_name,
                                          UniqueHandle &a_createdHandle) noexcept
{
    if (a_name.empty() ||
        a_name.size() > static_cast<std::size_t>(std::numeric_limits<USHORT>::max()) / sizeof(wchar_t))
    {
        return ERROR_FILENAME_EXCED_RANGE;
    }

    using NtCreateFileFunction = NTSTATUS(NTAPI *)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                                   PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using RtlNtStatusToDosErrorFunction = ULONG(NTAPI *)(NTSTATUS);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
    const auto ntCreateFile = reinterpret_cast<NtCreateFileFunction>(GetProcAddress(ntdll, "NtCreateFile"));
    const auto rtlNtStatusToDosError =
        reinterpret_cast<RtlNtStatusToDosErrorFunction>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (ntCreateFile == nullptr || rtlNtStatusToDosError == nullptr)
    {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }

    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(a_name.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    name.Buffer = const_cast<PWSTR>(a_name.data());

    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, a_parent, nullptr);
    IO_STATUS_BLOCK statusBlock{};
    HANDLE created = INVALID_HANDLE_VALUE;
    const NTSTATUS status = ntCreateFile(&created, GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE, &attributes,
                                         &statusBlock, nullptr, FILE_ATTRIBUTE_TEMPORARY, FILE_SHARE_READ, FILE_CREATE,
                                         FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH |
                                             FILE_OPEN_REPARSE_POINT,
                                         nullptr, 0U);
    if (status < 0)
    {
        return rtlNtStatusToDosError(status);
    }
    if (created == nullptr || created == INVALID_HANDLE_VALUE || statusBlock.Information != FILE_CREATED)
    {
        if (created != nullptr && created != INVALID_HANDLE_VALUE)
        {
            CloseHandle(created);
        }
        return ERROR_INVALID_DATA;
    }

    a_createdHandle.reset(created);
    return ERROR_SUCCESS;
}

/// @brief Windows Root境界内だけを列挙するWorkspace Adapter
class WindowsWorkspaceFilesystem final : public cue::WorkspaceFilesystem
{
  public:
    /// @brief 検証済みRoot Path、Handle、Identityを所有する
    WindowsWorkspaceFilesystem(const cue::AssertContext &a_assertContext, std::wstring a_requestedRootPath,
                               std::wstring a_rootPath, UniqueHandle a_rootHandle, RootIdentity a_identity,
                               std::size_t a_maxBoundPathCharacters) noexcept
        : cue::WorkspaceFilesystem(a_maxBoundPathCharacters), m_assertContext(a_assertContext),
          m_requestedRootPath(std::move(a_requestedRootPath)), m_rootPath(std::move(a_rootPath)),
          m_rootHandle(std::move(a_rootHandle)), m_identity(a_identity)
    {
    }
    /// @brief Native Rootの一意所有を保つためCopy構築を禁止する
    WindowsWorkspaceFilesystem(const WindowsWorkspaceFilesystem &) = delete;
    /// @brief Native Rootの一意所有を保つためCopy代入を禁止する
    WindowsWorkspaceFilesystem &operator=(const WindowsWorkspaceFilesystem &) = delete;
    /// @brief Native Root所有権を移動する
    WindowsWorkspaceFilesystem(WindowsWorkspaceFilesystem &&) noexcept = default;
    /// @brief Native Root所有権を移動代入する
    WindowsWorkspaceFilesystem &operator=(WindowsWorkspaceFilesystem &&) noexcept = default;
    /// @brief Root HandleとAdapter状態を解放する
    ~WindowsWorkspaceFilesystem() override = default;

    /// @brief Windows Adapterの固定Hard Limitを返す
    [[nodiscard]] cue::TraversalLimits hard_limits() const noexcept override
    {
        return k_windowsHardLimits;
    }

    /// @brief Windows Adapterの固定Content Hard Limitを返す
    [[nodiscard]] cue::ContentVerificationLimits hard_content_limits() const noexcept override
    {
        return k_windowsContentHardLimits;
    }

    /// @brief 未検証Absolute Windows PathをRoot相対Portable Capabilityへ正規化する
    [[nodiscard]] cue::Result<cue::BoundWorkspacePath> bind_external_path(
        std::string_view a_unverifiedAbsolutePath) noexcept override;

    /// @brief Identity固定したRoot内Directoryの再帰Watcherを生成する
    [[nodiscard]] cue::Result<std::unique_ptr<cue::WorkspaceWatcher>> create_watcher(
        const cue::WorkspaceDirectory &a_directory, cue::WorkspaceWatchLimits a_limits) noexcept override;

    /// @brief Directory直下をRoot Pin保持中に列挙する
    [[nodiscard]] cue::Result<cue::DirectorySnapshot> list_directory(const cue::WorkspaceDirectory &a_directory,
                                                                     cue::TraversalLimits a_limits) noexcept override;

    /// @brief CapabilityのDirectory ChainをIdentity固定して実在検証する
    [[nodiscard]] cue::Result<void> verify_directory(const cue::WorkspaceDirectory &a_directory) noexcept override;

    /// @brief DirectoryのWindows VolumeとFile IDをOpaque比較値として返す
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> directory_identity(
        const cue::WorkspaceDirectory &a_directory) noexcept override;

    /// @brief Identity固定したRegular Fileを排他的に上限付き読取りする
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file_bounded(const cue::BoundWorkspacePath &a_source,
                                                                        std::size_t a_maxBytes) noexcept override;

    /// @brief Entryの単一LinkとContentを上限内でFingerprint化する
    [[nodiscard]] cue::Result<cue::WorkspaceEntryFingerprint> fingerprint_entry(
        const cue::BoundWorkspacePath &a_source, cue::TraversalLimits a_traversalLimits,
        cue::ContentVerificationLimits a_contentLimits) noexcept override;

    /// @brief Sibling Staging DirectoryをWrite-through RenameしてCreate-newする
    [[nodiscard]] cue::WorkspaceMutationResult create_directory_new(const cue::BoundWorkspacePath &a_destination,
                                                                    std::string_view a_operationId) noexcept override;

    /// @brief Sibling Temporary Fileを完全書込み後にWrite-through RenameしてCreate-newする
    [[nodiscard]] cue::WorkspaceMutationResult create_file_new_atomic(const cue::BoundWorkspacePath &a_destination,
                                                                      std::span<const std::byte> a_bytes,
                                                                      std::string_view a_operationId) noexcept override;

    /// @brief 既存Regular FileをSibling Temporary FileからAtomic置換する
    [[nodiscard]] cue::WorkspaceMutationResult replace_file_atomic(const cue::BoundWorkspacePath &a_destination,
                                                                   std::span<const std::byte> a_bytes,
                                                                   std::string_view a_operationId) noexcept override;

    /// @brief Identity固定したSourceを一回の同一Volume RenameでDestinationへ移す
    [[nodiscard]] cue::WorkspaceMutationResult rename_entry(const cue::BoundWorkspacePath &a_source,
                                                            const cue::BoundWorkspacePath &a_destination,
                                                            cue::TraversalLimits a_limits) noexcept override;

    /// @brief EntryのIdentity、全Child、再帰変更監視を固定してFingerprint化する
    [[nodiscard]] cue::Result<cue::GuardedWorkspaceEntry> guard_entry(
        const cue::BoundWorkspacePath &a_entry, cue::TraversalLimits a_traversalLimits,
        cue::ContentVerificationLimits a_contentLimits) noexcept override;

    /// @brief Entryが期待Fingerprintと一致する場合だけWindows Mutation Guardを取得する
    [[nodiscard]] cue::Result<std::unique_ptr<cue::WorkspaceEntryMutationGuard>> guard_entry_if_matches(
        const cue::BoundWorkspacePath &a_entry, const cue::WorkspaceEntryFingerprint &a_expected,
        cue::TraversalLimits a_traversalLimits, cue::ContentVerificationLimits a_contentLimits) noexcept override;

    /// @brief Windows Guardが固定した同じRoot HandleからDestinationへRenameする
    [[nodiscard]] cue::WorkspaceMutationResult rename_guarded_entry(
        cue::WorkspaceEntryMutationGuard &a_guard, const cue::BoundWorkspacePath &a_source,
        const cue::BoundWorkspacePath &a_destination) noexcept override;

    /// @brief Guard期間中のFingerprintと再帰変更監視を検証して全Handleを解放する
    [[nodiscard]] cue::Result<void> finish_entry_mutation_guard(
        std::unique_ptr<cue::WorkspaceEntryMutationGuard> a_guard) noexcept override;

    /// @brief Guard保持中のSourceをSibling TemporaryまたはStagingからCreate-new Copyする
    [[nodiscard]] cue::WorkspaceMutationResult copy_entry_new(const cue::BoundWorkspacePath &a_source,
                                                              const cue::BoundWorkspacePath &a_destination,
                                                              cue::TraversalLimits a_traversalLimits,
                                                              cue::ContentVerificationLimits a_contentLimits,
                                                              std::string_view a_operationId) noexcept override;

    /// @brief Identity固定したRegular Fileまたは空Directoryだけを削除する
    [[nodiscard]] cue::WorkspaceMutationResult remove_file_or_empty_directory(
        const cue::BoundWorkspacePath &a_entry) noexcept override;

  private:
    /// @brief Root PathがBinding時と同じNative Directory Objectか再検証する
    [[nodiscard]] cue::Result<void> verify_root_identity() const noexcept;
    /// @brief 未検証PathがBinding済みRootをNative Identityで指すPrefix長を返す
    [[nodiscard]] cue::Result<std::size_t> external_root_prefix_length(
        std::wstring_view a_normalizedPath) const noexcept;

    /// @brief 対象Directoryまでの全Componentを非Reparse DirectoryとしてPinする
    [[nodiscard]] cue::Result<std::vector<UniqueHandle>> pin_directory_chain(
        const cue::WorkspaceDirectory &a_directory) const noexcept;

    /// @brief Find Data一件を検証済みPortable Entryへ変換する
    [[nodiscard]] cue::Result<cue::WorkspaceEntry> make_entry(const cue::WorkspaceDirectory &a_directory,
                                                              HANDLE a_directoryHandle,
                                                              const NativeDirectoryEntry &a_data,
                                                              std::uint64_t a_generation,
                                                              cue::DirectorySnapshot &a_snapshot) const noexcept;

    /// @brief 親DirectoryをPinし、DestinationとStagingのNative Pathを構築する
    [[nodiscard]] cue::Result<std::vector<UniqueHandle>> prepare_mutation_parent(
        const cue::BoundWorkspacePath &a_destination) const noexcept;
    /// @brief 親Locator全ComponentのNamespace変更Guardを同時に開始する
    [[nodiscard]] cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>> begin_mutation_parent_guards(
        const std::vector<UniqueHandle> &a_pinned,
        std::span<const RootIdentity> a_excludedDirectories = {}) const noexcept;
    /// @brief 親Locator全Componentの変更監視がPublish直前まで未発火か確認する
    [[nodiscard]] cue::Result<void> verify_mutation_parent_guards_pending(
        std::vector<std::unique_ptr<DirectoryChangeGuard>> &a_guards) const noexcept;
    /// @brief 親Locator全Componentの変更監視を完了し、最初の不整合を返す
    [[nodiscard]] cue::Result<void> finish_mutation_parent_guards(
        std::vector<std::unique_ptr<DirectoryChangeGuard>> &a_guards) const noexcept;
    /// @brief 親Locator全Componentの一意性と保持Identityを事後再検証する
    [[nodiscard]] cue::Result<void> verify_mutation_parent_chain(
        const cue::BoundWorkspacePath &a_destination, const std::vector<UniqueHandle> &a_expected) const noexcept;

    /// @brief Entryを一回の安定した読取りでFingerprint化する
    [[nodiscard]] cue::Result<cue::WorkspaceEntryFingerprint> fingerprint_entry_once(
        const cue::BoundWorkspacePath &a_source, cue::TraversalLimits a_traversalLimits,
        cue::ContentVerificationLimits a_contentLimits) noexcept;
    /// @brief Optional Fingerprint条件を適用してRenameを共通実行する
    [[nodiscard]] cue::WorkspaceMutationResult rename_entry_internal(
        const cue::BoundWorkspacePath &a_source, const cue::BoundWorkspacePath &a_destination,
        const cue::WorkspaceEntryFingerprint *a_expected, cue::TraversalLimits a_traversalLimits,
        cue::ContentVerificationLimits a_contentLimits) noexcept;

    cue::AssertContext m_assertContext;
    std::wstring m_requestedRootPath;
    std::wstring m_rootPath;
    UniqueHandle m_rootHandle;
    RootIdentity m_identity;
    std::uint64_t m_nextGeneration = 1U;
};

cue::Result<void> WindowsWorkspaceFilesystem::verify_root_identity() const noexcept
{
    UniqueHandle current(CreateFileW(m_rootPath.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!current.is_valid())
    {
        return cue::Result<void>::failure(cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot,
                                                             "Workspace root no longer resolves to the bound root",
                                                             static_cast<std::int64_t>(GetLastError())));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(current.get(), &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        information.dwVolumeSerialNumber != m_identity.volumeSerial ||
        information.nFileIndexHigh != m_identity.fileIndexHigh || information.nFileIndexLow != m_identity.fileIndexLow)
    {
        return cue::Result<void>::failure(
            cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot, "Workspace root identity changed"));
    }
    return cue::Result<void>::success();
}

cue::Result<std::size_t> WindowsWorkspaceFilesystem::external_root_prefix_length(
    std::wstring_view a_normalizedPath) const noexcept
{
    cue::Result<void> rootIdentity = verify_root_identity();
    if (!rootIdentity)
    {
        return cue::Result<std::size_t>::failure(std::move(*rootIdentity.try_error()));
    }

    const std::array<std::wstring_view, 2U> candidates{m_requestedRootPath, m_rootPath};
    for (std::wstring_view expectedRoot : candidates)
    {
        const bool hasSeparator =
            !expectedRoot.empty() && (expectedRoot.back() == L'\\' || expectedRoot.back() == L'/');
        const bool hasBoundary = a_normalizedPath.size() > expectedRoot.size() &&
                                 (hasSeparator || a_normalizedPath[expectedRoot.size()] == L'\\' ||
                                  a_normalizedPath[expectedRoot.size()] == L'/');
        if (!hasBoundary || !starts_with_ordinal_ignore_case(a_normalizedPath, expectedRoot))
        {
            continue;
        }

        std::wstring candidateRoot;
        try
        {
            candidateRoot.assign(a_normalizedPath.substr(0U, expectedRoot.size()));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        UniqueHandle candidate(CreateFileW(candidateRoot.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                           nullptr));
        BY_HANDLE_FILE_INFORMATION information{};
        if (!candidate.is_valid() || GetFileInformationByHandle(candidate.get(), &information) == FALSE ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            information.dwVolumeSerialNumber != m_identity.volumeSerial ||
            information.nFileIndexHigh != m_identity.fileIndexHigh ||
            information.nFileIndexLow != m_identity.fileIndexLow)
        {
            continue;
        }
        return cue::Result<std::size_t>::success(expectedRoot.size());
    }

    return cue::Result<std::size_t>::failure(cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot,
                                                                "External workspace path is outside the bound root"));
}

cue::Result<std::unique_ptr<cue::WorkspaceWatcher>> WindowsWorkspaceFilesystem::create_watcher(
    const cue::WorkspaceDirectory &a_directory, cue::WorkspaceWatchLimits a_limits) noexcept
{
    if (!owns_directory(a_directory))
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::OutsideRoot, "Workspace watcher directory belongs to another root"));
    }
    cue::Result<std::vector<UniqueHandle>> pinned = pin_directory_chain(a_directory);
    if (!pinned)
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceWatcher>>::failure(std::move(*pinned.try_error()));
    }
    std::vector<HANDLE> expectedChain;
    try
    {
        expectedChain.reserve(pinned.try_value()->size() + 1U);
        expectedChain.push_back(m_rootHandle.get());
        for (const UniqueHandle &directory : *pinned.try_value())
        {
            expectedChain.push_back(directory.get());
        }
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    const cue::BoundWorkspacePath *locator = a_directory.locator();
    const std::string_view relativeDirectory = locator == nullptr ? std::string_view{} : locator->text();
    return create_windows_workspace_watcher(expectedChain, m_rootPath, relativeDirectory, a_limits, m_assertContext);
}

/// @brief 未検証Absolute Windows PathをRoot相対Portable Capabilityへ正規化する
cue::Result<cue::BoundWorkspacePath> WindowsWorkspaceFilesystem::bind_external_path(
    std::string_view a_unverifiedAbsolutePath) noexcept
{
    cue::Result<std::wstring> converted = to_utf16(a_unverifiedAbsolutePath, m_assertContext);
    if (!converted)
    {
        return cue::Result<cue::BoundWorkspacePath>::failure(std::move(*converted.try_error()));
    }
    const std::wstring_view input(*converted.try_value());
    if (input.empty() || input.find(L'\0') != std::wstring_view::npos || is_unc_path(input))
    {
        return cue::Result<cue::BoundWorkspacePath>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::InvalidPath, "External workspace path must be a local absolute path"));
    }

    const bool extendedPrefix = input.starts_with(L"\\\\?\\");
    const std::size_t driveOffset = extendedPrefix ? 4U : 0U;
    const bool driveAbsolute = input.size() >= driveOffset + 3U && std::iswalpha(input[driveOffset]) != 0 &&
                               input[driveOffset + 1U] == L':' &&
                               (input[driveOffset + 2U] == L'\\' || input[driveOffset + 2U] == L'/');
    if (!driveAbsolute)
    {
        return cue::Result<cue::BoundWorkspacePath>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::InvalidPath, "External workspace path must be local-drive absolute"));
    }

    const DWORD required = GetFullPathNameW(converted.try_value()->c_str(), 0U, nullptr, nullptr);
    if (required == 0U)
    {
        return cue::Result<cue::BoundWorkspacePath>::failure(
            make_windows_error(m_assertContext, GetLastError(), "External workspace path normalization failed"));
    }
    std::wstring absolute;
    try
    {
        absolute.resize(required);
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    const DWORD written = GetFullPathNameW(converted.try_value()->c_str(), required, absolute.data(), nullptr);
    if (written == 0U || written >= required)
    {
        return cue::Result<cue::BoundWorkspacePath>::failure(
            make_windows_error(m_assertContext, written == 0U ? GetLastError() : ERROR_INSUFFICIENT_BUFFER,
                               "External workspace path normalization failed"));
    }
    absolute.resize(written);
    const std::size_t rootLength = local_drive_root_length(absolute);
    while (absolute.size() > rootLength && (absolute.back() == L'\\' || absolute.back() == L'/'))
    {
        absolute.pop_back();
    }

    cue::Result<std::wstring> extended = make_extended_path(std::move(absolute), m_assertContext);
    if (!extended)
    {
        return cue::Result<cue::BoundWorkspacePath>::failure(std::move(*extended.try_error()));
    }
    const std::wstring_view normalized(*extended.try_value());
    cue::Result<std::size_t> rootPrefixLength = external_root_prefix_length(normalized);
    if (!rootPrefixLength)
    {
        return cue::Result<cue::BoundWorkspacePath>::failure(std::move(*rootPrefixLength.try_error()));
    }

    std::size_t relativeOffset = *rootPrefixLength.try_value();
    const bool rootEndsWithSeparator =
        normalized[relativeOffset - 1U] == L'\\' || normalized[relativeOffset - 1U] == L'/';
    if (!rootEndsWithSeparator)
    {
        if (normalized[relativeOffset] != L'\\' && normalized[relativeOffset] != L'/')
        {
            return cue::Result<cue::BoundWorkspacePath>::failure(cue::make_io_error(
                m_assertContext, cue::IoError::OutsideRoot, "External workspace path is outside the bound root"));
        }
        ++relativeOffset;
    }

    std::wstring relative(normalized.substr(relativeOffset));
    std::replace(relative.begin(), relative.end(), L'\\', L'/');
    cue::Result<std::string> portable = to_utf8(relative, m_assertContext);
    if (!portable)
    {
        return cue::Result<cue::BoundWorkspacePath>::failure(std::move(*portable.try_error()));
    }
    cue::WorkspaceDirectory parent = cue::WorkspaceDirectory::root();
    cue::Result<cue::BoundWorkspacePath> bound = cue::Result<cue::BoundWorkspacePath>::failure(
        cue::make_io_error(m_assertContext, cue::IoError::InvalidPath, "External workspace path is empty"));
    std::string_view remaining(*portable.try_value());
    while (!remaining.empty())
    {
        const std::size_t separator = remaining.find('/');
        const std::string_view segment = remaining.substr(0U, separator);
        cue::Result<cue::RelativePath> locator = cue::RelativePath::parse(segment, m_assertContext);
        if (!locator)
        {
            return cue::Result<cue::BoundWorkspacePath>::failure(std::move(*locator.try_error()));
        }
        bound = append_path(parent, std::move(*locator.try_value()), m_assertContext);
        if (!bound)
        {
            return bound;
        }
        parent = cue::WorkspaceDirectory::from_bound_path(*bound.try_value());
        remaining = separator == std::string_view::npos ? std::string_view{} : remaining.substr(separator + 1U);
    }
    return bound;
}

cue::Result<std::vector<UniqueHandle>> WindowsWorkspaceFilesystem::pin_directory_chain(
    const cue::WorkspaceDirectory &a_directory) const noexcept
{
    cue::Result<void> rootIdentity = verify_root_identity();
    if (!rootIdentity)
    {
        return cue::Result<std::vector<UniqueHandle>>::failure(std::move(*rootIdentity.try_error()));
    }

    std::vector<UniqueHandle> pinned;
    try
    {
        const cue::BoundWorkspacePath *locator = a_directory.locator();
        if (locator == nullptr)
        {
            return cue::Result<std::vector<UniqueHandle>>::success(std::move(pinned));
        }

        std::string_view remaining = locator->text();
        while (!remaining.empty())
        {
            const std::size_t separator = remaining.find('/');
            const std::string_view component = remaining.substr(0U, separator);
            cue::Result<std::wstring> nativeComponent = to_utf16(component, m_assertContext);
            if (!nativeComponent)
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(std::move(*nativeComponent.try_error()));
            }

            const HANDLE parentHandle = pinned.empty() ? m_rootHandle.get() : pinned.back().get();
            cue::Result<NativeDirectoryEnumeration> before =
                enumerate_directory_handle_stable(parentHandle, k_windowsHardLimits.maxVisitedEntries,
                                                  k_windowsHardLimits.maxMetadataBytes, m_assertContext);
            if (!before)
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(std::move(*before.try_error()));
            }
            if (before.try_value()->interruptedCode.has_value())
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(
                    make_windows_error(m_assertContext, *before.try_value()->interruptedCode,
                                       "Workspace directory chain enumeration was interrupted"));
            }

            const auto findComponent =
                [&](const NativeDirectoryEnumeration &a_enumeration) noexcept -> const NativeDirectoryEntry *
            {
                const NativeDirectoryEntry *match = nullptr;
                std::size_t matchingNames = 0U;
                for (const NativeDirectoryEntry &entry : a_enumeration.entries)
                {
                    if (CompareStringOrdinal(entry.name.data(), static_cast<int>(entry.name.size()),
                                             nativeComponent.try_value()->data(),
                                             static_cast<int>(nativeComponent.try_value()->size()), TRUE) == CSTR_EQUAL)
                    {
                        ++matchingNames;
                        if (entry.name == *nativeComponent.try_value())
                        {
                            match = &entry;
                        }
                    }
                }
                return matchingNames == 1U ? match : nullptr;
            };

            const NativeDirectoryEntry *entry = findComponent(*before.try_value());
            if (entry == nullptr)
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(cue::make_io_error(
                    m_assertContext, cue::IoError::NotFound, "Workspace directory component was not found"));
            }
            if ((entry->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(
                    cue::make_io_error(m_assertContext, cue::IoError::UnsupportedEntry,
                                       "Workspace directory chain contains a reparse point"));
            }
            if ((entry->attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(cue::make_io_error(
                    m_assertContext, cue::IoError::TypeMismatch, "Workspace directory chain contains a file"));
            }

            const LARGE_INTEGER expectedId = entry->fileId;
            const DWORD expectedAttributes = entry->attributes;
            FILE_ID_DESCRIPTOR descriptor{};
            descriptor.dwSize = sizeof(descriptor);
            descriptor.Type = FileIdType;
            descriptor.FileId = expectedId;
            UniqueHandle handle(OpenFileById(parentHandle, &descriptor, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
            if (!handle.is_valid())
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(
                    make_windows_error(m_assertContext, GetLastError(), "Workspace directory open failed"));
            }
            BY_HANDLE_FILE_INFORMATION information{};
            if (GetFileInformationByHandle(handle.get(), &information) == FALSE)
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(
                    make_windows_error(m_assertContext, GetLastError(), "Workspace directory inspection failed"));
            }
            LARGE_INTEGER actualId{};
            actualId.HighPart = static_cast<LONG>(information.nFileIndexHigh);
            actualId.LowPart = information.nFileIndexLow;
            const DWORD typeMask = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
            if (!same_file_id(actualId, expectedId) ||
                (information.dwFileAttributes & typeMask) != (expectedAttributes & typeMask))
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(cue::make_io_error(
                    m_assertContext, cue::IoError::OutsideRoot, "Workspace directory component identity changed"));
            }
            if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(
                    cue::make_io_error(m_assertContext, cue::IoError::UnsupportedEntry,
                                       "Workspace directory chain contains a reparse point"));
            }
            if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(cue::make_io_error(
                    m_assertContext, cue::IoError::TypeMismatch, "Workspace directory chain contains a file"));
            }

            cue::Result<NativeDirectoryEnumeration> after =
                enumerate_directory_handle_stable(parentHandle, k_windowsHardLimits.maxVisitedEntries,
                                                  k_windowsHardLimits.maxMetadataBytes, m_assertContext);
            if (!after)
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(std::move(*after.try_error()));
            }
            if (after.try_value()->interruptedCode.has_value())
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(
                    make_windows_error(m_assertContext, *after.try_value()->interruptedCode,
                                       "Workspace directory chain verification was interrupted"));
            }
            const NativeDirectoryEntry *verified = findComponent(*after.try_value());
            if (verified == nullptr || !same_file_id(verified->fileId, expectedId) ||
                (verified->attributes & typeMask) != (expectedAttributes & typeMask))
            {
                return cue::Result<std::vector<UniqueHandle>>::failure(cue::make_io_error(
                    m_assertContext, cue::IoError::OutsideRoot, "Workspace directory component identity changed"));
            }
            pinned.push_back(std::move(handle));
            if (separator == std::string_view::npos)
            {
                break;
            }
            remaining.remove_prefix(separator + 1U);
        }
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    return cue::Result<std::vector<UniqueHandle>>::success(std::move(pinned));
}

cue::Result<std::vector<UniqueHandle>> WindowsWorkspaceFilesystem::prepare_mutation_parent(
    const cue::BoundWorkspacePath &a_destination) const noexcept
{
    if (!owns_path(a_destination))
    {
        return cue::Result<std::vector<UniqueHandle>>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::OutsideRoot, "Workspace path belongs to another root binding"));
    }
    cue::Result<cue::WorkspaceDirectory> parent = parent_directory(a_destination, m_assertContext);
    if (!parent)
    {
        return cue::Result<std::vector<UniqueHandle>>::failure(std::move(*parent.try_error()));
    }
    cue::Result<std::vector<UniqueHandle>> pinned = pin_directory_chain(*parent.try_value());
    if (!pinned)
    {
        return cue::Result<std::vector<UniqueHandle>>::failure(std::move(*pinned.try_error()));
    }
    if (pinned.try_value()->empty() || parent.try_value()->locator() == nullptr)
    {
        return cue::Result<std::vector<UniqueHandle>>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::PreconditionFailed, "Workspace root is not a mutation area"));
    }

    cue::Result<RootIdentity> expected = read_entry_identity(pinned.try_value()->back().get(), m_assertContext);
    if (!expected)
    {
        return cue::Result<std::vector<UniqueHandle>>::failure(std::move(*expected.try_error()));
    }
    cue::Result<std::wstring> nativeParent =
        make_native_workspace_path(m_rootPath, parent.try_value()->locator()->text(), m_assertContext);
    if (!nativeParent)
    {
        return cue::Result<std::vector<UniqueHandle>>::failure(std::move(*nativeParent.try_error()));
    }

    pinned.try_value()->back().reset();
    UniqueHandle mutationParent(CreateFileW(nativeParent.try_value()->c_str(),
                                            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!mutationParent.is_valid())
    {
        return cue::Result<std::vector<UniqueHandle>>::failure(
            make_windows_error(m_assertContext, GetLastError(), "Workspace mutation parent open failed"));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(mutationParent.get(), &information) == FALSE)
    {
        return cue::Result<std::vector<UniqueHandle>>::failure(
            make_windows_error(m_assertContext, GetLastError(), "Workspace mutation parent inspection failed"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        information.dwVolumeSerialNumber != expected.try_value()->volumeSerial ||
        information.nFileIndexHigh != expected.try_value()->fileIndexHigh ||
        information.nFileIndexLow != expected.try_value()->fileIndexLow)
    {
        return cue::Result<std::vector<UniqueHandle>>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::OutsideRoot, "Workspace mutation parent identity changed"));
    }
    pinned.try_value()->back() = std::move(mutationParent);
    return cue::Result<std::vector<UniqueHandle>>::success(std::move(*pinned.try_value()));
}

cue::Result<void> WindowsWorkspaceFilesystem::verify_mutation_parent_chain(
    const cue::BoundWorkspacePath &a_destination, const std::vector<UniqueHandle> &a_expected) const noexcept
{
    cue::Result<cue::WorkspaceDirectory> parent = parent_directory(a_destination, m_assertContext);
    if (!parent)
    {
        return cue::Result<void>::failure(std::move(*parent.try_error()));
    }
    cue::Result<std::vector<UniqueHandle>> current = pin_directory_chain(*parent.try_value());
    if (!current)
    {
        return cue::Result<void>::failure(std::move(*current.try_error()));
    }
    if (current.try_value()->size() != a_expected.size())
    {
        return cue::Result<void>::failure(cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot,
                                                             "Workspace mutation parent chain length changed"));
    }

    for (std::size_t index = 0U; index < a_expected.size(); ++index)
    {
        cue::Result<RootIdentity> expected = read_entry_identity(a_expected[index].get(), m_assertContext);
        cue::Result<RootIdentity> observed = read_entry_identity((*current.try_value())[index].get(), m_assertContext);
        if (!expected)
        {
            return cue::Result<void>::failure(std::move(*expected.try_error()));
        }
        if (!observed)
        {
            return cue::Result<void>::failure(std::move(*observed.try_error()));
        }
        if (expected.try_value()->volumeSerial != observed.try_value()->volumeSerial ||
            expected.try_value()->fileIndexHigh != observed.try_value()->fileIndexHigh ||
            expected.try_value()->fileIndexLow != observed.try_value()->fileIndexLow)
        {
            return cue::Result<void>::failure(cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot,
                                                                 "Workspace mutation parent chain identity changed"));
        }
    }
    return cue::Result<void>::success();
}

cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>> WindowsWorkspaceFilesystem::
    begin_mutation_parent_guards(const std::vector<UniqueHandle> &a_pinned,
                                 std::span<const RootIdentity> a_excludedDirectories) const noexcept
{
    if (a_pinned.empty())
    {
        return cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::PreconditionFailed, "Workspace mutation parent chain is empty"));
    }

    std::vector<std::unique_ptr<DirectoryChangeGuard>> guards;
    try
    {
        guards.reserve(a_pinned.size());
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }

    for (std::size_t index = 0U; index < a_pinned.size(); ++index)
    {
        const HANDLE directory = index == 0U ? m_rootHandle.get() : a_pinned[index - 1U].get();
        cue::Result<RootIdentity> identity = read_entry_identity(directory, m_assertContext);
        if (!identity)
        {
            return cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>>::failure(
                std::move(*identity.try_error()));
        }
        if (std::ranges::any_of(a_excludedDirectories,
                                /// @brief 現在DirectoryがMutation対象の直接親か判定する
                                [&](const RootIdentity &a_excluded) noexcept
                                { return same_identity(*identity.try_value(), a_excluded); }))
        {
            continue;
        }
        cue::Result<std::unique_ptr<DirectoryChangeGuard>> guard =
            begin_directory_change_guard(directory, m_assertContext);
        if (!guard)
        {
            return cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>>::failure(
                std::move(*guard.try_error()));
        }
        guards.push_back(std::move(*guard.try_value()));
    }
    return cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>>::success(std::move(guards));
}

cue::Result<void> WindowsWorkspaceFilesystem::finish_mutation_parent_guards(
    std::vector<std::unique_ptr<DirectoryChangeGuard>> &a_guards) const noexcept
{
    std::optional<cue::Error> firstError;
    for (std::unique_ptr<DirectoryChangeGuard> &guard : a_guards)
    {
        cue::Result<void> finished = finish_directory_change_guard(*guard, m_assertContext);
        if (!finished && !firstError.has_value())
        {
            firstError.emplace(std::move(*finished.try_error()));
        }
    }
    if (firstError.has_value())
    {
        return cue::Result<void>::failure(std::move(*firstError));
    }
    return cue::Result<void>::success();
}

cue::Result<void> WindowsWorkspaceFilesystem::verify_mutation_parent_guards_pending(
    std::vector<std::unique_ptr<DirectoryChangeGuard>> &a_guards) const noexcept
{
    for (std::unique_ptr<DirectoryChangeGuard> &guard : a_guards)
    {
        cue::Result<void> pending = verify_directory_change_guard_pending(*guard, m_assertContext);
        if (!pending)
        {
            return cue::Result<void>::failure(std::move(*pending.try_error()));
        }
    }
    return cue::Result<void>::success();
}

cue::Result<void> WindowsWorkspaceFilesystem::verify_directory(const cue::WorkspaceDirectory &a_directory) noexcept
{
    if (!owns_directory(a_directory))
    {
        return cue::Result<void>::failure(cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot,
                                                             "Workspace directory belongs to another root binding"));
    }
    cue::Result<std::vector<UniqueHandle>> pinned = pin_directory_chain(a_directory);
    if (!pinned)
    {
        return cue::Result<void>::failure(std::move(*pinned.try_error()));
    }
    if (a_directory.locator() == nullptr)
    {
        return cue::Result<void>::success();
    }
    if (pinned.try_value()->empty())
    {
        return cue::Result<void>::failure(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                             "Workspace directory verification was incomplete"));
    }

    cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>> guards =
        begin_mutation_parent_guards(*pinned.try_value());
    if (!guards)
    {
        return cue::Result<void>::failure(std::move(*guards.try_error()));
    }

    std::optional<cue::Error> verificationError;
    cue::Result<std::vector<UniqueHandle>> current = pin_directory_chain(a_directory);
    if (!current)
    {
        verificationError.emplace(std::move(*current.try_error()));
    }
    else if (current.try_value()->size() != pinned.try_value()->size())
    {
        verificationError.emplace(
            cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot, "Workspace directory chain length changed"));
    }
    else
    {
        for (std::size_t index = 0U; index < pinned.try_value()->size(); ++index)
        {
            cue::Result<RootIdentity> expected =
                read_entry_identity((*pinned.try_value())[index].get(), m_assertContext);
            cue::Result<RootIdentity> observed =
                read_entry_identity((*current.try_value())[index].get(), m_assertContext);
            if (!expected)
            {
                verificationError.emplace(std::move(*expected.try_error()));
                break;
            }
            if (!observed)
            {
                verificationError.emplace(std::move(*observed.try_error()));
                break;
            }
            if (expected.try_value()->volumeSerial != observed.try_value()->volumeSerial ||
                expected.try_value()->fileIndexHigh != observed.try_value()->fileIndexHigh ||
                expected.try_value()->fileIndexLow != observed.try_value()->fileIndexLow)
            {
                verificationError.emplace(cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot,
                                                             "Workspace directory chain identity changed"));
                break;
            }
        }
    }

    cue::Result<void> guardsFinished = finish_mutation_parent_guards(*guards.try_value());
    if (verificationError.has_value())
    {
        return cue::Result<void>::failure(std::move(*verificationError));
    }
    if (!guardsFinished)
    {
        return cue::Result<void>::failure(std::move(*guardsFinished.try_error()));
    }
    return cue::Result<void>::success();
}

cue::Result<cue::FilesystemIdentity> WindowsWorkspaceFilesystem::directory_identity(
    const cue::WorkspaceDirectory &a_directory) noexcept
{
    if (!owns_directory(a_directory))
    {
        return cue::Result<cue::FilesystemIdentity>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::OutsideRoot, "Workspace directory belongs to another root binding"));
    }

    cue::Result<std::vector<UniqueHandle>> pinned = pin_directory_chain(a_directory);
    if (!pinned)
    {
        return cue::Result<cue::FilesystemIdentity>::failure(std::move(*pinned.try_error()));
    }

    RootIdentity identity = m_identity;
    if (a_directory.locator() != nullptr)
    {
        if (pinned.try_value()->empty())
        {
            return cue::Result<cue::FilesystemIdentity>::failure(
                cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                   "Workspace directory identity inspection was incomplete"));
        }
        cue::Result<RootIdentity> inspected = read_entry_identity(pinned.try_value()->back().get(), m_assertContext);
        if (!inspected)
        {
            return cue::Result<cue::FilesystemIdentity>::failure(std::move(*inspected.try_error()));
        }
        identity = *inspected.try_value();
    }

    constexpr std::uint64_t k_windowsIdentityProvider = 0x57494E3100000000ULL;
    const std::uint64_t providerScope = k_windowsIdentityProvider | static_cast<std::uint64_t>(identity.volumeSerial);
    const std::uint64_t entry =
        (static_cast<std::uint64_t>(identity.fileIndexHigh) << 32U) | static_cast<std::uint64_t>(identity.fileIndexLow);
    return cue::Result<cue::FilesystemIdentity>::success(make_filesystem_identity(providerScope, entry));
}

cue::Result<std::vector<std::byte>> WindowsWorkspaceFilesystem::read_file_bounded(
    const cue::BoundWorkspacePath &a_source, std::size_t a_maxBytes) noexcept
{
    if (!owns_path(a_source))
    {
        return cue::Result<std::vector<std::byte>>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::OutsideRoot, "Workspace file belongs to another root binding"));
    }
    if (a_maxBytes == 0U)
    {
        return cue::Result<std::vector<std::byte>>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::CapacityExceeded, "Workspace file read limit must be non-zero"));
    }

    cue::Result<cue::WorkspaceDirectory> parent = parent_directory(a_source, m_assertContext);
    if (!parent)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*parent.try_error()));
    }
    cue::Result<std::vector<UniqueHandle>> pinned = pin_directory_chain(*parent.try_value());
    if (!pinned)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*pinned.try_error()));
    }
    HANDLE parentHandle = pinned.try_value()->empty() ? m_rootHandle.get() : pinned.try_value()->back().get();

    const std::size_t separator = a_source.text().rfind('/');
    const std::string_view name =
        separator == std::string_view::npos ? a_source.text() : a_source.text().substr(separator + 1U);
    cue::Result<std::wstring> nativeName = to_utf16(name, m_assertContext);
    if (!nativeName)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*nativeName.try_error()));
    }
    cue::Result<NativeDirectoryEnumeration> enumeration = enumerate_directory_handle_stable(
        parentHandle, k_windowsHardLimits.maxVisitedEntries, k_windowsHardLimits.maxMetadataBytes, m_assertContext);
    if (!enumeration)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*enumeration.try_error()));
    }
    if (enumeration.try_value()->interruptedCode.has_value())
    {
        return cue::Result<std::vector<std::byte>>::failure(
            make_windows_error(m_assertContext, *enumeration.try_value()->interruptedCode,
                               "Workspace file parent enumeration was interrupted"));
    }

    const NativeDirectoryEntry *matched = nullptr;
    std::size_t matchingNames = 0U;
    for (const NativeDirectoryEntry &entry : enumeration.try_value()->entries)
    {
        const int comparison =
            CompareStringOrdinal(entry.name.data(), static_cast<int>(entry.name.size()), nativeName.try_value()->data(),
                                 static_cast<int>(nativeName.try_value()->size()), TRUE);
        if (comparison == 0)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                make_windows_error(m_assertContext, GetLastError(), "Workspace file name comparison failed"));
        }
        if (comparison == CSTR_EQUAL)
        {
            ++matchingNames;
            if (entry.name == *nativeName.try_value())
            {
                matched = &entry;
            }
        }
    }
    if (matchingNames != 1U || matched == nullptr)
    {
        const cue::IoError code = matchingNames == 0U ? cue::IoError::NotFound : cue::IoError::PreconditionFailed;
        return cue::Result<std::vector<std::byte>>::failure(
            cue::make_io_error(m_assertContext, code, "Workspace file name is missing or ambiguous"));
    }
    if ((matched->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return cue::Result<std::vector<std::byte>>::failure(
            cue::make_io_error(m_assertContext, cue::IoError::UnsupportedEntry, "Workspace file is a reparse point"));
    }
    if ((matched->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
    {
        return cue::Result<std::vector<std::byte>>::failure(
            cue::make_io_error(m_assertContext, cue::IoError::TypeMismatch, "Workspace file is a directory"));
    }

    FILE_ID_DESCRIPTOR descriptor{};
    descriptor.dwSize = sizeof(descriptor);
    descriptor.Type = FileIdType;
    descriptor.FileId = matched->fileId;
    UniqueHandle file(OpenFileById(parentHandle, &descriptor, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                                   nullptr, FILE_FLAG_OPEN_REPARSE_POINT));
    if (!file.is_valid())
    {
        return cue::Result<std::vector<std::byte>>::failure(
            make_windows_error(m_assertContext, GetLastError(), "Workspace file snapshot open failed"));
    }

    BY_HANDLE_FILE_INFORMATION information{};
    LARGE_INTEGER size{};
    if (GetFileInformationByHandle(file.get(), &information) == FALSE || GetFileSizeEx(file.get(), &size) == FALSE)
    {
        return cue::Result<std::vector<std::byte>>::failure(
            make_windows_error(m_assertContext, GetLastError(), "Workspace file snapshot inspection failed"));
    }
    LARGE_INTEGER actualId{};
    actualId.HighPart = static_cast<LONG>(information.nFileIndexHigh);
    actualId.LowPart = information.nFileIndexLow;
    if (!same_file_id(actualId, matched->fileId) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
    {
        return cue::Result<std::vector<std::byte>>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::PreconditionFailed, "Workspace file identity or type changed"));
    }

    cue::Result<NativeDirectoryEnumeration> verifiedParent = enumerate_directory_handle_stable(
        parentHandle, k_windowsHardLimits.maxVisitedEntries, k_windowsHardLimits.maxMetadataBytes, m_assertContext);
    if (!verifiedParent)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*verifiedParent.try_error()));
    }
    if (verifiedParent.try_value()->interruptedCode.has_value())
    {
        return cue::Result<std::vector<std::byte>>::failure(
            make_windows_error(m_assertContext, *verifiedParent.try_value()->interruptedCode,
                               "Workspace file identity verification was interrupted"));
    }
    matchingNames = 0U;
    bool identityStillBoundToName = false;
    for (const NativeDirectoryEntry &entry : verifiedParent.try_value()->entries)
    {
        const int comparison =
            CompareStringOrdinal(entry.name.data(), static_cast<int>(entry.name.size()), nativeName.try_value()->data(),
                                 static_cast<int>(nativeName.try_value()->size()), TRUE);
        if (comparison == 0)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                make_windows_error(m_assertContext, GetLastError(), "Workspace file identity name comparison failed"));
        }
        if (comparison == CSTR_EQUAL)
        {
            ++matchingNames;
            identityStillBoundToName = identityStillBoundToName ||
                                       (entry.name == *nativeName.try_value() && same_file_id(entry.fileId, actualId) &&
                                        (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U &&
                                        (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U);
        }
    }
    if (matchingNames != 1U || !identityStillBoundToName)
    {
        return cue::Result<std::vector<std::byte>>::failure(
            cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                               "Workspace file identity is no longer bound to the requested parent entry"));
    }
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > a_maxBytes)
    {
        return cue::Result<std::vector<std::byte>>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::CapacityExceeded, "Workspace file exceeds the read limit"));
    }

    std::vector<std::byte> bytes;
    try
    {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    std::size_t offset = 0U;
    while (offset < bytes.size())
    {
        const DWORD requested = static_cast<DWORD>(
            std::min(bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD read = 0U;
        if (ReadFile(file.get(), bytes.data() + offset, requested, &read, nullptr) == FALSE || read != requested)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                make_windows_error(m_assertContext, GetLastError(), "Workspace file snapshot read failed"));
        }
        offset += read;
    }
    return cue::Result<std::vector<std::byte>>::success(std::move(bytes));
}

cue::Result<cue::WorkspaceEntryFingerprint> WindowsWorkspaceFilesystem::fingerprint_entry_once(
    const cue::BoundWorkspacePath &a_source, cue::TraversalLimits a_traversalLimits,
    cue::ContentVerificationLimits a_contentLimits) noexcept
{
    if (!owns_path(a_source))
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::OutsideRoot, "Workspace fingerprint path belongs to another root binding"));
    }
    if (!a_traversalLimits.is_valid() || !a_contentLimits.is_valid())
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::CapacityExceeded, "Workspace fingerprint limits must all be non-zero"));
    }

    const cue::TraversalLimits traversal{
        std::min(a_traversalLimits.maxDepth, k_windowsHardLimits.maxDepth),
        std::min(a_traversalLimits.maxVisitedEntries, k_windowsHardLimits.maxVisitedEntries),
        std::min(a_traversalLimits.maxResults, k_windowsHardLimits.maxResults),
        std::min(a_traversalLimits.maxMetadataBytes, k_windowsHardLimits.maxMetadataBytes)};
    const cue::ContentVerificationLimits content{
        std::min(a_contentLimits.maxFileBytes, k_windowsContentHardLimits.maxFileBytes),
        std::min(a_contentLimits.maxTotalBytes, k_windowsContentHardLimits.maxTotalBytes)};

    /// @brief 単一LinkのRegular File内容を上限内でFingerprint化する
    const auto fingerprintFile = [&](const cue::BoundWorkspacePath &a_file,
                                     std::uint64_t &a_totalBytes) noexcept -> cue::Result<cue::WorkspaceFileFingerprint>
    {
        cue::Result<cue::WorkspaceDirectory> parent = parent_directory(a_file, m_assertContext);
        if (!parent)
        {
            return cue::Result<cue::WorkspaceFileFingerprint>::failure(std::move(*parent.try_error()));
        }
        cue::Result<std::vector<UniqueHandle>> pinned = pin_directory_chain(*parent.try_value());
        if (!pinned)
        {
            return cue::Result<cue::WorkspaceFileFingerprint>::failure(std::move(*pinned.try_error()));
        }
        const HANDLE parentHandle = pinned.try_value()->empty() ? m_rootHandle.get() : pinned.try_value()->back().get();
        const std::size_t separator = a_file.text().rfind('/');
        const std::string_view leaf =
            separator == std::string_view::npos ? a_file.text() : a_file.text().substr(separator + 1U);
        cue::Result<std::wstring> nativeLeaf = to_utf16(leaf, m_assertContext);
        if (!nativeLeaf)
        {
            return cue::Result<cue::WorkspaceFileFingerprint>::failure(std::move(*nativeLeaf.try_error()));
        }
        cue::Result<OpenedNativeEntry> opened =
            open_exact_entry(parentHandle, *nativeLeaf.try_value(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_DELETE, m_assertContext);
        if (!opened)
        {
            return cue::Result<cue::WorkspaceFileFingerprint>::failure(std::move(*opened.try_error()));
        }
        if (opened.try_value()->isDirectory)
        {
            return cue::Result<cue::WorkspaceFileFingerprint>::failure(cue::make_io_error(
                m_assertContext, cue::IoError::TypeMismatch, "Workspace fingerprint file became a directory"));
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(opened.try_value()->handle.get(), &information) == FALSE)
        {
            return cue::Result<cue::WorkspaceFileFingerprint>::failure(
                make_windows_error(m_assertContext, GetLastError(), "Workspace fingerprint file inspection failed"));
        }
        if (information.nNumberOfLinks != 1U)
        {
            return cue::Result<cue::WorkspaceFileFingerprint>::failure(cue::make_io_error(
                m_assertContext, cue::IoError::UnsupportedEntry, "Workspace recovery file has multiple hard links"));
        }
        ULARGE_INTEGER size{};
        size.HighPart = information.nFileSizeHigh;
        size.LowPart = information.nFileSizeLow;
        if (size.QuadPart > content.maxFileBytes || size.QuadPart > content.maxTotalBytes - a_totalBytes ||
            size.QuadPart > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        {
            return cue::Result<cue::WorkspaceFileFingerprint>::failure(cue::make_io_error(
                m_assertContext, cue::IoError::CapacityExceeded, "Workspace fingerprint content limit was exceeded"));
        }
        std::vector<std::byte> bytes;
        try
        {
            bytes.resize(static_cast<std::size_t>(size.QuadPart));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        std::size_t offset = 0U;
        while (offset < bytes.size())
        {
            const DWORD requested = static_cast<DWORD>(
                std::min(bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD read = 0U;
            if (ReadFile(opened.try_value()->handle.get(), bytes.data() + offset, requested, &read, nullptr) == FALSE ||
                read != requested)
            {
                return cue::Result<cue::WorkspaceFileFingerprint>::failure(
                    make_windows_error(m_assertContext, GetLastError(), "Workspace fingerprint file read failed"));
            }
            offset += read;
        }
        LARGE_INTEGER rewind{};
        if (SetFilePointerEx(opened.try_value()->handle.get(), rewind, nullptr, FILE_BEGIN) == FALSE)
        {
            return cue::Result<cue::WorkspaceFileFingerprint>::failure(
                make_windows_error(m_assertContext, GetLastError(), "Workspace fingerprint file rewind failed"));
        }
        a_totalBytes += size.QuadPart;
        return cue::Result<cue::WorkspaceFileFingerprint>::success(
            cue::WorkspaceFileFingerprint{size.QuadPart, content_digest(bytes)});
    };

    cue::Result<cue::WorkspaceDirectory> sourceParent = parent_directory(a_source, m_assertContext);
    if (!sourceParent)
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(std::move(*sourceParent.try_error()));
    }
    cue::Result<std::vector<UniqueHandle>> sourcePinned = pin_directory_chain(*sourceParent.try_value());
    if (!sourcePinned)
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(std::move(*sourcePinned.try_error()));
    }
    const HANDLE sourceParentHandle =
        sourcePinned.try_value()->empty() ? m_rootHandle.get() : sourcePinned.try_value()->back().get();
    const std::size_t sourceSeparator = a_source.text().rfind('/');
    const std::string_view sourceLeaf =
        sourceSeparator == std::string_view::npos ? a_source.text() : a_source.text().substr(sourceSeparator + 1U);
    cue::Result<std::wstring> sourceName = to_utf16(sourceLeaf, m_assertContext);
    if (!sourceName)
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(std::move(*sourceName.try_error()));
    }
    cue::Result<OpenedNativeEntry> source =
        open_exact_entry(sourceParentHandle, *sourceName.try_value(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, m_assertContext);
    if (!source)
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(std::move(*source.try_error()));
    }

    cue::WorkspaceEntryFingerprint result;
    std::uint64_t totalBytes = 0U;
    if (!source.try_value()->isDirectory)
    {
        cue::Result<cue::WorkspaceFileFingerprint> file = fingerprintFile(a_source, totalBytes);
        if (!file)
        {
            return cue::Result<cue::WorkspaceEntryFingerprint>::failure(std::move(*file.try_error()));
        }
        result.type = cue::WorkspaceEntryType::RegularFile;
        result.file = *file.try_value();
        return cue::Result<cue::WorkspaceEntryFingerprint>::success(std::move(result));
    }

    cue::Result<std::unique_ptr<DirectoryChangeGuard>> changeGuard = begin_directory_change_guard(
        source.try_value()->handle.get(), m_assertContext, true,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION);
    if (!changeGuard)
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(std::move(*changeGuard.try_error()));
    }
    cue::WorkspaceDirectory sourceDirectory = cue::WorkspaceDirectory::from_bound_path(a_source);
    cue::Result<cue::WorkspaceSearchResult> tree =
        cue::search_workspace(*this, sourceDirectory, {}, traversal, m_assertContext);
    if (!tree || tree.try_value()->state != cue::WorkspaceSnapshotState::Complete)
    {
        cue::Result<void> ignored = finish_directory_change_guard(**changeGuard.try_value(), m_assertContext);
        (void)ignored;
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(
            !tree ? std::move(*tree.try_error())
                  : cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                       "Workspace fingerprint tree requires a rescan"));
    }

    result.type = cue::WorkspaceEntryType::Directory;
    try
    {
        result.manifest.reserve(tree.try_value()->entries.size());
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    for (const cue::WorkspaceEntry &entry : tree.try_value()->entries)
    {
        if (!entry.is_operable() || !entry.locator.has_value() ||
            entry.locator->text().size() <= a_source.text().size() ||
            !entry.locator->text().starts_with(a_source.text()) || entry.locator->text()[a_source.text().size()] != '/')
        {
            cue::Result<void> ignored = finish_directory_change_guard(**changeGuard.try_value(), m_assertContext);
            (void)ignored;
            return cue::Result<cue::WorkspaceEntryFingerprint>::failure(
                cue::make_io_error(m_assertContext, cue::IoError::UnsupportedEntry,
                                   "Workspace fingerprint tree contains an unsupported entry"));
        }
        cue::WorkspaceManifestEntry manifestEntry;
        try
        {
            manifestEntry.path.assign(entry.locator->text().substr(a_source.text().size() + 1U));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        manifestEntry.type = entry.type;
        if (entry.type == cue::WorkspaceEntryType::RegularFile)
        {
            cue::Result<cue::WorkspaceFileFingerprint> file = fingerprintFile(*entry.locator, totalBytes);
            if (!file)
            {
                cue::Result<void> ignored = finish_directory_change_guard(**changeGuard.try_value(), m_assertContext);
                (void)ignored;
                return cue::Result<cue::WorkspaceEntryFingerprint>::failure(std::move(*file.try_error()));
            }
            manifestEntry.file = *file.try_value();
        }
        else if (entry.type != cue::WorkspaceEntryType::Directory)
        {
            cue::Result<void> ignored = finish_directory_change_guard(**changeGuard.try_value(), m_assertContext);
            (void)ignored;
            return cue::Result<cue::WorkspaceEntryFingerprint>::failure(
                cue::make_io_error(m_assertContext, cue::IoError::UnsupportedEntry,
                                   "Workspace fingerprint tree contains an unsupported type"));
        }
        try
        {
            result.manifest.push_back(std::move(manifestEntry));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
    }

    try
    {
        std::sort(result.manifest.begin(), result.manifest.end(), manifest_entry_less);
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    cue::Result<void> stable = finish_directory_change_guard(**changeGuard.try_value(), m_assertContext);
    if (!stable)
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(std::move(*stable.try_error()));
    }
    return cue::Result<cue::WorkspaceEntryFingerprint>::success(std::move(result));
}

cue::Result<cue::WorkspaceEntryFingerprint> WindowsWorkspaceFilesystem::fingerprint_entry(
    const cue::BoundWorkspacePath &a_source, cue::TraversalLimits a_traversalLimits,
    cue::ContentVerificationLimits a_contentLimits) noexcept
{
    return fingerprint_entry_once(a_source, a_traversalLimits, a_contentLimits);
}

cue::Result<cue::WorkspaceEntry> WindowsWorkspaceFilesystem::make_entry(
    const cue::WorkspaceDirectory &a_directory, HANDLE a_directoryHandle, const NativeDirectoryEntry &a_data,
    std::uint64_t a_generation, cue::DirectorySnapshot &a_snapshot) const noexcept
{
    const std::wstring_view nativeName(a_data.name);
    cue::WorkspaceEntry entry;
    entry.parentGeneration = a_generation;

    cue::Result<std::string> convertedName = to_utf8(nativeName, m_assertContext);
    if (!convertedName)
    {
        entry.displayName = make_native_name_fallback(nativeName, m_assertContext);
        copy_display_sort_key(entry, m_assertContext);
        entry.type = cue::WorkspaceEntryType::UnsupportedEntry;
        entry.rejection = cue::WorkspaceDiagnosticCode::UnsupportedName;
        return cue::Result<cue::WorkspaceEntry>::success(std::move(entry));
    }
    entry.displayName = std::move(*convertedName.try_value());

    cue::Result<cue::RelativePath> childLocator = cue::RelativePath::parse(entry.displayName, m_assertContext);
    if (!childLocator)
    {
        copy_display_sort_key(entry, m_assertContext);
        entry.type = cue::WorkspaceEntryType::UnsupportedEntry;
        entry.rejection = cue::WorkspaceDiagnosticCode::UnsupportedName;
        return cue::Result<cue::WorkspaceEntry>::success(std::move(entry));
    }

    FILE_ID_DESCRIPTOR descriptor{};
    descriptor.dwSize = sizeof(descriptor);
    descriptor.Type = FileIdType;
    descriptor.FileId = a_data.fileId;
    UniqueHandle child(OpenFileById(a_directoryHandle, &descriptor, FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS));
    if (!child.is_valid())
    {
        const DWORD code = GetLastError();
        cue::WorkspaceDiagnosticCode diagnosticCode = cue::WorkspaceDiagnosticCode::EnumerationFailed;
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        {
            diagnosticCode = cue::WorkspaceDiagnosticCode::EntryDisappeared;
        }
        else if (classify_windows_error(code) == cue::IoError::PermissionDenied)
        {
            diagnosticCode = cue::WorkspaceDiagnosticCode::PermissionDenied;
        }
        copy_display_sort_key(entry, m_assertContext);
        entry.type = cue::WorkspaceEntryType::UnsupportedEntry;
        entry.rejection = diagnosticCode;
        a_snapshot.state = cue::WorkspaceSnapshotState::RescanRequired;
        append_diagnostic(a_snapshot, diagnosticCode, entry.displayName, static_cast<std::int64_t>(code),
                          m_assertContext);
        return cue::Result<cue::WorkspaceEntry>::success(std::move(entry));
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(child.get(), &information) == FALSE)
    {
        const DWORD code = GetLastError();
        copy_display_sort_key(entry, m_assertContext);
        entry.type = cue::WorkspaceEntryType::UnsupportedEntry;
        entry.rejection = cue::WorkspaceDiagnosticCode::EnumerationFailed;
        a_snapshot.state = cue::WorkspaceSnapshotState::RescanRequired;
        append_diagnostic(a_snapshot, cue::WorkspaceDiagnosticCode::EnumerationFailed, entry.displayName,
                          static_cast<std::int64_t>(code), m_assertContext);
        return cue::Result<cue::WorkspaceEntry>::success(std::move(entry));
    }
    const bool findDirectory = (a_data.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool findReparse = (a_data.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    const bool actualDirectory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool actualReparse = (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (findDirectory != actualDirectory || findReparse != actualReparse)
    {
        const cue::WorkspaceDiagnosticCode diagnosticCode =
            actualReparse ? cue::WorkspaceDiagnosticCode::ReparsePoint : cue::WorkspaceDiagnosticCode::TypeChanged;
        copy_display_sort_key(entry, m_assertContext);
        entry.type = cue::WorkspaceEntryType::UnsupportedEntry;
        entry.rejection = diagnosticCode;
        a_snapshot.state = cue::WorkspaceSnapshotState::RescanRequired;
        append_diagnostic(a_snapshot, diagnosticCode, entry.displayName, 0, m_assertContext);
        return cue::Result<cue::WorkspaceEntry>::success(std::move(entry));
    }
    if (actualReparse)
    {
        copy_display_sort_key(entry, m_assertContext);
        entry.type = cue::WorkspaceEntryType::UnsupportedEntry;
        entry.rejection = cue::WorkspaceDiagnosticCode::ReparsePoint;
        return cue::Result<cue::WorkspaceEntry>::success(std::move(entry));
    }

    entry.sortKey = childLocator.try_value()->comparison_key(m_assertContext);
    cue::Result<cue::BoundWorkspacePath> locator =
        append_path(a_directory, std::move(*childLocator.try_value()), m_assertContext);
    if (!locator)
    {
        return cue::Result<cue::WorkspaceEntry>::failure(std::move(*locator.try_error()));
    }
    entry.locator = std::move(*locator.try_value());
    entry.type = actualDirectory ? cue::WorkspaceEntryType::Directory : cue::WorkspaceEntryType::RegularFile;
    if (!actualDirectory)
    {
        ULARGE_INTEGER size{};
        size.HighPart = information.nFileSizeHigh;
        size.LowPart = information.nFileSizeLow;
        entry.byteSize = size.QuadPart;
    }
    return cue::Result<cue::WorkspaceEntry>::success(std::move(entry));
}

cue::Result<cue::DirectorySnapshot> WindowsWorkspaceFilesystem::list_directory(
    const cue::WorkspaceDirectory &a_directory, cue::TraversalLimits a_limits) noexcept
{
    if (!owns_directory(a_directory))
    {
        return cue::Result<cue::DirectorySnapshot>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::OutsideRoot, "Workspace directory belongs to another root binding"));
    }
    if (!a_limits.is_valid())
    {
        return cue::Result<cue::DirectorySnapshot>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::CapacityExceeded, "Workspace listing limits must all be non-zero"));
    }
    if (m_nextGeneration == std::numeric_limits<std::uint64_t>::max())
    {
        return cue::Result<cue::DirectorySnapshot>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::CapacityExceeded, "Workspace snapshot generation was exhausted"));
    }

    cue::DirectorySnapshot snapshot;
    snapshot.generation = m_nextGeneration++;
    cue::Result<std::vector<UniqueHandle>> pinned = pin_directory_chain(a_directory);
    if (!pinned)
    {
        return cue::Result<cue::DirectorySnapshot>::failure(std::move(*pinned.try_error()));
    }

    const HANDLE directoryHandle = pinned.try_value()->empty() ? m_rootHandle.get() : pinned.try_value()->back().get();
    std::size_t metadataBytes = 0U;
    const std::size_t maximumEntries =
        std::min({a_limits.maxVisitedEntries, a_limits.maxResults, k_windowsHardLimits.maxVisitedEntries,
                  k_windowsHardLimits.maxResults});
    const std::size_t maximumMetadata = std::min(a_limits.maxMetadataBytes, k_windowsHardLimits.maxMetadataBytes);

    cue::Result<NativeDirectoryEnumeration> initial =
        enumerate_directory_handle(directoryHandle, maximumEntries, maximumMetadata, m_assertContext);
    if (!initial)
    {
        return cue::Result<cue::DirectorySnapshot>::failure(std::move(*initial.try_error()));
    }

    const auto append_bounded_diagnostic = [&](cue::WorkspaceDiagnosticCode a_code, std::string_view a_displayName,
                                               std::int64_t a_nativeCode) noexcept -> cue::Result<void>
    {
        if (a_displayName.size() > std::numeric_limits<std::size_t>::max() - sizeof(cue::WorkspaceDiagnostic))
        {
            return cue::Result<void>::failure(cue::make_io_error(m_assertContext, cue::IoError::CapacityExceeded,
                                                                 "Workspace listing metadata size overflowed"));
        }
        const std::size_t bytes = sizeof(cue::WorkspaceDiagnostic) + a_displayName.size();
        if (bytes > maximumMetadata - metadataBytes)
        {
            return cue::Result<void>::failure(cue::make_io_error(m_assertContext, cue::IoError::CapacityExceeded,
                                                                 "Workspace listing metadata limit was exceeded"));
        }
        append_diagnostic(snapshot, a_code, a_displayName, a_nativeCode, m_assertContext);
        metadataBytes += bytes;
        return cue::Result<void>::success();
    };

    const auto reject_unverified_entries = [&]() noexcept
    {
        for (cue::WorkspaceEntry &entry : snapshot.entries)
        {
            if (entry.is_operable())
            {
                reject_stale_entry(entry, cue::WorkspaceDiagnosticCode::EnumerationFailed, m_assertContext);
            }
        }
    };

    for (const NativeDirectoryEntry &nativeEntry : initial.try_value()->entries)
    {
        const std::size_t diagnosticCount = snapshot.diagnostics.size();
        cue::Result<cue::WorkspaceEntry> converted =
            make_entry(a_directory, directoryHandle, nativeEntry, snapshot.generation, snapshot);
        if (!converted)
        {
            return cue::Result<cue::DirectorySnapshot>::failure(std::move(*converted.try_error()));
        }
        for (std::size_t index = diagnosticCount; index < snapshot.diagnostics.size(); ++index)
        {
            const std::size_t diagnosticBytes = metadata_bytes(snapshot.diagnostics[index]);
            if (diagnosticBytes > maximumMetadata - metadataBytes)
            {
                return cue::Result<cue::DirectorySnapshot>::failure(cue::make_io_error(
                    m_assertContext, cue::IoError::CapacityExceeded, "Workspace listing metadata limit was exceeded"));
            }
            metadataBytes += diagnosticBytes;
        }
        const std::size_t entryBytes = metadata_bytes(*converted.try_value());
        if (entryBytes > maximumMetadata - metadataBytes)
        {
            return cue::Result<cue::DirectorySnapshot>::failure(cue::make_io_error(
                m_assertContext, cue::IoError::CapacityExceeded, "Workspace listing metadata limit was exceeded"));
        }
        metadataBytes += entryBytes;
        snapshot.entries.push_back(std::move(*converted.try_value()));
    }

    if (initial.try_value()->interruptedCode.has_value())
    {
        snapshot.state = cue::WorkspaceSnapshotState::RescanRequired;
        reject_unverified_entries();
        cue::Result<void> recorded =
            append_bounded_diagnostic(cue::WorkspaceDiagnosticCode::EnumerationFailed, {},
                                      static_cast<std::int64_t>(*initial.try_value()->interruptedCode));
        if (!recorded)
        {
            return cue::Result<cue::DirectorySnapshot>::failure(std::move(*recorded.try_error()));
        }
    }
    else
    {
        cue::Result<NativeDirectoryEnumeration> verification =
            enumerate_directory_handle(directoryHandle, maximumEntries, maximumMetadata, m_assertContext);
        if (!verification)
        {
            const cue::ErrorCode &rootCode = verification.try_error()->root_code();
            const bool isCapacityFailure =
                rootCode.domain() == "Cue.IO" &&
                rootCode.value() == static_cast<std::int64_t>(cue::IoError::CapacityExceeded);
            const bool isPortableFailure = verification.try_error()->try_native_error() == nullptr;
            if (isCapacityFailure || isPortableFailure)
            {
                return cue::Result<cue::DirectorySnapshot>::failure(std::move(*verification.try_error()));
            }
            std::int64_t nativeCode = 0;
            if (const cue::NativeError *native = verification.try_error()->try_native_error(); native != nullptr)
            {
                nativeCode = native->value();
            }
            snapshot.state = cue::WorkspaceSnapshotState::RescanRequired;
            reject_unverified_entries();
            cue::Result<void> recorded =
                append_bounded_diagnostic(cue::WorkspaceDiagnosticCode::EnumerationFailed, {}, nativeCode);
            if (!recorded)
            {
                return cue::Result<cue::DirectorySnapshot>::failure(std::move(*recorded.try_error()));
            }
        }
        else if (verification.try_value()->interruptedCode.has_value())
        {
            snapshot.state = cue::WorkspaceSnapshotState::RescanRequired;
            reject_unverified_entries();
            cue::Result<void> recorded =
                append_bounded_diagnostic(cue::WorkspaceDiagnosticCode::EnumerationFailed, {},
                                          static_cast<std::int64_t>(*verification.try_value()->interruptedCode));
            if (!recorded)
            {
                return cue::Result<cue::DirectorySnapshot>::failure(std::move(*recorded.try_error()));
            }
        }
        else
        {
            try
            {
                auto &currentEntries = verification.try_value()->entries;
                std::sort(currentEntries.begin(), currentEntries.end(),
                          [](const NativeDirectoryEntry &a_left, const NativeDirectoryEntry &a_right) noexcept
                          { return a_left.name < a_right.name; });

                std::vector<std::wstring_view> initialNames;
                initialNames.reserve(initial.try_value()->entries.size());
                for (const NativeDirectoryEntry &entry : initial.try_value()->entries)
                {
                    initialNames.push_back(entry.name);
                }
                std::sort(initialNames.begin(), initialNames.end());

                for (std::size_t index = 0U; index < initial.try_value()->entries.size(); ++index)
                {
                    const NativeDirectoryEntry &before = initial.try_value()->entries[index];
                    const auto current =
                        std::lower_bound(currentEntries.begin(), currentEntries.end(), before.name,
                                         [](const NativeDirectoryEntry &a_entry, const std::wstring &a_name) noexcept
                                         { return a_entry.name < a_name; });

                    std::optional<cue::WorkspaceDiagnosticCode> diagnostic;
                    if (current == currentEntries.end() || current->name != before.name)
                    {
                        diagnostic = cue::WorkspaceDiagnosticCode::EntryDisappeared;
                    }
                    else
                    {
                        const DWORD typeMask = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
                        if (!same_file_id(current->fileId, before.fileId))
                        {
                            diagnostic = (current->attributes & typeMask) != (before.attributes & typeMask)
                                             ? cue::WorkspaceDiagnosticCode::TypeChanged
                                             : cue::WorkspaceDiagnosticCode::EntryDisappeared;
                        }
                        else if ((current->attributes & typeMask) != (before.attributes & typeMask))
                        {
                            diagnostic = (current->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U
                                             ? cue::WorkspaceDiagnosticCode::ReparsePoint
                                             : cue::WorkspaceDiagnosticCode::TypeChanged;
                        }
                    }

                    if (diagnostic.has_value())
                    {
                        snapshot.state = cue::WorkspaceSnapshotState::RescanRequired;
                        reject_stale_entry(snapshot.entries[index], *diagnostic, m_assertContext);
                        cue::Result<void> recorded =
                            append_bounded_diagnostic(*diagnostic, snapshot.entries[index].displayName, 0);
                        if (!recorded)
                        {
                            return cue::Result<cue::DirectorySnapshot>::failure(std::move(*recorded.try_error()));
                        }
                    }
                }

                for (const NativeDirectoryEntry &current : currentEntries)
                {
                    if (!std::binary_search(initialNames.begin(), initialNames.end(), std::wstring_view(current.name)))
                    {
                        cue::Result<std::string> displayName = to_utf8(current.name, m_assertContext);
                        std::string fallback;
                        std::string_view diagnosticName;
                        if (displayName)
                        {
                            diagnosticName = *displayName.try_value();
                        }
                        else
                        {
                            fallback = make_native_name_fallback(current.name, m_assertContext);
                            diagnosticName = fallback;
                        }
                        snapshot.state = cue::WorkspaceSnapshotState::RescanRequired;
                        cue::Result<void> recorded = append_bounded_diagnostic(
                            cue::WorkspaceDiagnosticCode::EnumerationFailed, diagnosticName, 0);
                        if (!recorded)
                        {
                            return cue::Result<cue::DirectorySnapshot>::failure(std::move(*recorded.try_error()));
                        }
                    }
                }
            }
            catch (...)
            {
                terminate_allocation(m_assertContext);
            }
        }
    }

    cue::sort_workspace_entries(snapshot.entries);
    return cue::Result<cue::DirectorySnapshot>::success(std::move(snapshot));
}

cue::WorkspaceMutationResult WindowsWorkspaceFilesystem::create_directory_new(
    const cue::BoundWorkspacePath &a_destination, std::string_view a_operationId) noexcept
{
    if (!is_valid_operation_id(a_operationId))
    {
        return not_committed(
            cue::make_io_error(m_assertContext, cue::IoError::InvalidPath, "Workspace operation id is invalid"));
    }
    cue::Result<std::vector<UniqueHandle>> pinned = prepare_mutation_parent(a_destination);
    if (!pinned)
    {
        return not_committed(std::move(*pinned.try_error()));
    }
    cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>> parentGuards =
        begin_mutation_parent_guards(*pinned.try_value());
    if (!parentGuards)
    {
        return not_committed(std::move(*parentGuards.try_error()));
    }
    cue::Result<void> initialParent = verify_mutation_parent_chain(a_destination, *pinned.try_value());
    if (!initialParent)
    {
        return not_committed(std::move(*initialParent.try_error()));
    }

    cue::Result<std::wstring> destination =
        make_native_workspace_path(m_rootPath, a_destination.text(), m_assertContext);
    if (!destination)
    {
        return not_committed(std::move(*destination.try_error()));
    }

    std::string stagingText;
    try
    {
        const std::size_t separator = a_destination.text().rfind('/');
        if (separator != std::string_view::npos)
        {
            stagingText.assign(a_destination.text().substr(0U, separator + 1U));
        }
        stagingText.push_back('.');
        stagingText.append(a_operationId);
        stagingText.append(".cuedir-staging");
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    cue::Result<std::wstring> staging = make_native_workspace_path(m_rootPath, stagingText, m_assertContext);
    if (!staging)
    {
        return not_committed(std::move(*staging.try_error()));
    }

    const std::size_t stagingNameOffset = staging.try_value()->rfind(L'\\');
    if (stagingNameOffset == std::wstring::npos || stagingNameOffset + 1U == staging.try_value()->size())
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::InvalidPath,
                                                "Workspace staging directory name is invalid"));
    }

    UniqueHandle stagingHandle;
    const DWORD createCode = create_directory_exclusive(
        pinned.try_value()->back().get(), std::wstring_view(*staging.try_value()).substr(stagingNameOffset + 1U),
        stagingHandle);
    if (createCode != ERROR_SUCCESS)
    {
        return not_committed(
            make_windows_error(m_assertContext, createCode, "Workspace staging directory creation failed"));
    }
    cue::Result<RootIdentity> identity = read_entry_identity(stagingHandle.get(), m_assertContext);
    if (!identity)
    {
        return reconciliation_required(std::move(*identity.try_error()));
    }
    const RootIdentity expected = *identity.try_value();

    const std::size_t destinationNameOffset = destination.try_value()->rfind(L'\\');
    if (destinationNameOffset == std::wstring::npos || destinationNameOffset + 1U == destination.try_value()->size())
    {
        cue::WorkspaceMutationResult result = not_committed(cue::make_io_error(
            m_assertContext, cue::IoError::InvalidPath, "Workspace destination directory name is invalid"));
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, true, result, m_assertContext);
        return result;
    }
    cue::Result<void> destinationAvailable = verify_portable_destination_absent(
        pinned.try_value()->back().get(),
        std::wstring_view(*destination.try_value()).substr(destinationNameOffset + 1U), m_assertContext);
    if (!destinationAvailable)
    {
        cue::WorkspaceMutationResult result = not_committed(std::move(*destinationAvailable.try_error()));
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, true, result, m_assertContext);
        return result;
    }

    cue::Result<void> parentGuardsPending = verify_mutation_parent_guards_pending(*parentGuards.try_value());
    if (!parentGuardsPending)
    {
        cue::WorkspaceMutationResult result = not_committed(std::move(*parentGuardsPending.try_error()));
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, true, result, m_assertContext);
        cue::Result<void> stableGuards = finish_mutation_parent_guards(*parentGuards.try_value());
        if (!stableGuards)
        {
            append_secondary(result, std::move(*stableGuards.try_error()), m_assertContext);
        }
        return result;
    }

    const DWORD publishCode = rename_open_entry(
        stagingHandle.get(), pinned.try_value()->back().get(),
        std::wstring_view(*destination.try_value()).substr(destinationNameOffset + 1U), m_assertContext);
    if (publishCode == ERROR_SUCCESS)
    {
        const NativeEntryObservation observed = observe_native_entry(*destination.try_value(), expected, true);
        if (observed.state == NativeEntryObservationState::Matches)
        {
            cue::Result<void> uniqueDestination = verify_portable_destination_unique(
                pinned.try_value()->back().get(),
                std::wstring_view(*destination.try_value()).substr(destinationNameOffset + 1U), expected, true,
                m_assertContext);
            if (!uniqueDestination)
            {
                return reconciliation_required(std::move(*uniqueDestination.try_error()));
            }
            cue::Result<void> stableParent = verify_mutation_parent_chain(a_destination, *pinned.try_value());
            if (!stableParent)
            {
                return reconciliation_required(std::move(*stableParent.try_error()));
            }
            cue::Result<void> stableGuards = finish_mutation_parent_guards(*parentGuards.try_value());
            if (!stableGuards)
            {
                return reconciliation_required(std::move(*stableGuards.try_error()));
            }
            cue::WorkspaceMutationResult result;
            result.outcome = cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
            result.primaryError =
                cue::make_io_error(m_assertContext, cue::IoError::DurabilityUnknown,
                                   "Workspace directory is visible but handle-based publish durability is unknown");
            return result;
        }
        const DWORD code =
            observed.state == NativeEntryObservationState::QueryFailed ? observed.nativeCode : ERROR_INVALID_DATA;
        return reconciliation_required(
            make_windows_error(m_assertContext, code, "Workspace published directory verification failed"));
    }

    const NativeEntryObservation destinationState = observe_native_entry(*destination.try_value(), expected, true);
    if (destinationState.state == NativeEntryObservationState::Matches)
    {
        cue::Result<void> uniqueDestination = verify_portable_destination_unique(
            pinned.try_value()->back().get(),
            std::wstring_view(*destination.try_value()).substr(destinationNameOffset + 1U), expected, true,
            m_assertContext);
        if (!uniqueDestination)
        {
            return reconciliation_required(std::move(*uniqueDestination.try_error()));
        }
        cue::Result<void> stableParent = verify_mutation_parent_chain(a_destination, *pinned.try_value());
        if (!stableParent)
        {
            return reconciliation_required(std::move(*stableParent.try_error()));
        }
        cue::Result<void> stableGuards = finish_mutation_parent_guards(*parentGuards.try_value());
        if (!stableGuards)
        {
            return reconciliation_required(std::move(*stableGuards.try_error()));
        }
        cue::WorkspaceMutationResult result;
        result.outcome = cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
        result.primaryError = cue::make_io_error(m_assertContext, cue::IoError::DurabilityUnknown,
                                                 "Workspace directory is visible but publish durability is unknown",
                                                 static_cast<std::int64_t>(publishCode));
        return result;
    }

    cue::WorkspaceMutationResult result =
        destinationState.state == NativeEntryObservationState::QueryFailed
            ? reconciliation_required(make_windows_error(m_assertContext, destinationState.nativeCode,
                                                         "Workspace destination verification failed"))
            : not_committed(make_windows_error(
                  m_assertContext,
                  destinationState.state == NativeEntryObservationState::Different ? ERROR_ALREADY_EXISTS : publishCode,
                  "Workspace directory publish failed"));
    cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, true, result, m_assertContext);
    return result;
}

cue::WorkspaceMutationResult WindowsWorkspaceFilesystem::create_file_new_atomic(
    const cue::BoundWorkspacePath &a_destination, std::span<const std::byte> a_bytes,
    std::string_view a_operationId) noexcept
{
    if (!is_valid_operation_id(a_operationId))
    {
        return not_committed(
            cue::make_io_error(m_assertContext, cue::IoError::InvalidPath, "Workspace operation id is invalid"));
    }
    if (a_bytes.size() > static_cast<std::size_t>(std::numeric_limits<LONGLONG>::max()))
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::CapacityExceeded,
                                                "Workspace initial file content exceeds the host limit"));
    }
    cue::Result<std::vector<UniqueHandle>> pinned = prepare_mutation_parent(a_destination);
    if (!pinned)
    {
        return not_committed(std::move(*pinned.try_error()));
    }
    cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>> parentGuards =
        begin_mutation_parent_guards(*pinned.try_value());
    if (!parentGuards)
    {
        return not_committed(std::move(*parentGuards.try_error()));
    }
    cue::Result<void> initialParent = verify_mutation_parent_chain(a_destination, *pinned.try_value());
    if (!initialParent)
    {
        return not_committed(std::move(*initialParent.try_error()));
    }

    cue::Result<std::wstring> destination =
        make_native_workspace_path(m_rootPath, a_destination.text(), m_assertContext);
    if (!destination)
    {
        return not_committed(std::move(*destination.try_error()));
    }

    std::string stagingText;
    try
    {
        const std::size_t separator = a_destination.text().rfind('/');
        if (separator != std::string_view::npos)
        {
            stagingText.assign(a_destination.text().substr(0U, separator + 1U));
        }
        stagingText.push_back('.');
        stagingText.append(a_operationId);
        stagingText.append(".cuefile-staging");
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    cue::Result<std::wstring> staging = make_native_workspace_path(m_rootPath, stagingText, m_assertContext);
    if (!staging)
    {
        return not_committed(std::move(*staging.try_error()));
    }

    UniqueHandle stagingHandle(CreateFileW(staging.try_value()->c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                           FILE_SHARE_READ, nullptr, CREATE_NEW,
                                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!stagingHandle.is_valid())
    {
        return not_committed(
            make_windows_error(m_assertContext, GetLastError(), "Workspace temporary file creation failed"));
    }
    cue::Result<RootIdentity> identity = read_entry_identity(stagingHandle.get(), m_assertContext);
    if (!identity)
    {
        return reconciliation_required(std::move(*identity.try_error()));
    }
    const RootIdentity expected = *identity.try_value();

    std::size_t writtenTotal = 0U;
    while (writtenTotal < a_bytes.size())
    {
        const std::size_t remaining = a_bytes.size() - writtenTotal;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
        DWORD written = 0U;
        const BOOL writeSucceeded =
            WriteFile(stagingHandle.get(), a_bytes.data() + writtenTotal, chunk, &written, nullptr);
        if (writeSucceeded == FALSE || written != chunk)
        {
            const DWORD code = writeSucceeded == FALSE ? GetLastError() : ERROR_WRITE_FAULT;
            cue::WorkspaceMutationResult result =
                not_committed(make_windows_error(m_assertContext, code, "Workspace temporary file write failed"));
            cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, false, result, m_assertContext);
            return result;
        }
        writtenTotal += written;
    }
    if (FlushFileBuffers(stagingHandle.get()) == FALSE)
    {
        cue::WorkspaceMutationResult result =
            not_committed(make_windows_error(m_assertContext, GetLastError(), "Workspace temporary file flush failed"));
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, false, result, m_assertContext);
        return result;
    }
    LARGE_INTEGER fileSize{};
    const BOOL sizeSucceeded = GetFileSizeEx(stagingHandle.get(), &fileSize);
    if (sizeSucceeded == FALSE || fileSize.QuadPart != static_cast<LONGLONG>(a_bytes.size()))
    {
        const DWORD code = sizeSucceeded == FALSE ? GetLastError() : ERROR_INVALID_DATA;
        cue::WorkspaceMutationResult result =
            not_committed(make_windows_error(m_assertContext, code, "Workspace temporary file verification failed"));
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, false, result, m_assertContext);
        return result;
    }

    const std::size_t destinationNameOffset = destination.try_value()->rfind(L'\\');
    if (destinationNameOffset == std::wstring::npos || destinationNameOffset + 1U == destination.try_value()->size())
    {
        cue::WorkspaceMutationResult result = not_committed(cue::make_io_error(
            m_assertContext, cue::IoError::InvalidPath, "Workspace destination file name is invalid"));
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, false, result, m_assertContext);
        return result;
    }
    cue::Result<void> destinationAvailable = verify_portable_destination_absent(
        pinned.try_value()->back().get(),
        std::wstring_view(*destination.try_value()).substr(destinationNameOffset + 1U), m_assertContext);
    if (!destinationAvailable)
    {
        cue::WorkspaceMutationResult result = not_committed(std::move(*destinationAvailable.try_error()));
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, false, result, m_assertContext);
        return result;
    }

    cue::Result<void> parentGuardsPending = verify_mutation_parent_guards_pending(*parentGuards.try_value());
    if (!parentGuardsPending)
    {
        cue::WorkspaceMutationResult result = not_committed(std::move(*parentGuardsPending.try_error()));
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, false, result, m_assertContext);
        cue::Result<void> stableGuards = finish_mutation_parent_guards(*parentGuards.try_value());
        if (!stableGuards)
        {
            append_secondary(result, std::move(*stableGuards.try_error()), m_assertContext);
        }
        return result;
    }

    const DWORD publishCode = rename_open_entry(
        stagingHandle.get(), pinned.try_value()->back().get(),
        std::wstring_view(*destination.try_value()).substr(destinationNameOffset + 1U), m_assertContext);
    if (publishCode == ERROR_SUCCESS)
    {
        const NativeEntryObservation observed = observe_native_entry(*destination.try_value(), expected, false);
        if (observed.state == NativeEntryObservationState::Matches)
        {
            cue::Result<void> uniqueDestination = verify_portable_destination_unique(
                pinned.try_value()->back().get(),
                std::wstring_view(*destination.try_value()).substr(destinationNameOffset + 1U), expected, false,
                m_assertContext);
            if (!uniqueDestination)
            {
                return reconciliation_required(std::move(*uniqueDestination.try_error()));
            }
            cue::Result<void> stableParent = verify_mutation_parent_chain(a_destination, *pinned.try_value());
            if (!stableParent)
            {
                return reconciliation_required(std::move(*stableParent.try_error()));
            }
            cue::Result<void> stableGuards = finish_mutation_parent_guards(*parentGuards.try_value());
            if (!stableGuards)
            {
                return reconciliation_required(std::move(*stableGuards.try_error()));
            }
            cue::WorkspaceMutationResult result;
            result.outcome = cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
            result.primaryError =
                cue::make_io_error(m_assertContext, cue::IoError::DurabilityUnknown,
                                   "Workspace file is visible but handle-based publish durability is unknown");
            return result;
        }
        const DWORD code =
            observed.state == NativeEntryObservationState::QueryFailed ? observed.nativeCode : ERROR_INVALID_DATA;
        return reconciliation_required(
            make_windows_error(m_assertContext, code, "Workspace published file verification failed"));
    }

    const NativeEntryObservation destinationState = observe_native_entry(*destination.try_value(), expected, false);
    if (destinationState.state == NativeEntryObservationState::Matches)
    {
        cue::Result<void> uniqueDestination = verify_portable_destination_unique(
            pinned.try_value()->back().get(),
            std::wstring_view(*destination.try_value()).substr(destinationNameOffset + 1U), expected, false,
            m_assertContext);
        if (!uniqueDestination)
        {
            return reconciliation_required(std::move(*uniqueDestination.try_error()));
        }
        cue::Result<void> stableParent = verify_mutation_parent_chain(a_destination, *pinned.try_value());
        if (!stableParent)
        {
            return reconciliation_required(std::move(*stableParent.try_error()));
        }
        cue::Result<void> stableGuards = finish_mutation_parent_guards(*parentGuards.try_value());
        if (!stableGuards)
        {
            return reconciliation_required(std::move(*stableGuards.try_error()));
        }
        cue::WorkspaceMutationResult result;
        result.outcome = cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
        result.primaryError = cue::make_io_error(m_assertContext, cue::IoError::DurabilityUnknown,
                                                 "Workspace file is visible but publish durability is unknown",
                                                 static_cast<std::int64_t>(publishCode));
        return result;
    }

    cue::WorkspaceMutationResult result =
        destinationState.state == NativeEntryObservationState::QueryFailed
            ? reconciliation_required(make_windows_error(m_assertContext, destinationState.nativeCode,
                                                         "Workspace destination verification failed"))
            : not_committed(make_windows_error(
                  m_assertContext,
                  destinationState.state == NativeEntryObservationState::Different ? ERROR_ALREADY_EXISTS : publishCode,
                  "Workspace file publish failed"));
    cleanup_owned_temporary(stagingHandle, *staging.try_value(), expected, false, result, m_assertContext);
    return result;
}

cue::WorkspaceMutationResult WindowsWorkspaceFilesystem::replace_file_atomic(
    const cue::BoundWorkspacePath &a_destination, std::span<const std::byte> a_bytes,
    std::string_view a_operationId) noexcept
{
    if (!owns_path(a_destination) || !is_valid_operation_id(a_operationId))
    {
        return not_committed(
            cue::make_io_error(m_assertContext, cue::IoError::InvalidPath, "Workspace replace request is invalid"));
    }
    if (a_bytes.size() > static_cast<std::size_t>(std::numeric_limits<LONGLONG>::max()))
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::CapacityExceeded,
                                                "Workspace replacement content exceeds the host limit"));
    }
    cue::Result<WorkspaceReplacementMutexOwnership> replacementOwnership =
        acquire_workspace_replacement_mutex(m_assertContext);
    if (!replacementOwnership)
    {
        return not_committed(std::move(*replacementOwnership.try_error()));
    }
    cue::Result<std::vector<UniqueHandle>> pinned = prepare_mutation_parent(a_destination);
    if (!pinned)
    {
        return not_committed(std::move(*pinned.try_error()));
    }
    cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>> parentGuards =
        begin_mutation_parent_guards(*pinned.try_value());
    if (!parentGuards)
    {
        return not_committed(std::move(*parentGuards.try_error()));
    }

    const std::size_t separator = a_destination.text().rfind('/');
    const std::string_view leaf =
        separator == std::string_view::npos ? a_destination.text() : a_destination.text().substr(separator + 1U);
    cue::Result<std::wstring> nativeLeaf = to_utf16(leaf, m_assertContext);
    cue::Result<std::wstring> destination =
        make_native_workspace_path(m_rootPath, a_destination.text(), m_assertContext);
    if (!nativeLeaf || !destination)
    {
        return not_committed(!nativeLeaf ? std::move(*nativeLeaf.try_error()) : std::move(*destination.try_error()));
    }
    cue::Result<OpenedNativeEntry> existing =
        open_exact_entry(pinned.try_value()->back().get(), *nativeLeaf.try_value(), DELETE | FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_DELETE, m_assertContext);
    if (!existing)
    {
        return not_committed(std::move(*existing.try_error()));
    }
    BY_HANDLE_FILE_INFORMATION existingInformation{};
    if (existing.try_value()->isDirectory)
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::TypeMismatch,
                                                "Workspace replacement destination is a directory"));
    }
    if (GetFileInformationByHandle(existing.try_value()->handle.get(), &existingInformation) == FALSE)
    {
        const DWORD inspectionCode = GetLastError();
        return not_committed(
            make_windows_error(m_assertContext, inspectionCode, "Workspace replacement destination inspection failed"));
    }
    if (existingInformation.nNumberOfLinks != 1U)
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::UnsupportedEntry,
                                                "Workspace replacement destination has multiple hard links"));
    }

    std::string stagingText;
    try
    {
        if (separator != std::string_view::npos)
        {
            stagingText.assign(a_destination.text().substr(0U, separator + 1U));
        }
        stagingText.push_back('.');
        stagingText.append(a_operationId);
        stagingText.append(".cuefile-replace-staging");
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    cue::Result<std::wstring> staging = make_native_workspace_path(m_rootPath, stagingText, m_assertContext);
    if (!staging)
    {
        return not_committed(std::move(*staging.try_error()));
    }
    UniqueHandle stagingHandle(CreateFileW(staging.try_value()->c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                           FILE_SHARE_READ, nullptr, CREATE_NEW,
                                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!stagingHandle.is_valid())
    {
        return not_committed(
            make_windows_error(m_assertContext, GetLastError(), "Workspace replacement staging creation failed"));
    }
    cue::Result<RootIdentity> stagingIdentity = read_entry_identity(stagingHandle.get(), m_assertContext);
    if (!stagingIdentity)
    {
        return reconciliation_required(std::move(*stagingIdentity.try_error()));
    }
    const RootIdentity expectedStaging = *stagingIdentity.try_value();
    cue::Result<void> written = write_and_flush_file(stagingHandle.get(), a_bytes, m_assertContext);
    if (!written)
    {
        cue::WorkspaceMutationResult result = not_committed(std::move(*written.try_error()));
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expectedStaging, false, result, m_assertContext);
        return result;
    }
    cue::Result<void> destinationStable =
        verify_portable_destination_unique(pinned.try_value()->back().get(), *nativeLeaf.try_value(),
                                           existing.try_value()->identity, false, m_assertContext);
    cue::Result<void> parentsPending = verify_mutation_parent_guards_pending(*parentGuards.try_value());
    if (!destinationStable || !parentsPending)
    {
        cue::WorkspaceMutationResult result = not_committed(
            !destinationStable ? std::move(*destinationStable.try_error()) : std::move(*parentsPending.try_error()));
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expectedStaging, false, result, m_assertContext);
        cue::Result<void> guardsFinished = finish_mutation_parent_guards(*parentGuards.try_value());
        if (!guardsFinished)
        {
            append_secondary(result, std::move(*guardsFinished.try_error()), m_assertContext);
        }
        return result;
    }

    existing.try_value()->handle.reset();
    const DWORD replaceCode = replace_open_entry(stagingHandle.get(), pinned.try_value()->back().get(),
                                                 *nativeLeaf.try_value(), m_assertContext);
    const NativeEntryObservation replacement = observe_native_entry(*destination.try_value(), expectedStaging, false);
    if (replacement.state == NativeEntryObservationState::Matches)
    {
        cue::Result<void> parentStable = verify_mutation_parent_chain(a_destination, *pinned.try_value());
        cue::Result<void> guardsFinished = finish_mutation_parent_guards(*parentGuards.try_value());
        if (!parentStable || !guardsFinished)
        {
            return reconciliation_required(!parentStable ? std::move(*parentStable.try_error())
                                                         : std::move(*guardsFinished.try_error()));
        }
        cue::WorkspaceMutationResult result;
        result.outcome = cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
        result.primaryError =
            cue::make_io_error(m_assertContext, cue::IoError::DurabilityUnknown,
                               "Workspace replacement is visible but handle-based publish durability is unknown",
                               static_cast<std::int64_t>(replaceCode));
        return result;
    }

    const NativeEntryObservation original =
        observe_native_entry(*destination.try_value(), existing.try_value()->identity, false);
    cue::WorkspaceMutationResult result =
        replaceCode != ERROR_SUCCESS && original.state == NativeEntryObservationState::Matches &&
                replacement.state == NativeEntryObservationState::Different
            ? not_committed(make_windows_error(m_assertContext, replaceCode, "Workspace replacement publish failed"))
            : reconciliation_required(make_windows_error(m_assertContext,
                                                         replacement.state == NativeEntryObservationState::QueryFailed
                                                             ? replacement.nativeCode
                                                             : ERROR_INVALID_DATA,
                                                         "Workspace replacement state could not be confirmed"));
    if (result.outcome == cue::WorkspaceMutationOutcome::NotCommitted)
    {
        cleanup_owned_temporary(stagingHandle, *staging.try_value(), expectedStaging, false, result, m_assertContext);
    }
    cue::Result<void> guardsFinished = finish_mutation_parent_guards(*parentGuards.try_value());
    if (!guardsFinished)
    {
        append_secondary(result, std::move(*guardsFinished.try_error()), m_assertContext);
        result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
    }
    return result;
}

cue::WorkspaceMutationResult WindowsWorkspaceFilesystem::rename_entry(const cue::BoundWorkspacePath &a_source,
                                                                      const cue::BoundWorkspacePath &a_destination,
                                                                      cue::TraversalLimits a_limits) noexcept
{
    return rename_entry_internal(a_source, a_destination, nullptr, a_limits, cue::ContentVerificationLimits{1U, 1U});
}

cue::Result<cue::GuardedWorkspaceEntry> WindowsWorkspaceFilesystem::guard_entry(
    const cue::BoundWorkspacePath &a_entry, cue::TraversalLimits a_traversalLimits,
    cue::ContentVerificationLimits a_contentLimits) noexcept
{
    if (!owns_path(a_entry))
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::OutsideRoot, "Workspace mutation guard path belongs to another root"));
    }
    if (!a_traversalLimits.is_valid() || !a_contentLimits.is_valid())
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::CapacityExceeded, "Workspace mutation guard limits must be non-zero"));
    }

    cue::Result<cue::WorkspaceDirectory> parent = parent_directory(a_entry, m_assertContext);
    if (!parent)
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(std::move(*parent.try_error()));
    }
    cue::Result<std::vector<UniqueHandle>> pinned = pin_directory_chain(*parent.try_value());
    if (!pinned)
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(std::move(*pinned.try_error()));
    }
    const HANDLE parentHandle = pinned.try_value()->empty() ? m_rootHandle.get() : pinned.try_value()->back().get();
    const std::size_t separator = a_entry.text().rfind('/');
    const std::string_view leaf =
        separator == std::string_view::npos ? a_entry.text() : a_entry.text().substr(separator + 1U);
    cue::Result<std::wstring> nativeLeaf = to_utf16(leaf, m_assertContext);
    cue::Result<std::wstring> nativePath = make_native_workspace_path(m_rootPath, a_entry.text(), m_assertContext);
    if (!nativeLeaf || !nativePath)
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(!nativeLeaf ? std::move(*nativeLeaf.try_error())
                                                                            : std::move(*nativePath.try_error()));
    }
    cue::Result<OpenedNativeEntry> opened = open_exact_entry(
        parentHandle, *nativeLeaf.try_value(), GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        m_assertContext, true, FILE_FLAG_WRITE_THROUGH, nativePath.try_value()->c_str());
    if (!opened)
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(std::move(*opened.try_error()));
    }
    const RootIdentity rootIdentity = opened.try_value()->identity;
    const bool isDirectory = opened.try_value()->isDirectory;
    BY_HANDLE_FILE_INFORMATION rootInformation{};
    if (GetFileInformationByHandle(opened.try_value()->handle.get(), &rootInformation) == FALSE)
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(
            make_windows_error(m_assertContext, GetLastError(), "Workspace mutation guard root inspection failed"));
    }
    if (((rootInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) != isDirectory)
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::PreconditionFailed, "Workspace mutation guard root type changed"));
    }
    if (!isDirectory && rootInformation.nNumberOfLinks != 1U)
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::UnsupportedEntry, "Workspace mutation guard file has multiple hard links"));
    }

    cue::Result<cue::WorkspaceEntryFingerprint> initial =
        isDirectory ? fingerprint_entry_once(a_entry, a_traversalLimits, a_contentLimits)
                    : fingerprint_guarded_file(opened.try_value()->handle.get(), a_contentLimits, m_assertContext);
    if (!initial)
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(std::move(*initial.try_error()));
    }

    std::vector<UniqueHandle> childHandles;
    std::unique_ptr<DirectoryChangeGuard> rootChangeGuard;
    try
    {
        childHandles.reserve(initial.try_value()->manifest.size());
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    if (isDirectory)
    {
        for (const cue::WorkspaceManifestEntry &manifestEntry : initial.try_value()->manifest)
        {
            cue::Result<cue::RelativePath> relative = cue::RelativePath::parse(manifestEntry.path, m_assertContext);
            cue::Result<cue::BoundWorkspacePath> child =
                relative ? bind_child_path(a_entry, std::move(*relative.try_value()), m_assertContext)
                         : cue::Result<cue::BoundWorkspacePath>::failure(std::move(*relative.try_error()));
            if (!child)
            {
                return cue::Result<cue::GuardedWorkspaceEntry>::failure(std::move(*child.try_error()));
            }
            cue::Result<cue::WorkspaceDirectory> childParent = parent_directory(*child.try_value(), m_assertContext);
            if (!childParent)
            {
                return cue::Result<cue::GuardedWorkspaceEntry>::failure(std::move(*childParent.try_error()));
            }
            cue::Result<std::vector<UniqueHandle>> childPinned = pin_directory_chain(*childParent.try_value());
            if (!childPinned)
            {
                return cue::Result<cue::GuardedWorkspaceEntry>::failure(std::move(*childPinned.try_error()));
            }
            const HANDLE childParentHandle =
                childPinned.try_value()->empty() ? m_rootHandle.get() : childPinned.try_value()->back().get();
            const std::size_t childSeparator = child.try_value()->text().rfind('/');
            const std::string_view childLeaf = child.try_value()->text().substr(childSeparator + 1U);
            cue::Result<std::wstring> nativeChildLeaf = to_utf16(childLeaf, m_assertContext);
            if (!nativeChildLeaf)
            {
                return cue::Result<cue::GuardedWorkspaceEntry>::failure(std::move(*nativeChildLeaf.try_error()));
            }
            const bool childIsDirectory = manifestEntry.type == cue::WorkspaceEntryType::Directory;
            const DWORD childAccess = childIsDirectory ? GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY
                                                       : GENERIC_READ | FILE_READ_ATTRIBUTES;
            cue::Result<OpenedNativeEntry> openedChild =
                open_exact_entry(childParentHandle, *nativeChildLeaf.try_value(), childAccess,
                                 FILE_SHARE_READ | FILE_SHARE_DELETE, m_assertContext, childIsDirectory);
            if (!openedChild || openedChild.try_value()->isDirectory != childIsDirectory)
            {
                return cue::Result<cue::GuardedWorkspaceEntry>::failure(
                    !openedChild ? std::move(*openedChild.try_error())
                                 : cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                      "Workspace mutation guard child type changed"));
            }
            if (!childIsDirectory)
            {
                BY_HANDLE_FILE_INFORMATION childInformation{};
                if (GetFileInformationByHandle(openedChild.try_value()->handle.get(), &childInformation) == FALSE)
                {
                    const DWORD inspectionCode = GetLastError();
                    return cue::Result<cue::GuardedWorkspaceEntry>::failure(make_windows_error(
                        m_assertContext, inspectionCode, "Workspace mutation guard child inspection failed"));
                }
                if (childInformation.nNumberOfLinks != 1U)
                {
                    return cue::Result<cue::GuardedWorkspaceEntry>::failure(
                        cue::make_io_error(m_assertContext, cue::IoError::UnsupportedEntry,
                                           "Workspace mutation guard child has multiple hard links"));
                }
            }
            childHandles.push_back(std::move(openedChild.try_value()->handle));
        }

        cue::Result<std::unique_ptr<DirectoryChangeGuard>> changeGuard = begin_directory_change_guard(
            opened.try_value()->handle.get(), m_assertContext, true,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION);
        if (!changeGuard)
        {
            return cue::Result<cue::GuardedWorkspaceEntry>::failure(std::move(*changeGuard.try_error()));
        }
        rootChangeGuard = std::move(*changeGuard.try_value());
    }

    cue::Result<cue::WorkspaceEntryFingerprint> verified =
        isDirectory ? fingerprint_entry_once(a_entry, a_traversalLimits, a_contentLimits)
                    : fingerprint_guarded_file(opened.try_value()->handle.get(), a_contentLimits, m_assertContext);
    cue::Result<void> rootChangePending = isDirectory
                                              ? verify_directory_change_guard_pending(*rootChangeGuard, m_assertContext)
                                              : cue::Result<void>::success();
    if (!verified || !rootChangePending || *verified.try_value() != *initial.try_value())
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(
            !verified ? std::move(*verified.try_error())
            : !rootChangePending
                ? std::move(*rootChangePending.try_error())
                : cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                     "Workspace mutation guard fingerprint changed during acquisition"));
    }

    std::unique_ptr<cue::WorkspaceEntryMutationGuard> guard;
    try
    {
        guard = std::make_unique<WindowsWorkspaceEntryMutationGuard>(
            this, a_entry, *verified.try_value(), a_traversalLimits, a_contentLimits,
            std::move(opened.try_value()->handle), rootIdentity, isDirectory, std::move(childHandles),
            std::move(rootChangeGuard));
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    cue::GuardedWorkspaceEntry result{std::move(*verified.try_value()), std::move(guard)};
    return cue::Result<cue::GuardedWorkspaceEntry>::success(std::move(result));
}

cue::Result<std::unique_ptr<cue::WorkspaceEntryMutationGuard>> WindowsWorkspaceFilesystem::guard_entry_if_matches(
    const cue::BoundWorkspacePath &a_entry, const cue::WorkspaceEntryFingerprint &a_expected,
    cue::TraversalLimits a_traversalLimits, cue::ContentVerificationLimits a_contentLimits) noexcept
{
    cue::Result<cue::GuardedWorkspaceEntry> guarded = guard_entry(a_entry, a_traversalLimits, a_contentLimits);
    if (!guarded)
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceEntryMutationGuard>>::failure(std::move(*guarded.try_error()));
    }
    if (guarded.try_value()->fingerprint != a_expected)
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceEntryMutationGuard>>::failure(cue::make_io_error(
            m_assertContext, cue::IoError::PreconditionFailed, "Workspace mutation guard fingerprint did not match"));
    }
    return cue::Result<std::unique_ptr<cue::WorkspaceEntryMutationGuard>>::success(
        std::move(guarded.try_value()->guard));
}

cue::WorkspaceMutationResult WindowsWorkspaceFilesystem::rename_guarded_entry(
    cue::WorkspaceEntryMutationGuard &a_guard, const cue::BoundWorkspacePath &a_source,
    const cue::BoundWorkspacePath &a_destination) noexcept
{
    auto *guard = dynamic_cast<WindowsWorkspaceEntryMutationGuard *>(&a_guard);
    if (guard == nullptr || guard->m_owner != this || !owns_path(a_source) || !owns_path(a_destination) ||
        guard->m_path.text() != a_source.text())
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                "Workspace guarded rename received an unrelated guard or path"));
    }
    cue::Result<std::vector<UniqueHandle>> destinationPinned = prepare_mutation_parent(a_destination);
    if (!destinationPinned)
    {
        return not_committed(std::move(*destinationPinned.try_error()));
    }
    const std::size_t destinationSeparator = a_destination.text().rfind('/');
    const std::string_view destinationLeaf = destinationSeparator == std::string_view::npos
                                                 ? a_destination.text()
                                                 : a_destination.text().substr(destinationSeparator + 1U);
    cue::Result<std::wstring> destinationName = to_utf16(destinationLeaf, m_assertContext);
    cue::Result<std::wstring> sourcePath = make_native_workspace_path(m_rootPath, a_source.text(), m_assertContext);
    cue::Result<std::wstring> destinationPath =
        make_native_workspace_path(m_rootPath, a_destination.text(), m_assertContext);
    if (!destinationName || !sourcePath || !destinationPath)
    {
        return not_committed(!destinationName ? std::move(*destinationName.try_error())
                             : !sourcePath    ? std::move(*sourcePath.try_error())
                                              : std::move(*destinationPath.try_error()));
    }
    cue::Result<void> destinationAvailable = verify_portable_destination_absent(
        destinationPinned.try_value()->back().get(), *destinationName.try_value(), m_assertContext);
    if (!destinationAvailable)
    {
        return not_committed(std::move(*destinationAvailable.try_error()));
    }
    cue::Result<void> rootChangePending =
        guard->m_isDirectory ? verify_directory_change_guard_pending(*guard->m_rootChangeGuard, m_assertContext)
                             : cue::Result<void>::success();
    if (!rootChangePending)
    {
        return not_committed(std::move(*rootChangePending.try_error()));
    }

    const DWORD renameCode = rename_open_entry(guard->m_rootHandle.get(), destinationPinned.try_value()->back().get(),
                                               *destinationName.try_value(), m_assertContext);
    const NativeEntryObservation destinationObserved =
        observe_native_entry(*destinationPath.try_value(), guard->m_rootIdentity, guard->m_isDirectory);
    const NativeEntryObservation sourceObserved =
        observe_native_entry(*sourcePath.try_value(), guard->m_rootIdentity, guard->m_isDirectory);
    if (destinationObserved.state != NativeEntryObservationState::Matches)
    {
        return renameCode != ERROR_SUCCESS && sourceObserved.state == NativeEntryObservationState::Matches &&
                       destinationObserved.state == NativeEntryObservationState::Missing
                   ? not_committed(make_windows_error(m_assertContext, renameCode, "Workspace guarded rename failed"))
                   : reconciliation_required(
                         make_windows_error(m_assertContext,
                                            destinationObserved.state == NativeEntryObservationState::QueryFailed
                                                ? destinationObserved.nativeCode
                                                : (renameCode != ERROR_SUCCESS ? renameCode : ERROR_INVALID_DATA),
                                            "Workspace guarded rename destination could not be confirmed"));
    }

    cue::Result<void> destinationChainStable =
        verify_mutation_parent_chain(a_destination, *destinationPinned.try_value());
    cue::Result<cue::WorkspaceEntryFingerprint> destinationFingerprint =
        guard->m_isDirectory
            ? fingerprint_entry_once(a_destination, guard->m_traversalLimits, guard->m_contentLimits)
            : fingerprint_guarded_file(guard->m_rootHandle.get(), guard->m_contentLimits, m_assertContext);
    cue::Result<void> postRenameRootChange =
        guard->m_isDirectory ? verify_directory_change_guard_pending(*guard->m_rootChangeGuard, m_assertContext)
                             : cue::Result<void>::success();
    if (!destinationChainStable || !destinationFingerprint || !postRenameRootChange ||
        *destinationFingerprint.try_value() != guard->m_fingerprint ||
        sourceObserved.state != NativeEntryObservationState::Missing)
    {
        return reconciliation_required(
            !destinationChainStable   ? std::move(*destinationChainStable.try_error())
            : !destinationFingerprint ? std::move(*destinationFingerprint.try_error())
            : !postRenameRootChange   ? std::move(*postRenameRootChange.try_error())
            : *destinationFingerprint.try_value() != guard->m_fingerprint
                ? cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                     "Workspace guarded rename destination fingerprint changed")
                : make_windows_error(m_assertContext,
                                     sourceObserved.state == NativeEntryObservationState::QueryFailed
                                         ? sourceObserved.nativeCode
                                         : ERROR_INVALID_DATA,
                                     "Workspace guarded rename source remained visible"));
    }

    guard->m_path = a_destination;
    cue::WorkspaceMutationResult result;
    result.outcome = cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
    result.primaryError = cue::make_io_error(m_assertContext, cue::IoError::DurabilityUnknown,
                                             "Workspace guarded rename is visible but durability is unknown",
                                             static_cast<std::int64_t>(renameCode));
    return result;
}

cue::Result<void> WindowsWorkspaceFilesystem::finish_entry_mutation_guard(
    std::unique_ptr<cue::WorkspaceEntryMutationGuard> a_guard) noexcept
{
    auto *guard = dynamic_cast<WindowsWorkspaceEntryMutationGuard *>(a_guard.get());
    if (guard == nullptr || guard->m_owner != this)
    {
        return cue::Result<void>::failure(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                             "Workspace mutation guard belongs to another adapter"));
    }
    cue::Result<void> rootChangePending =
        guard->m_isDirectory ? verify_directory_change_guard_pending(*guard->m_rootChangeGuard, m_assertContext)
                             : cue::Result<void>::success();
    cue::Result<cue::WorkspaceEntryFingerprint> actual =
        guard->m_isDirectory
            ? fingerprint_entry_once(guard->m_path, guard->m_traversalLimits, guard->m_contentLimits)
            : fingerprint_guarded_file(guard->m_rootHandle.get(), guard->m_contentLimits, m_assertContext);
    BY_HANDLE_FILE_INFORMATION information{};
    const BOOL informationSucceeded = GetFileInformationByHandle(guard->m_rootHandle.get(), &information);
    const DWORD informationCode = informationSucceeded != FALSE ? ERROR_SUCCESS : GetLastError();
    const bool rootStable = informationSucceeded != FALSE &&
                            information.dwVolumeSerialNumber == guard->m_rootIdentity.volumeSerial &&
                            information.nFileIndexHigh == guard->m_rootIdentity.fileIndexHigh &&
                            information.nFileIndexLow == guard->m_rootIdentity.fileIndexLow &&
                            ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) == guard->m_isDirectory &&
                            (guard->m_isDirectory || information.nNumberOfLinks == 1U);
    cue::Result<void> rootChangeFinished =
        guard->m_isDirectory ? finish_directory_change_guard(*guard->m_rootChangeGuard, m_assertContext)
                             : cue::Result<void>::success();
    if (!rootChangePending || !actual || informationSucceeded == FALSE || !rootStable ||
        *actual.try_value() != guard->m_fingerprint || !rootChangeFinished)
    {
        return cue::Result<void>::failure(
            !rootChangePending              ? std::move(*rootChangePending.try_error())
            : !actual                       ? std::move(*actual.try_error())
            : informationSucceeded == FALSE ? make_windows_error(m_assertContext, informationCode,
                                                                 "Workspace mutation guard root inspection failed")
            : !rootStable ? cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                               "Workspace mutation guard root identity or link count changed")
            : *actual.try_value() != guard->m_fingerprint
                ? cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                     "Workspace mutation guard fingerprint changed before record commit")
                : std::move(*rootChangeFinished.try_error()));
    }
    return cue::Result<void>::success();
}

cue::WorkspaceMutationResult WindowsWorkspaceFilesystem::rename_entry_internal(
    const cue::BoundWorkspacePath &a_source, const cue::BoundWorkspacePath &a_destination,
    const cue::WorkspaceEntryFingerprint *a_expected, cue::TraversalLimits a_limits,
    cue::ContentVerificationLimits a_contentLimits) noexcept
{
    if (!owns_path(a_source) || !owns_path(a_destination))
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot,
                                                "Workspace transfer path belongs to another root binding"));
    }
    if (!a_limits.is_valid() || !k_windowsHardLimits.is_valid())
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::CapacityExceeded,
                                                "Workspace transfer limits must all be non-zero"));
    }

    cue::Result<cue::WorkspaceDirectory> sourceParent = parent_directory(a_source, m_assertContext);
    if (!sourceParent)
    {
        return not_committed(std::move(*sourceParent.try_error()));
    }
    cue::Result<std::vector<UniqueHandle>> sourcePinned = pin_directory_chain(*sourceParent.try_value());
    if (!sourcePinned)
    {
        return not_committed(std::move(*sourcePinned.try_error()));
    }
    if (sourcePinned.try_value()->empty())
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                "Workspace root is not a transferable entry area"));
    }
    cue::Result<std::vector<UniqueHandle>> destinationPinned = prepare_mutation_parent(a_destination);
    if (!destinationPinned)
    {
        return not_committed(std::move(*destinationPinned.try_error()));
    }

    const std::size_t sourceSeparator = a_source.text().rfind('/');
    const std::size_t destinationSeparator = a_destination.text().rfind('/');
    const std::string_view sourceLeaf =
        sourceSeparator == std::string_view::npos ? a_source.text() : a_source.text().substr(sourceSeparator + 1U);
    const std::string_view destinationLeaf = destinationSeparator == std::string_view::npos
                                                 ? a_destination.text()
                                                 : a_destination.text().substr(destinationSeparator + 1U);
    cue::Result<std::wstring> sourceName = to_utf16(sourceLeaf, m_assertContext);
    cue::Result<std::wstring> destinationName = to_utf16(destinationLeaf, m_assertContext);
    cue::Result<std::wstring> destinationPath =
        make_native_workspace_path(m_rootPath, a_destination.text(), m_assertContext);
    if (!sourceName)
    {
        return not_committed(std::move(*sourceName.try_error()));
    }
    if (!destinationName)
    {
        return not_committed(std::move(*destinationName.try_error()));
    }
    if (!destinationPath)
    {
        return not_committed(std::move(*destinationPath.try_error()));
    }

    cue::Result<OpenedNativeEntry> source =
        open_exact_entry(sourcePinned.try_value()->back().get(), *sourceName.try_value(),
                         DELETE | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ, m_assertContext);
    if (!source)
    {
        return not_committed(std::move(*source.try_error()));
    }

    cue::Result<std::wstring> sourcePath = make_native_workspace_path(m_rootPath, a_source.text(), m_assertContext);
    if (!sourcePath)
    {
        return not_committed(std::move(*sourcePath.try_error()));
    }
    const RootIdentity expectedSourceIdentity = source.try_value()->identity;
    const bool expectedSourceDirectory = source.try_value()->isDirectory;
    source.try_value()->handle.reset(CreateFileW(
        sourcePath.try_value()->c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | (expectedSourceDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0U),
        nullptr));
    if (!source.try_value()->handle.is_valid())
    {
        return not_committed(
            make_windows_error(m_assertContext, GetLastError(), "Workspace transfer source reopen failed"));
    }
    cue::Result<RootIdentity> reopenedSourceIdentity =
        read_entry_identity(source.try_value()->handle.get(), m_assertContext);
    BY_HANDLE_FILE_INFORMATION reopenedSourceInformation{};
    if (!reopenedSourceIdentity ||
        GetFileInformationByHandle(source.try_value()->handle.get(), &reopenedSourceInformation) == FALSE)
    {
        return not_committed(!reopenedSourceIdentity
                                 ? std::move(*reopenedSourceIdentity.try_error())
                                 : make_windows_error(m_assertContext, GetLastError(),
                                                      "Workspace transfer source reopen inspection failed"));
    }
    const bool reopenedDirectory = (reopenedSourceInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if (reopenedSourceIdentity.try_value()->volumeSerial != expectedSourceIdentity.volumeSerial ||
        reopenedSourceIdentity.try_value()->fileIndexHigh != expectedSourceIdentity.fileIndexHigh ||
        reopenedSourceIdentity.try_value()->fileIndexLow != expectedSourceIdentity.fileIndexLow ||
        reopenedDirectory != expectedSourceDirectory)
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                "Workspace transfer source identity changed during reopen"));
    }

    std::unique_ptr<DirectoryChangeGuard> sourceTreeGuard;
    if (source.try_value()->isDirectory)
    {
        cue::Result<std::unique_ptr<DirectoryChangeGuard>> guard = begin_directory_change_guard(
            source.try_value()->handle.get(), m_assertContext, true,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION);
        if (!guard)
        {
            return not_committed(std::move(*guard.try_error()));
        }
        sourceTreeGuard = std::move(*guard.try_value());

        cue::WorkspaceDirectory sourceDirectory = cue::WorkspaceDirectory::from_bound_path(a_source);
        cue::Result<cue::WorkspaceSearchResult> tree =
            cue::search_workspace(*this, sourceDirectory, {}, a_limits, m_assertContext);
        if (!tree || tree.try_value()->state != cue::WorkspaceSnapshotState::Complete ||
            std::any_of(tree.try_value()->entries.begin(), tree.try_value()->entries.end(),
                        /// @brief Directory Transfer対象に操作不能Entryが含まれるか判定する
                        [](const cue::WorkspaceEntry &a_entry) noexcept { return !a_entry.is_operable(); }))
        {
            cue::Result<void> guardFinished = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
            if (!tree)
            {
                return not_committed(std::move(*tree.try_error()));
            }
            if (!guardFinished)
            {
                return not_committed(std::move(*guardFinished.try_error()));
            }
            return not_committed(cue::make_io_error(m_assertContext, cue::IoError::UnsupportedEntry,
                                                    "Workspace directory tree contains an unsupported entry"));
        }
    }

    if (a_expected != nullptr)
    {
        cue::Result<cue::WorkspaceEntryFingerprint> actual =
            fingerprint_entry_once(a_source, a_limits, a_contentLimits);
        if (!actual || *actual.try_value() != *a_expected)
        {
            if (sourceTreeGuard)
            {
                cue::Result<void> ignored = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
                (void)ignored;
            }
            return not_committed(!actual ? std::move(*actual.try_error())
                                         : cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                              "Workspace verified rename source fingerprint changed"));
        }
    }

    const bool sameParent =
        sourceParent.try_value()->locator() != nullptr &&
        destinationPinned.try_value()->size() == sourcePinned.try_value()->size() &&
        sourceSeparator == destinationSeparator &&
        a_source.text().substr(0U, sourceSeparator) == a_destination.text().substr(0U, destinationSeparator);
    const int leafComparison = CompareStringOrdinal(
        sourceName.try_value()->data(), static_cast<int>(sourceName.try_value()->size()),
        destinationName.try_value()->data(), static_cast<int>(destinationName.try_value()->size()), TRUE);
    if (leafComparison == 0)
    {
        if (sourceTreeGuard)
        {
            cue::Result<void> ignored = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
            (void)ignored;
        }
        return not_committed(
            make_windows_error(m_assertContext, GetLastError(), "Workspace transfer name comparison failed"));
    }
    const bool caseOnlyRename =
        sameParent && leafComparison == CSTR_EQUAL && *sourceName.try_value() != *destinationName.try_value();
    if (!caseOnlyRename)
    {
        cue::Result<void> destinationAvailable = verify_portable_destination_absent(
            destinationPinned.try_value()->back().get(), *destinationName.try_value(), m_assertContext);
        if (!destinationAvailable)
        {
            if (sourceTreeGuard)
            {
                cue::Result<void> ignored = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
                (void)ignored;
            }
            return not_committed(std::move(*destinationAvailable.try_error()));
        }
    }

    cue::Result<RootIdentity> sourceParentIdentity =
        read_entry_identity(sourcePinned.try_value()->back().get(), m_assertContext);
    cue::Result<RootIdentity> destinationParentIdentity =
        read_entry_identity(destinationPinned.try_value()->back().get(), m_assertContext);
    if (!sourceParentIdentity || !destinationParentIdentity)
    {
        if (sourceTreeGuard)
        {
            cue::Result<void> ignored = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
            (void)ignored;
        }
        return not_committed(!sourceParentIdentity ? std::move(*sourceParentIdentity.try_error())
                                                   : std::move(*destinationParentIdentity.try_error()));
    }
    const std::array excludedParents{*sourceParentIdentity.try_value(), *destinationParentIdentity.try_value()};
    cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>> sourceParentGuards =
        begin_mutation_parent_guards(*sourcePinned.try_value(), excludedParents);
    if (!sourceParentGuards)
    {
        if (sourceTreeGuard)
        {
            cue::Result<void> ignored = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
            (void)ignored;
        }
        return not_committed(std::move(*sourceParentGuards.try_error()));
    }
    cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>> destinationParentGuards =
        begin_mutation_parent_guards(*destinationPinned.try_value(), excludedParents);
    if (!destinationParentGuards)
    {
        cue::Result<void> ignoredSourceParents = finish_mutation_parent_guards(*sourceParentGuards.try_value());
        (void)ignoredSourceParents;
        if (sourceTreeGuard)
        {
            cue::Result<void> ignoredSourceTree = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
            (void)ignoredSourceTree;
        }
        return not_committed(std::move(*destinationParentGuards.try_error()));
    }

    if (sourceTreeGuard)
    {
        cue::Result<void> sourceStableBeforePublish =
            verify_directory_change_guard_pending(*sourceTreeGuard, m_assertContext);
        if (!sourceStableBeforePublish)
        {
            cue::WorkspaceMutationResult result = not_committed(std::move(*sourceStableBeforePublish.try_error()));
            sourceTreeGuard.reset();
            cue::Result<void> sourceParentsStable = finish_mutation_parent_guards(*sourceParentGuards.try_value());
            cue::Result<void> destinationParentsStable =
                finish_mutation_parent_guards(*destinationParentGuards.try_value());
            if (!sourceParentsStable)
            {
                append_secondary(result, std::move(*sourceParentsStable.try_error()), m_assertContext);
            }
            if (!destinationParentsStable)
            {
                append_secondary(result, std::move(*destinationParentsStable.try_error()), m_assertContext);
            }
            return result;
        }
    }

    cue::Result<void> sourceParentsPending = verify_mutation_parent_guards_pending(*sourceParentGuards.try_value());
    cue::Result<void> destinationParentsPending =
        verify_mutation_parent_guards_pending(*destinationParentGuards.try_value());
    if (!sourceParentsPending || !destinationParentsPending)
    {
        cue::WorkspaceMutationResult result =
            not_committed(!sourceParentsPending ? std::move(*sourceParentsPending.try_error())
                                                : std::move(*destinationParentsPending.try_error()));
        if (!sourceParentsPending && !destinationParentsPending)
        {
            append_secondary(result, std::move(*destinationParentsPending.try_error()), m_assertContext);
        }
        cue::Result<void> sourceParentsStable = finish_mutation_parent_guards(*sourceParentGuards.try_value());
        cue::Result<void> destinationParentsStable =
            finish_mutation_parent_guards(*destinationParentGuards.try_value());
        cue::Result<void> sourceTreeStable = sourceTreeGuard
                                                 ? finish_directory_change_guard(*sourceTreeGuard, m_assertContext)
                                                 : cue::Result<void>::success();
        if (!sourceParentsStable)
        {
            append_secondary(result, std::move(*sourceParentsStable.try_error()), m_assertContext);
        }
        if (!destinationParentsStable)
        {
            append_secondary(result, std::move(*destinationParentsStable.try_error()), m_assertContext);
        }
        if (!sourceTreeStable)
        {
            append_secondary(result, std::move(*sourceTreeStable.try_error()), m_assertContext);
        }
        return result;
    }

    const DWORD renameCode =
        rename_open_entry(source.try_value()->handle.get(), destinationPinned.try_value()->back().get(),
                          *destinationName.try_value(), m_assertContext);
    if (renameCode != ERROR_SUCCESS)
    {
        const NativeEntryObservation destinationObserved = observe_native_entry(
            *destinationPath.try_value(), source.try_value()->identity, source.try_value()->isDirectory);
        if (destinationObserved.state != NativeEntryObservationState::Matches)
        {
            const NativeEntryObservation sourceObserved = observe_native_entry(
                *sourcePath.try_value(), source.try_value()->identity, source.try_value()->isDirectory);
            cue::Result<void> sourceParentsStable = finish_mutation_parent_guards(*sourceParentGuards.try_value());
            cue::Result<void> destinationParentsStable =
                finish_mutation_parent_guards(*destinationParentGuards.try_value());
            cue::Result<void> sourceTreeStable = sourceTreeGuard
                                                     ? finish_directory_change_guard(*sourceTreeGuard, m_assertContext)
                                                     : cue::Result<void>::success();
            cue::Error renameError =
                make_windows_error(m_assertContext, renameCode, "Workspace transfer rename failed");
            if (sourceObserved.state == NativeEntryObservationState::Matches && sourceParentsStable &&
                destinationParentsStable && sourceTreeStable &&
                destinationObserved.state == NativeEntryObservationState::Missing)
            {
                return not_committed(std::move(renameError));
            }
            return reconciliation_required(std::move(renameError));
        }
    }

    cue::Result<void> destinationUnique = verify_portable_destination_unique(
        destinationPinned.try_value()->back().get(), *destinationName.try_value(), source.try_value()->identity,
        source.try_value()->isDirectory, m_assertContext);
    cue::Result<void> sourceChainStable = verify_mutation_parent_chain(a_source, *sourcePinned.try_value());
    cue::Result<void> destinationChainStable =
        verify_mutation_parent_chain(a_destination, *destinationPinned.try_value());
    cue::Result<cue::WorkspaceEntryFingerprint> destinationFingerprint =
        a_expected != nullptr ? fingerprint_entry_once(a_destination, a_limits, a_contentLimits)
                              : cue::Result<cue::WorkspaceEntryFingerprint>::success(cue::WorkspaceEntryFingerprint{});
    const bool destinationMatches =
        a_expected == nullptr || (destinationFingerprint && *destinationFingerprint.try_value() == *a_expected);
    cue::Result<void> sourceParentsStable = finish_mutation_parent_guards(*sourceParentGuards.try_value());
    cue::Result<void> destinationParentsStable = finish_mutation_parent_guards(*destinationParentGuards.try_value());
    cue::Result<void> sourceTreeStable = sourceTreeGuard
                                             ? finish_directory_change_guard(*sourceTreeGuard, m_assertContext)
                                             : cue::Result<void>::success();
    const NativeEntryObservation oldSourceObserved =
        caseOnlyRename ? NativeEntryObservation{NativeEntryObservationState::Missing, ERROR_SUCCESS}
                       : observe_native_entry(*sourcePath.try_value(), source.try_value()->identity,
                                              source.try_value()->isDirectory);
    if (!destinationUnique || !sourceChainStable || !destinationChainStable || !sourceParentsStable ||
        !destinationParentsStable || !sourceTreeStable || !destinationMatches ||
        (!caseOnlyRename && oldSourceObserved.state != NativeEntryObservationState::Missing))
    {
        cue::Error error = !destinationUnique          ? std::move(*destinationUnique.try_error())
                           : !sourceChainStable        ? std::move(*sourceChainStable.try_error())
                           : !destinationChainStable   ? std::move(*destinationChainStable.try_error())
                           : !sourceParentsStable      ? std::move(*sourceParentsStable.try_error())
                           : !destinationParentsStable ? std::move(*destinationParentsStable.try_error())
                           : !sourceTreeStable         ? std::move(*sourceTreeStable.try_error())
                           : !destinationFingerprint   ? std::move(*destinationFingerprint.try_error())
                           : !destinationMatches
                               ? cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                    "Workspace verified rename destination fingerprint changed")
                               : make_windows_error(m_assertContext,
                                                    oldSourceObserved.state == NativeEntryObservationState::QueryFailed
                                                        ? oldSourceObserved.nativeCode
                                                        : ERROR_INVALID_DATA,
                                                    "Workspace transfer source remained visible after rename");
        return reconciliation_required(std::move(error));
    }

    cue::WorkspaceMutationResult result;
    result.outcome = cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
    result.primaryError = cue::make_io_error(m_assertContext, cue::IoError::DurabilityUnknown,
                                             "Workspace transfer is visible but rename durability is unknown",
                                             static_cast<std::int64_t>(renameCode));
    return result;
}

cue::WorkspaceMutationResult WindowsWorkspaceFilesystem::copy_entry_new(const cue::BoundWorkspacePath &a_source,
                                                                        const cue::BoundWorkspacePath &a_destination,
                                                                        cue::TraversalLimits a_traversalLimits,
                                                                        cue::ContentVerificationLimits a_contentLimits,
                                                                        std::string_view a_operationId) noexcept
{
    if (!owns_path(a_source) || !owns_path(a_destination))
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot,
                                                "Workspace copy path belongs to another root binding"));
    }
    if (!is_valid_operation_id(a_operationId) || !a_traversalLimits.is_valid() || !a_contentLimits.is_valid() ||
        !k_windowsContentHardLimits.is_valid())
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::CapacityExceeded,
                                                "Workspace copy limits or operation id are invalid"));
    }

    const cue::ContentVerificationLimits effectiveContent{
        std::min(a_contentLimits.maxFileBytes, k_windowsContentHardLimits.maxFileBytes),
        std::min(a_contentLimits.maxTotalBytes, k_windowsContentHardLimits.maxTotalBytes)};
    cue::Result<cue::WorkspaceDirectory> sourceParent = parent_directory(a_source, m_assertContext);
    if (!sourceParent)
    {
        return not_committed(std::move(*sourceParent.try_error()));
    }
    cue::Result<std::vector<UniqueHandle>> sourcePinned = pin_directory_chain(*sourceParent.try_value());
    if (!sourcePinned)
    {
        return not_committed(std::move(*sourcePinned.try_error()));
    }
    cue::Result<cue::WorkspaceDirectory> destinationParent = parent_directory(a_destination, m_assertContext);
    if (!destinationParent)
    {
        return not_committed(std::move(*destinationParent.try_error()));
    }
    cue::Result<std::vector<UniqueHandle>> destinationPinned = prepare_mutation_parent(a_destination);
    if (!destinationPinned)
    {
        return not_committed(std::move(*destinationPinned.try_error()));
    }

    const std::size_t sourceSeparator = a_source.text().rfind('/');
    const std::size_t destinationSeparator = a_destination.text().rfind('/');
    const std::string_view sourceLeaf = a_source.text().substr(sourceSeparator + 1U);
    const std::string_view destinationLeaf = a_destination.text().substr(destinationSeparator + 1U);
    cue::Result<std::wstring> sourceName = to_utf16(sourceLeaf, m_assertContext);
    cue::Result<std::wstring> destinationName = to_utf16(destinationLeaf, m_assertContext);
    cue::Result<std::wstring> destinationPath =
        make_native_workspace_path(m_rootPath, a_destination.text(), m_assertContext);
    if (!sourceName)
    {
        return not_committed(std::move(*sourceName.try_error()));
    }
    if (!destinationName)
    {
        return not_committed(std::move(*destinationName.try_error()));
    }
    if (!destinationPath)
    {
        return not_committed(std::move(*destinationPath.try_error()));
    }

    HANDLE sourceParentHandle =
        sourcePinned.try_value()->empty() ? m_rootHandle.get() : sourcePinned.try_value()->back().get();
    cue::Result<OpenedNativeEntry> source =
        open_exact_entry(sourceParentHandle, *sourceName.try_value(),
                         GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ, m_assertContext);
    if (!source)
    {
        return not_committed(std::move(*source.try_error()));
    }
    if (source.try_value()->isDirectory)
    {
        cue::Result<OpenedNativeEntry> overlappedSource = open_exact_entry(
            sourceParentHandle, *sourceName.try_value(), GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ, m_assertContext, true);
        if (!overlappedSource)
        {
            return not_committed(std::move(*overlappedSource.try_error()));
        }
        source = std::move(overlappedSource);
    }
    cue::Result<void> destinationAvailable = verify_portable_destination_absent(
        destinationPinned.try_value()->back().get(), *destinationName.try_value(), m_assertContext);
    if (!destinationAvailable)
    {
        return not_committed(std::move(*destinationAvailable.try_error()));
    }

    cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>> destinationParentGuards =
        begin_mutation_parent_guards(*destinationPinned.try_value());
    if (!destinationParentGuards)
    {
        return not_committed(std::move(*destinationParentGuards.try_error()));
    }

    std::string stagingText;
    try
    {
        if (destinationSeparator != std::string_view::npos)
        {
            stagingText.assign(a_destination.text().substr(0U, destinationSeparator + 1U));
        }
        stagingText.append("cuecopy-");
        stagingText.append(a_operationId);
        stagingText.append(source.try_value()->isDirectory ? "-dir-staging" : "-file-staging");
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    cue::Result<std::wstring> stagingPath = make_native_workspace_path(m_rootPath, stagingText, m_assertContext);
    if (!stagingPath)
    {
        return not_committed(std::move(*stagingPath.try_error()));
    }
    const std::size_t stagingNameOffset = stagingPath.try_value()->rfind(L'\\');
    if (stagingNameOffset == std::wstring::npos || stagingNameOffset + 1U == stagingPath.try_value()->size())
    {
        return not_committed(
            cue::make_io_error(m_assertContext, cue::IoError::InvalidPath, "Workspace copy staging name is invalid"));
    }
    const std::wstring_view stagingName = std::wstring_view(*stagingPath.try_value()).substr(stagingNameOffset + 1U);

    UniqueHandle stagingHandle;
    RootIdentity stagingIdentity{};
    std::vector<UniqueHandle> sourceChildren;
    std::vector<std::unique_ptr<DirectoryOplockGuard>> sourceDirectoryOplocks;
    std::vector<UniqueHandle> stagingChildren;
    std::vector<OwnedStagingChild> stagingChildRecords;
    std::unique_ptr<DirectoryChangeGuard> sourceTreeGuard;
    std::unique_ptr<DirectoryChangeGuard> sourcePublishGuard;

    /// @brief 保持中のSource Directory Oplockを完了し、最初のBreakを返す
    const auto finishSourceDirectoryOplocks = [&]() noexcept
    { return finish_directory_oplock_guards(sourceDirectoryOplocks, m_assertContext); };

    /// @brief Publish前のOperation-owned Staging Treeを安全に除去して失敗結果を返す
    const auto cleanupFailure = [&](cue::Error a_error) noexcept
    {
        cue::WorkspaceMutationResult result = not_committed(std::move(a_error));
        if (stagingChildren.empty())
        {
            cleanup_staging_child_records(stagingChildRecords, result, m_assertContext);
        }
        else
        {
            cleanup_staging_children(stagingChildren, result, m_assertContext);
            stagingChildRecords.clear();
        }
        if (stagingHandle.is_valid())
        {
            cleanup_owned_temporary(stagingHandle, *stagingPath.try_value(), stagingIdentity,
                                    source.try_value()->isDirectory, result, m_assertContext);
        }
        cue::Result<void> parentStable = finish_mutation_parent_guards(*destinationParentGuards.try_value());
        if (!parentStable)
        {
            append_secondary(result, std::move(*parentStable.try_error()), m_assertContext);
            result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
        }
        if (sourceTreeGuard)
        {
            cue::Result<void> sourceStable = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
            sourceTreeGuard.reset();
            if (!sourceStable)
            {
                append_secondary(result, std::move(*sourceStable.try_error()), m_assertContext);
                result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
            }
        }
        if (sourcePublishGuard)
        {
            cue::Result<void> sourceStable = finish_directory_change_guard(*sourcePublishGuard, m_assertContext);
            sourcePublishGuard.reset();
            if (!sourceStable)
            {
                append_secondary(result, std::move(*sourceStable.try_error()), m_assertContext);
                result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
            }
        }
        cue::Result<void> sourceOplocksStable = finishSourceDirectoryOplocks();
        if (!sourceOplocksStable)
        {
            append_secondary(result, std::move(*sourceOplocksStable.try_error()), m_assertContext);
            result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
        }
        return result;
    };

    /// @brief Publish結果を観測できない場合に候補Dataを保持して照合要求を返す
    const auto reconcilePublishFailure = [&](DWORD a_publishCode, DWORD a_observationCode,
                                             std::string_view a_publishMessage,
                                             std::string_view a_observationMessage) noexcept
    {
        cue::WorkspaceMutationResult result =
            reconciliation_required(make_windows_error(m_assertContext, a_publishCode, a_publishMessage));
        append_secondary(result, make_windows_error(m_assertContext, a_observationCode, a_observationMessage),
                         m_assertContext);
        cue::Result<void> parentStable = finish_mutation_parent_guards(*destinationParentGuards.try_value());
        if (!parentStable)
        {
            append_secondary(result, std::move(*parentStable.try_error()), m_assertContext);
        }
        if (sourceTreeGuard)
        {
            cue::Result<void> sourceStable = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
            sourceTreeGuard.reset();
            if (!sourceStable)
            {
                append_secondary(result, std::move(*sourceStable.try_error()), m_assertContext);
            }
        }
        if (sourcePublishGuard)
        {
            cue::Result<void> sourceStable = finish_directory_change_guard(*sourcePublishGuard, m_assertContext);
            sourcePublishGuard.reset();
            if (!sourceStable)
            {
                append_secondary(result, std::move(*sourceStable.try_error()), m_assertContext);
            }
        }
        cue::Result<void> sourceOplocksStable = finishSourceDirectoryOplocks();
        if (!sourceOplocksStable)
        {
            append_secondary(result, std::move(*sourceOplocksStable.try_error()), m_assertContext);
        }
        return result;
    };

    /// @brief Publish失敗時に親とStaging Identityを確定できた場合だけ候補DataをCleanupする
    const auto cleanupConfirmedStagingAfterPublishFailure =
        [&](DWORD a_publishCode, std::string_view a_publishMessage) noexcept
    {
        cue::Result<void> parentStable = finish_mutation_parent_guards(*destinationParentGuards.try_value());
        if (!parentStable)
        {
            cue::WorkspaceMutationResult result =
                reconciliation_required(make_windows_error(m_assertContext, a_publishCode, a_publishMessage));
            append_secondary(result, std::move(*parentStable.try_error()), m_assertContext);
            if (sourceTreeGuard)
            {
                cue::Result<void> sourceStable = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
                sourceTreeGuard.reset();
                if (!sourceStable)
                {
                    append_secondary(result, std::move(*sourceStable.try_error()), m_assertContext);
                }
            }
            if (sourcePublishGuard)
            {
                cue::Result<void> sourceStable = finish_directory_change_guard(*sourcePublishGuard, m_assertContext);
                sourcePublishGuard.reset();
                if (!sourceStable)
                {
                    append_secondary(result, std::move(*sourceStable.try_error()), m_assertContext);
                }
            }
            cue::Result<void> sourceOplocksStable = finishSourceDirectoryOplocks();
            if (!sourceOplocksStable)
            {
                append_secondary(result, std::move(*sourceOplocksStable.try_error()), m_assertContext);
            }
            return result;
        }

        const NativeEntryObservation stagingObserved =
            observe_native_entry(*stagingPath.try_value(), stagingIdentity, source.try_value()->isDirectory);
        if (stagingObserved.state == NativeEntryObservationState::Matches)
        {
            return cleanupFailure(make_windows_error(m_assertContext, a_publishCode, a_publishMessage));
        }

        cue::WorkspaceMutationResult result =
            reconciliation_required(make_windows_error(m_assertContext, a_publishCode, a_publishMessage));
        if (stagingObserved.state == NativeEntryObservationState::Different)
        {
            append_secondary(result,
                             cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                "Workspace copy staging identity changed after publish failure"),
                             m_assertContext);
        }
        else
        {
            append_secondary(result,
                             make_windows_error(m_assertContext, stagingObserved.nativeCode,
                                                "Workspace copy staging location could not be confirmed"),
                             m_assertContext);
        }
        if (sourceTreeGuard)
        {
            cue::Result<void> sourceStable = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
            sourceTreeGuard.reset();
            if (!sourceStable)
            {
                append_secondary(result, std::move(*sourceStable.try_error()), m_assertContext);
            }
        }
        if (sourcePublishGuard)
        {
            cue::Result<void> sourceStable = finish_directory_change_guard(*sourcePublishGuard, m_assertContext);
            sourcePublishGuard.reset();
            if (!sourceStable)
            {
                append_secondary(result, std::move(*sourceStable.try_error()), m_assertContext);
            }
        }
        cue::Result<void> sourceOplocksStable = finishSourceDirectoryOplocks();
        if (!sourceOplocksStable)
        {
            append_secondary(result, std::move(*sourceOplocksStable.try_error()), m_assertContext);
        }
        return result;
    };

    if (!source.try_value()->isDirectory)
    {
        cue::Result<std::vector<std::byte>> sourceBytes =
            read_open_file(source.try_value()->handle.get(), effectiveContent.maxFileBytes, m_assertContext);
        if (!sourceBytes)
        {
            return cleanupFailure(std::move(*sourceBytes.try_error()));
        }
        if (sourceBytes.try_value()->size() > effectiveContent.maxTotalBytes)
        {
            return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::CapacityExceeded,
                                                     "Workspace copy exceeds the total content limit"));
        }

        stagingHandle.reset(CreateFileW(
            stagingPath.try_value()->c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!stagingHandle.is_valid())
        {
            return cleanupFailure(
                make_windows_error(m_assertContext, GetLastError(), "Workspace copy temporary file creation failed"));
        }
        cue::Result<RootIdentity> identity = read_entry_identity(stagingHandle.get(), m_assertContext);
        if (!identity)
        {
            return cleanupFailure(std::move(*identity.try_error()));
        }
        stagingIdentity = *identity.try_value();

        cue::Result<void> written =
            write_and_flush_file(stagingHandle.get(), *sourceBytes.try_value(), m_assertContext);
        if (!written)
        {
            return cleanupFailure(std::move(*written.try_error()));
        }
        cue::Result<std::vector<std::byte>> verified =
            read_open_file(stagingHandle.get(), effectiveContent.maxFileBytes, m_assertContext);
        if (!verified)
        {
            return cleanupFailure(std::move(*verified.try_error()));
        }
        if (verified.try_value()->size() != sourceBytes.try_value()->size() ||
            content_digest(*verified.try_value()) != content_digest(*sourceBytes.try_value()) ||
            *verified.try_value() != *sourceBytes.try_value())
        {
            return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                     "Workspace copied file content verification failed"));
        }
    }
    else
    {
        cue::Result<std::unique_ptr<DirectoryChangeGuard>> sourceGuard = begin_directory_change_guard(
            source.try_value()->handle.get(), m_assertContext, true,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION);
        if (!sourceGuard)
        {
            return cleanupFailure(std::move(*sourceGuard.try_error()));
        }
        sourceTreeGuard = std::move(*sourceGuard.try_value());

        cue::WorkspaceDirectory sourceDirectory = cue::WorkspaceDirectory::from_bound_path(a_source);
        cue::Result<cue::WorkspaceSearchResult> sourceTree =
            cue::search_workspace(*this, sourceDirectory, {}, a_traversalLimits, m_assertContext);
        if (!sourceTree)
        {
            return cleanupFailure(std::move(*sourceTree.try_error()));
        }
        if (sourceTree.try_value()->state != cue::WorkspaceSnapshotState::Complete ||
            std::any_of(sourceTree.try_value()->entries.begin(), sourceTree.try_value()->entries.end(),
                        /// @brief Directory Copy対象に操作不能Entryが含まれるか判定する
                        [](const cue::WorkspaceEntry &a_entry) noexcept { return !a_entry.is_operable(); }))
        {
            return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::UnsupportedEntry,
                                                     "Workspace copied directory contains an unsupported entry"));
        }

        const DWORD createRootCode =
            create_directory_exclusive(destinationPinned.try_value()->back().get(), stagingName, stagingHandle);
        if (createRootCode != ERROR_SUCCESS)
        {
            return cleanupFailure(make_windows_error(m_assertContext, createRootCode,
                                                     "Workspace copy staging directory creation failed"));
        }
        cue::Result<RootIdentity> identity = read_entry_identity(stagingHandle.get(), m_assertContext);
        if (!identity)
        {
            return cleanupFailure(std::move(*identity.try_error()));
        }
        stagingIdentity = *identity.try_value();

        std::vector<std::pair<std::string, HANDLE>> sourceDirectories;
        std::vector<std::pair<std::string, HANDLE>> stagingDirectories;
        std::vector<const cue::WorkspaceEntry *> orderedEntries;
        try
        {
            sourceDirectories.reserve(sourceTree.try_value()->entries.size() + 1U);
            sourceDirectories.emplace_back(std::string{}, source.try_value()->handle.get());
            sourceChildren.reserve(sourceTree.try_value()->entries.size());
            stagingDirectories.reserve(sourceTree.try_value()->entries.size() + 1U);
            stagingDirectories.emplace_back(std::string{}, stagingHandle.get());
            orderedEntries.reserve(sourceTree.try_value()->entries.size());
            for (const cue::WorkspaceEntry &entry : sourceTree.try_value()->entries)
            {
                orderedEntries.push_back(&entry);
            }
            std::stable_sort(orderedEntries.begin(), orderedEntries.end(),
                             /// @brief Staging Treeを親Directoryから子Entryへ構築できる順に並べる
                             [&](const cue::WorkspaceEntry *a_left, const cue::WorkspaceEntry *a_right) noexcept
                             {
                                 const std::string_view leftSuffix =
                                     a_left->locator->text().substr(a_source.text().size() + 1U);
                                 const std::string_view rightSuffix =
                                     a_right->locator->text().substr(a_source.text().size() + 1U);
                                 const std::size_t leftDepth =
                                     static_cast<std::size_t>(std::count(leftSuffix.begin(), leftSuffix.end(), '/'));
                                 const std::size_t rightDepth =
                                     static_cast<std::size_t>(std::count(rightSuffix.begin(), rightSuffix.end(), '/'));
                                 if (leftDepth != rightDepth)
                                 {
                                     return leftDepth < rightDepth;
                                 }
                                 if (a_left->type != a_right->type)
                                 {
                                     return a_left->type == cue::WorkspaceEntryType::Directory;
                                 }
                                 return leftSuffix < rightSuffix;
                             });
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }

        std::uint64_t totalBytes = 0U;
        for (const cue::WorkspaceEntry *entryPointer : orderedEntries)
        {
            const cue::WorkspaceEntry &entry = *entryPointer;
            const std::string_view sourceLocator = entry.locator->text();
            if (sourceLocator.size() <= a_source.text().size() || sourceLocator[a_source.text().size()] != '/')
            {
                return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot,
                                                         "Workspace copied entry escaped the source directory"));
            }
            const std::string_view suffix = sourceLocator.substr(a_source.text().size() + 1U);
            const std::size_t suffixSeparator = suffix.rfind('/');
            const std::string_view parentSuffix =
                suffixSeparator == std::string_view::npos ? std::string_view{} : suffix.substr(0U, suffixSeparator);
            const std::string_view leaf =
                suffixSeparator == std::string_view::npos ? suffix : suffix.substr(suffixSeparator + 1U);
            const auto parentIterator =
                std::find_if(stagingDirectories.begin(), stagingDirectories.end(),
                             /// @brief Staging Childの作成元となるIdentity固定済み親Handleを検索する
                             [&](const std::pair<std::string, HANDLE> &a_directory) noexcept
                             { return a_directory.first == parentSuffix; });
            if (parentIterator == stagingDirectories.end())
            {
                return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                         "Workspace staged child parent is missing"));
            }
            const auto sourceParentIterator =
                std::find_if(sourceDirectories.begin(), sourceDirectories.end(),
                             /// @brief Source Childの固定元となるIdentity固定済み親Handleを検索する
                             [&](const std::pair<std::string, HANDLE> &a_directory) noexcept
                             { return a_directory.first == parentSuffix; });
            if (sourceParentIterator == sourceDirectories.end())
            {
                return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                         "Workspace source child parent is missing"));
            }
            cue::Result<std::wstring> childName = to_utf16(leaf, m_assertContext);
            if (!childName)
            {
                return cleanupFailure(std::move(*childName.try_error()));
            }

            const bool sourceIsDirectory = entry.type == cue::WorkspaceEntryType::Directory;
            const DWORD sourceAccess = sourceIsDirectory ? GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY
                                                         : GENERIC_READ | FILE_READ_ATTRIBUTES;
            cue::Result<OpenedNativeEntry> sourceChild =
                open_exact_entry(sourceParentIterator->second, *childName.try_value(), sourceAccess, FILE_SHARE_READ,
                                 m_assertContext, sourceIsDirectory);
            if (!sourceChild)
            {
                return cleanupFailure(std::move(*sourceChild.try_error()));
            }
            if (sourceChild.try_value()->isDirectory != sourceIsDirectory)
            {
                return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                         "Workspace source child type changed"));
            }

            if (entry.type == cue::WorkspaceEntryType::Directory)
            {
                UniqueHandle child;
                const DWORD createCode =
                    create_directory_exclusive(parentIterator->second, *childName.try_value(), child);
                if (createCode != ERROR_SUCCESS)
                {
                    return cleanupFailure(make_windows_error(m_assertContext, createCode,
                                                             "Workspace staged child directory creation failed"));
                }
                try
                {
                    sourceDirectories.emplace_back(std::string(suffix), sourceChild.try_value()->handle.get());
                    sourceChildren.push_back(std::move(sourceChild.try_value()->handle));
                    stagingDirectories.emplace_back(std::string(suffix), child.get());
                    stagingChildren.push_back(std::move(child));
                }
                catch (...)
                {
                    terminate_allocation(m_assertContext);
                }
                continue;
            }

            cue::Result<std::vector<std::byte>> bytes =
                read_open_file(sourceChild.try_value()->handle.get(), effectiveContent.maxFileBytes, m_assertContext);
            if (!bytes)
            {
                return cleanupFailure(std::move(*bytes.try_error()));
            }
            if (bytes.try_value()->size() != entry.byteSize)
            {
                return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                         "Workspace source child size changed"));
            }
            if (bytes.try_value()->size() > effectiveContent.maxTotalBytes - totalBytes)
            {
                return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::CapacityExceeded,
                                                         "Workspace directory copy exceeds the total content limit"));
            }
            totalBytes += bytes.try_value()->size();
            UniqueHandle child;
            const DWORD createCode = create_file_exclusive(parentIterator->second, *childName.try_value(), child);
            if (createCode != ERROR_SUCCESS)
            {
                return cleanupFailure(
                    make_windows_error(m_assertContext, createCode, "Workspace staged child file creation failed"));
            }
            cue::Result<void> written = write_and_flush_file(child.get(), *bytes.try_value(), m_assertContext);
            if (!written)
            {
                stagingChildren.push_back(std::move(child));
                return cleanupFailure(std::move(*written.try_error()));
            }
            cue::Result<std::vector<std::byte>> verified =
                read_open_file(child.get(), effectiveContent.maxFileBytes, m_assertContext);
            if (!verified)
            {
                stagingChildren.push_back(std::move(child));
                return cleanupFailure(std::move(*verified.try_error()));
            }
            if (*verified.try_value() != *bytes.try_value() ||
                content_digest(*verified.try_value()) != content_digest(*bytes.try_value()))
            {
                stagingChildren.push_back(std::move(child));
                return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                         "Workspace staged child file verification failed"));
            }
            sourceChildren.push_back(std::move(sourceChild.try_value()->handle));
            stagingChildren.push_back(std::move(child));
        }

        try
        {
            sourceDirectoryOplocks.reserve(sourceDirectories.size());
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        for (const std::pair<std::string, HANDLE> &directory : sourceDirectories)
        {
            cue::Result<std::unique_ptr<DirectoryOplockGuard>> oplock =
                begin_directory_oplock_guard(directory.second, m_assertContext);
            if (!oplock)
            {
                return cleanupFailure(std::move(*oplock.try_error()));
            }
            sourceDirectoryOplocks.push_back(std::move(*oplock.try_value()));
        }

        try
        {
            stagingChildRecords.reserve(stagingChildren.size());
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        for (UniqueHandle &child : stagingChildren)
        {
            cue::Result<OwnedStagingChild> record = capture_staging_child(child.get(), m_assertContext);
            if (!record)
            {
                return cleanupFailure(std::move(*record.try_error()));
            }
            stagingChildRecords.push_back(std::move(*record.try_value()));
        }

        const std::string_view stagingLeafText = destinationSeparator == std::string_view::npos
                                                     ? std::string_view(stagingText)
                                                     : std::string_view(stagingText).substr(destinationSeparator + 1U);
        cue::Result<cue::RelativePath> stagingRelative = cue::RelativePath::parse(stagingLeafText, m_assertContext);
        if (!stagingRelative)
        {
            return cleanupFailure(std::move(*stagingRelative.try_error()));
        }
        cue::Result<cue::BoundWorkspacePath> stagingBound =
            append_path(*destinationParent.try_value(), std::move(*stagingRelative.try_value()), m_assertContext);
        if (!stagingBound)
        {
            return cleanupFailure(std::move(*stagingBound.try_error()));
        }
        cue::WorkspaceDirectory stagingDirectory = cue::WorkspaceDirectory::from_bound_path(*stagingBound.try_value());

        /// @brief SourceとStagingのRoot相対Suffix、種別、Sizeが一致するか判定する
        const auto manifestMatches = [&](const cue::WorkspaceSearchResult &a_stagedTree) noexcept
        {
            if (a_stagedTree.state != cue::WorkspaceSnapshotState::Complete ||
                a_stagedTree.entries.size() != sourceTree.try_value()->entries.size())
            {
                return false;
            }
            for (std::size_t index = 0U; index < a_stagedTree.entries.size(); ++index)
            {
                const cue::WorkspaceEntry &sourceEntry = sourceTree.try_value()->entries[index];
                const cue::WorkspaceEntry &stagedEntry = a_stagedTree.entries[index];
                if (!sourceEntry.locator.has_value() || !stagedEntry.locator.has_value())
                {
                    return false;
                }
                const std::string_view sourceLocator = sourceEntry.locator->text();
                const std::string_view stagedLocator = stagedEntry.locator->text();
                if (sourceLocator.size() <= a_source.text().size() || stagedLocator.size() <= stagingText.size() ||
                    sourceLocator.substr(a_source.text().size()) != stagedLocator.substr(stagingText.size()) ||
                    sourceEntry.type != stagedEntry.type || sourceEntry.byteSize != stagedEntry.byteSize ||
                    !stagedEntry.is_operable())
                {
                    return false;
                }
            }
            return true;
        };

        cue::Result<cue::WorkspaceSearchResult> stagedTree =
            cue::search_workspace(*this, stagingDirectory, {}, a_traversalLimits, m_assertContext);
        if (!stagedTree)
        {
            return cleanupFailure(std::move(*stagedTree.try_error()));
        }
        if (!manifestMatches(*stagedTree.try_value()))
        {
            return cleanupFailure(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                     "Workspace staged directory manifest verification failed"));
        }

        cue::Result<std::unique_ptr<DirectoryChangeGuard>> stagingGuard = begin_directory_change_guard(
            stagingHandle.get(), m_assertContext, true,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION);
        if (!stagingGuard)
        {
            return cleanupFailure(std::move(*stagingGuard.try_error()));
        }
        cue::Result<cue::WorkspaceSearchResult> stableStagedTree =
            cue::search_workspace(*this, stagingDirectory, {}, a_traversalLimits, m_assertContext);
        if (!stableStagedTree || !manifestMatches(*stableStagedTree.try_value()))
        {
            cue::Error error = !stableStagedTree ? std::move(*stableStagedTree.try_error())
                                                 : cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                                      "Workspace staged directory manifest changed");
            cue::Result<void> ignoredGuard = finish_directory_change_guard(**stagingGuard.try_value(), m_assertContext);
            (void)ignoredGuard;
            return cleanupFailure(std::move(error));
        }

        cue::Result<std::unique_ptr<DirectoryChangeGuard>> stagingPublishGuard = begin_directory_change_guard(
            stagingHandle.get(), m_assertContext, true,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION);
        if (!stagingPublishGuard)
        {
            cue::Result<void> ignoredGuard = finish_directory_change_guard(**stagingGuard.try_value(), m_assertContext);
            (void)ignoredGuard;
            return cleanupFailure(std::move(*stagingPublishGuard.try_error()));
        }

        cue::Result<std::unique_ptr<DirectoryChangeGuard>> sourcePublish = begin_directory_change_guard(
            source.try_value()->handle.get(), m_assertContext, true,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION);
        if (!sourcePublish)
        {
            cue::Result<void> ignoredStagingGuard =
                finish_directory_change_guard(**stagingGuard.try_value(), m_assertContext);
            (void)ignoredStagingGuard;
            cue::Result<void> ignoredPublishGuard =
                finish_directory_change_guard(**stagingPublishGuard.try_value(), m_assertContext);
            (void)ignoredPublishGuard;
            return cleanupFailure(std::move(*sourcePublish.try_error()));
        }
        sourcePublishGuard = std::move(*sourcePublish.try_value());

        cue::Result<void> sourceStableBeforePublish = finish_directory_change_guard(*sourceTreeGuard, m_assertContext);
        if (!sourceStableBeforePublish)
        {
            sourceTreeGuard.reset();
            cue::Result<void> ignoredStagingGuard =
                finish_directory_change_guard(**stagingGuard.try_value(), m_assertContext);
            (void)ignoredStagingGuard;
            cue::Result<void> ignoredPublishGuard =
                finish_directory_change_guard(**stagingPublishGuard.try_value(), m_assertContext);
            (void)ignoredPublishGuard;
            stagingGuard.try_value()->reset();
            stagingPublishGuard.try_value()->reset();
            return cleanupFailure(std::move(*sourceStableBeforePublish.try_error()));
        }
        sourceTreeGuard.reset();
        cue::Result<void> stagingStableBeforePublish =
            finish_directory_change_guard(**stagingGuard.try_value(), m_assertContext);
        if (!stagingStableBeforePublish)
        {
            stagingGuard.try_value()->reset();
            cue::Result<void> ignoredPublishGuard =
                finish_directory_change_guard(**stagingPublishGuard.try_value(), m_assertContext);
            (void)ignoredPublishGuard;
            stagingPublishGuard.try_value()->reset();
            return cleanupFailure(std::move(*stagingStableBeforePublish.try_error()));
        }
        stagingGuard.try_value()->reset();

        destinationAvailable = verify_portable_destination_absent(destinationPinned.try_value()->back().get(),
                                                                  *destinationName.try_value(), m_assertContext);
        if (!destinationAvailable)
        {
            cue::Result<void> stagingPublishStable =
                finish_directory_change_guard(**stagingPublishGuard.try_value(), m_assertContext);
            stagingPublishGuard.try_value()->reset();
            cue::Result<void> sourcePublishStable = finish_directory_change_guard(*sourcePublishGuard, m_assertContext);
            sourcePublishGuard.reset();
            cue::Result<void> sourceOplocksStable = finishSourceDirectoryOplocks();
            if (!stagingPublishStable || !sourcePublishStable || !sourceOplocksStable)
            {
                cue::WorkspaceMutationResult result =
                    reconciliation_required(!stagingPublishStable  ? std::move(*stagingPublishStable.try_error())
                                            : !sourcePublishStable ? std::move(*sourcePublishStable.try_error())
                                                                   : std::move(*sourceOplocksStable.try_error()));
                append_secondary(result, std::move(*destinationAvailable.try_error()), m_assertContext);
                if (!stagingPublishStable && !sourcePublishStable)
                {
                    append_secondary(result, std::move(*sourcePublishStable.try_error()), m_assertContext);
                }
                if (!sourceOplocksStable && (!stagingPublishStable || !sourcePublishStable))
                {
                    append_secondary(result, std::move(*sourceOplocksStable.try_error()), m_assertContext);
                }
                cue::Result<void> parentStable = finish_mutation_parent_guards(*destinationParentGuards.try_value());
                if (!parentStable)
                {
                    append_secondary(result, std::move(*parentStable.try_error()), m_assertContext);
                }
                return result;
            }
            return cleanupFailure(std::move(*destinationAvailable.try_error()));
        }
        for (UniqueHandle &child : stagingChildren)
        {
            child.reset();
        }
        stagingChildren.clear();
        cue::Result<void> sourcePublishStableBeforePublish =
            verify_directory_change_guard_pending(*sourcePublishGuard, m_assertContext);
        if (!sourcePublishStableBeforePublish)
        {
            sourcePublishGuard.reset();
            cue::Result<void> stagingPublishStable =
                finish_directory_change_guard(**stagingPublishGuard.try_value(), m_assertContext);
            stagingPublishGuard.try_value()->reset();
            cue::WorkspaceMutationResult result =
                cleanupFailure(std::move(*sourcePublishStableBeforePublish.try_error()));
            if (!stagingPublishStable)
            {
                append_secondary(result, std::move(*stagingPublishStable.try_error()), m_assertContext);
                result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
            }
            return result;
        }
        cue::Result<void> stagingPublishStableBeforePublish =
            verify_directory_change_guard_pending(**stagingPublishGuard.try_value(), m_assertContext);
        if (!stagingPublishStableBeforePublish)
        {
            stagingPublishGuard.try_value()->reset();
            cue::Result<void> sourcePublishStable = finish_directory_change_guard(*sourcePublishGuard, m_assertContext);
            sourcePublishGuard.reset();
            cue::WorkspaceMutationResult result =
                cleanupFailure(std::move(*stagingPublishStableBeforePublish.try_error()));
            if (!sourcePublishStable)
            {
                append_secondary(result, std::move(*sourcePublishStable.try_error()), m_assertContext);
                result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
            }
            return result;
        }
        cue::Result<void> sourceOplocksPending =
            verify_directory_oplock_guards_pending(sourceDirectoryOplocks, m_assertContext);
        if (!sourceOplocksPending)
        {
            cue::Result<void> stagingPublishStable =
                finish_directory_change_guard(**stagingPublishGuard.try_value(), m_assertContext);
            stagingPublishGuard.try_value()->reset();
            cue::Result<void> sourcePublishStable = finish_directory_change_guard(*sourcePublishGuard, m_assertContext);
            sourcePublishGuard.reset();
            cue::WorkspaceMutationResult result = cleanupFailure(std::move(*sourceOplocksPending.try_error()));
            if (!stagingPublishStable)
            {
                append_secondary(result, std::move(*stagingPublishStable.try_error()), m_assertContext);
                result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
            }
            if (!sourcePublishStable)
            {
                append_secondary(result, std::move(*sourcePublishStable.try_error()), m_assertContext);
                result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
            }
            return result;
        }
        cue::Result<void> destinationParentsPending =
            verify_mutation_parent_guards_pending(*destinationParentGuards.try_value());
        if (!destinationParentsPending)
        {
            cue::Result<void> stagingPublishStable =
                finish_directory_change_guard(**stagingPublishGuard.try_value(), m_assertContext);
            stagingPublishGuard.try_value()->reset();
            cue::Result<void> sourcePublishStable = finish_directory_change_guard(*sourcePublishGuard, m_assertContext);
            sourcePublishGuard.reset();
            cue::WorkspaceMutationResult result = cleanupFailure(std::move(*destinationParentsPending.try_error()));
            if (!stagingPublishStable)
            {
                append_secondary(result, std::move(*stagingPublishStable.try_error()), m_assertContext);
                result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
            }
            if (!sourcePublishStable)
            {
                append_secondary(result, std::move(*sourcePublishStable.try_error()), m_assertContext);
                result.outcome = cue::WorkspaceMutationOutcome::ReconciliationRequired;
            }
            return result;
        }
        const DWORD publishCode = rename_open_entry(stagingHandle.get(), destinationPinned.try_value()->back().get(),
                                                    *destinationName.try_value(), m_assertContext);
        cue::Result<void> stagingPublishStable =
            finish_directory_change_guard(**stagingPublishGuard.try_value(), m_assertContext);
        stagingPublishGuard.try_value()->reset();
        cue::Result<void> sourcePublishStable = finish_directory_change_guard(*sourcePublishGuard, m_assertContext);
        sourcePublishGuard.reset();
        cue::Result<void> sourceOplocksStable = finishSourceDirectoryOplocks();
        if (publishCode != ERROR_SUCCESS)
        {
            const NativeEntryObservation observed =
                observe_native_entry(*destinationPath.try_value(), stagingIdentity, true);
            if (observed.state == NativeEntryObservationState::QueryFailed)
            {
                cue::WorkspaceMutationResult result =
                    reconcilePublishFailure(publishCode, observed.nativeCode, "Workspace directory copy publish failed",
                                            "Workspace directory copy publish result could not be observed");
                if (!stagingPublishStable)
                {
                    append_secondary(result, std::move(*stagingPublishStable.try_error()), m_assertContext);
                }
                if (!sourcePublishStable)
                {
                    append_secondary(result, std::move(*sourcePublishStable.try_error()), m_assertContext);
                }
                if (!sourceOplocksStable)
                {
                    append_secondary(result, std::move(*sourceOplocksStable.try_error()), m_assertContext);
                }
                return result;
            }
            if (!stagingPublishStable || !sourcePublishStable || !sourceOplocksStable)
            {
                cue::WorkspaceMutationResult result = reconciliation_required(
                    make_windows_error(m_assertContext, publishCode, "Workspace directory copy publish failed"));
                if (!stagingPublishStable)
                {
                    append_secondary(result, std::move(*stagingPublishStable.try_error()), m_assertContext);
                }
                if (!sourcePublishStable)
                {
                    append_secondary(result, std::move(*sourcePublishStable.try_error()), m_assertContext);
                }
                if (!sourceOplocksStable)
                {
                    append_secondary(result, std::move(*sourceOplocksStable.try_error()), m_assertContext);
                }
                cue::Result<void> parentStable = finish_mutation_parent_guards(*destinationParentGuards.try_value());
                if (!parentStable)
                {
                    append_secondary(result, std::move(*parentStable.try_error()), m_assertContext);
                }
                return result;
            }
            if (observed.state != NativeEntryObservationState::Matches)
            {
                return cleanupConfirmedStagingAfterPublishFailure(publishCode,
                                                                  "Workspace directory copy publish failed");
            }
        }
        else if (!stagingPublishStable || !sourcePublishStable || !sourceOplocksStable)
        {
            cue::WorkspaceMutationResult result =
                reconciliation_required(!stagingPublishStable  ? std::move(*stagingPublishStable.try_error())
                                        : !sourcePublishStable ? std::move(*sourcePublishStable.try_error())
                                                               : std::move(*sourceOplocksStable.try_error()));
            if (!stagingPublishStable && !sourcePublishStable)
            {
                append_secondary(result, std::move(*sourcePublishStable.try_error()), m_assertContext);
            }
            if (!sourceOplocksStable && (!stagingPublishStable || !sourcePublishStable))
            {
                append_secondary(result, std::move(*sourceOplocksStable.try_error()), m_assertContext);
            }
            cue::Result<void> parentStable = finish_mutation_parent_guards(*destinationParentGuards.try_value());
            if (!parentStable)
            {
                append_secondary(result, std::move(*parentStable.try_error()), m_assertContext);
            }
            return result;
        }
        stagingChildRecords.clear();
    }

    if (!source.try_value()->isDirectory)
    {
        destinationAvailable = verify_portable_destination_absent(destinationPinned.try_value()->back().get(),
                                                                  *destinationName.try_value(), m_assertContext);
        if (!destinationAvailable)
        {
            return cleanupFailure(std::move(*destinationAvailable.try_error()));
        }
        cue::Result<void> destinationParentsPending =
            verify_mutation_parent_guards_pending(*destinationParentGuards.try_value());
        if (!destinationParentsPending)
        {
            return cleanupFailure(std::move(*destinationParentsPending.try_error()));
        }
        const DWORD publishCode = rename_open_entry(stagingHandle.get(), destinationPinned.try_value()->back().get(),
                                                    *destinationName.try_value(), m_assertContext);
        if (publishCode != ERROR_SUCCESS)
        {
            const NativeEntryObservation observed =
                observe_native_entry(*destinationPath.try_value(), stagingIdentity, false);
            if (observed.state == NativeEntryObservationState::QueryFailed)
            {
                return reconcilePublishFailure(publishCode, observed.nativeCode, "Workspace file copy publish failed",
                                               "Workspace file copy publish result could not be observed");
            }
            if (observed.state != NativeEntryObservationState::Matches)
            {
                return cleanupConfirmedStagingAfterPublishFailure(publishCode, "Workspace file copy publish failed");
            }
        }
    }

    cue::Result<void> destinationUnique =
        verify_portable_destination_unique(destinationPinned.try_value()->back().get(), *destinationName.try_value(),
                                           stagingIdentity, source.try_value()->isDirectory, m_assertContext);
    cue::Result<void> destinationChainStable =
        verify_mutation_parent_chain(a_destination, *destinationPinned.try_value());
    cue::Result<void> destinationParentsStable = finish_mutation_parent_guards(*destinationParentGuards.try_value());
    cue::Result<void> sourceChainStable = verify_mutation_parent_chain(a_source, *sourcePinned.try_value());
    if (!destinationUnique || !destinationChainStable || !destinationParentsStable || !sourceChainStable)
    {
        cue::Error error = !destinationUnique          ? std::move(*destinationUnique.try_error())
                           : !destinationChainStable   ? std::move(*destinationChainStable.try_error())
                           : !destinationParentsStable ? std::move(*destinationParentsStable.try_error())
                                                       : std::move(*sourceChainStable.try_error());
        return reconciliation_required(std::move(error));
    }

    cue::WorkspaceMutationResult result;
    result.outcome = cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
    result.primaryError = cue::make_io_error(m_assertContext, cue::IoError::DurabilityUnknown,
                                             "Workspace copy is visible but publish durability is unknown");
    return result;
}

cue::WorkspaceMutationResult WindowsWorkspaceFilesystem::remove_file_or_empty_directory(
    const cue::BoundWorkspacePath &a_entry) noexcept
{
    if (!owns_path(a_entry))
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::OutsideRoot,
                                                "Workspace cleanup path belongs to another root binding"));
    }
    cue::Result<std::vector<UniqueHandle>> pinned = prepare_mutation_parent(a_entry);
    if (!pinned)
    {
        return not_committed(std::move(*pinned.try_error()));
    }
    cue::Result<std::vector<std::unique_ptr<DirectoryChangeGuard>>> parentGuards =
        begin_mutation_parent_guards(*pinned.try_value());
    if (!parentGuards)
    {
        return not_committed(std::move(*parentGuards.try_error()));
    }
    const std::size_t separator = a_entry.text().rfind('/');
    const std::string_view leaf =
        separator == std::string_view::npos ? a_entry.text() : a_entry.text().substr(separator + 1U);
    cue::Result<std::wstring> nativeLeaf = to_utf16(leaf, m_assertContext);
    cue::Result<std::wstring> nativePath = make_native_workspace_path(m_rootPath, a_entry.text(), m_assertContext);
    if (!nativeLeaf || !nativePath)
    {
        return not_committed(!nativeLeaf ? std::move(*nativeLeaf.try_error()) : std::move(*nativePath.try_error()));
    }
    cue::Result<OpenedNativeEntry> opened =
        open_exact_entry(pinned.try_value()->back().get(), *nativeLeaf.try_value(),
                         DELETE | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ, m_assertContext);
    if (!opened)
    {
        return not_committed(std::move(*opened.try_error()));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(opened.try_value()->handle.get(), &information) == FALSE)
    {
        return not_committed(
            make_windows_error(m_assertContext, GetLastError(), "Workspace cleanup entry inspection failed"));
    }
    if (!opened.try_value()->isDirectory && information.nNumberOfLinks != 1U)
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::UnsupportedEntry,
                                                "Workspace cleanup file has multiple hard links"));
    }
    if (opened.try_value()->isDirectory)
    {
        cue::Result<NativeDirectoryEnumeration> contents =
            enumerate_directory_handle(opened.try_value()->handle.get(), 1U, 4096U, m_assertContext);
        if (!contents)
        {
            return not_committed(std::move(*contents.try_error()));
        }
        if (!contents.try_value()->entries.empty() || contents.try_value()->interruptedCode.has_value())
        {
            return not_committed(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                    "Workspace cleanup directory is not empty"));
        }
    }
    cue::Result<void> parentsPending = verify_mutation_parent_guards_pending(*parentGuards.try_value());
    if (!parentsPending)
    {
        return not_committed(std::move(*parentsPending.try_error()));
    }
    const RootIdentity expected = opened.try_value()->identity;
    const bool expectedDirectory = opened.try_value()->isDirectory;
    opened.try_value()->handle.reset();
    opened.try_value()->handle.reset(CreateFileW(
        nativePath.try_value()->c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (expectedDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0U), nullptr));
    if (!opened.try_value()->handle.is_valid())
    {
        return not_committed(
            make_windows_error(m_assertContext, GetLastError(), "Workspace cleanup entry reopen failed"));
    }
    cue::Result<RootIdentity> reopenedIdentity = read_entry_identity(opened.try_value()->handle.get(), m_assertContext);
    if (!reopenedIdentity)
    {
        return not_committed(std::move(*reopenedIdentity.try_error()));
    }
    BY_HANDLE_FILE_INFORMATION reopenedInformation{};
    if (GetFileInformationByHandle(opened.try_value()->handle.get(), &reopenedInformation) == FALSE)
    {
        const DWORD inspectionCode = GetLastError();
        return not_committed(
            make_windows_error(m_assertContext, inspectionCode, "Workspace cleanup reopened entry inspection failed"));
    }
    if (reopenedIdentity.try_value()->volumeSerial != expected.volumeSerial ||
        reopenedIdentity.try_value()->fileIndexHigh != expected.fileIndexHigh ||
        reopenedIdentity.try_value()->fileIndexLow != expected.fileIndexLow ||
        ((reopenedInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) != expectedDirectory ||
        (reopenedInformation.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (!expectedDirectory && reopenedInformation.nNumberOfLinks != 1U))
    {
        return not_committed(cue::make_io_error(m_assertContext, cue::IoError::PreconditionFailed,
                                                "Workspace cleanup entry changed during reopen"));
    }
    cue::Result<void> marked = mark_entry_for_deletion(opened.try_value()->handle, m_assertContext);
    if (!marked)
    {
        return not_committed(std::move(*marked.try_error()));
    }
    const NativeEntryObservation observed = observe_native_entry(*nativePath.try_value(), expected, expectedDirectory);
    cue::Result<void> parentsStable = finish_mutation_parent_guards(*parentGuards.try_value());
    if (observed.state != NativeEntryObservationState::Missing || !parentsStable)
    {
        return reconciliation_required(
            !parentsStable
                ? std::move(*parentsStable.try_error())
                : make_windows_error(m_assertContext,
                                     observed.state == NativeEntryObservationState::QueryFailed ? observed.nativeCode
                                                                                                : ERROR_INVALID_DATA,
                                     "Workspace cleanup result could not be confirmed"));
    }
    cue::WorkspaceMutationResult result;
    result.outcome = cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
    result.primaryError = cue::make_io_error(m_assertContext, cue::IoError::DurabilityUnknown,
                                             "Workspace cleanup is visible but durability is unknown");
    return result;
}
} // namespace

namespace cue
{
Result<std::unique_ptr<WorkspaceFilesystem>> create_windows_workspace_filesystem(
    std::string_view a_rootPath, const AssertContext &a_assertContext) noexcept
{
    Result<std::wstring> converted = to_utf16(a_rootPath, a_assertContext);
    if (!converted)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(std::move(*converted.try_error()));
    }
    const std::wstring_view input(*converted.try_value());
    if (input.find(L'\0') != std::wstring_view::npos)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_io_error(a_assertContext, IoError::InvalidPath, "Workspace root path contains NUL"));
    }
    const bool extendedPrefix = input.starts_with(L"\\\\?\\");
    if (is_unc_path(input))
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_io_error(a_assertContext, IoError::UnsupportedEntry, "UNC workspace roots are not supported"));
    }
    const std::size_t driveOffset = extendedPrefix ? 4U : 0U;
    const bool driveAbsolute = input.size() >= driveOffset + 3U && std::iswalpha(input[driveOffset]) != 0 &&
                               input[driveOffset + 1U] == L':' &&
                               (input[driveOffset + 2U] == L'\\' || input[driveOffset + 2U] == L'/');
    if (!driveAbsolute)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_io_error(a_assertContext, IoError::InvalidPath, "Workspace root path must be local-drive absolute"));
    }
    const std::array<wchar_t, 4U> driveRoot{input[driveOffset], L':', L'\\', L'\0'};
    if (GetDriveTypeW(driveRoot.data()) == DRIVE_REMOTE)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(make_io_error(
            a_assertContext, IoError::UnsupportedEntry, "Remote drive workspace roots are not supported"));
    }

    const DWORD required = GetFullPathNameW(converted.try_value()->c_str(), 0U, nullptr, nullptr);
    if (required == 0U)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace root absolute path resolution failed"));
    }
    std::wstring absolute;
    try
    {
        absolute.resize(required);
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    const DWORD written = GetFullPathNameW(converted.try_value()->c_str(), required, absolute.data(), nullptr);
    if (written == 0U || written >= required)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace root absolute path resolution failed"));
    }
    absolute.resize(written);
    const std::size_t rootLength = local_drive_root_length(absolute);
    while (absolute.size() > rootLength && (absolute.back() == L'\\' || absolute.back() == L'/'))
    {
        absolute.pop_back();
    }

    Result<std::wstring> extended = make_extended_path(std::move(absolute), a_assertContext);
    if (!extended)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(std::move(*extended.try_error()));
    }
    std::wstring requestedRootPath;
    try
    {
        requestedRootPath = *extended.try_value();
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    UniqueHandle root(CreateFileW(extended.try_value()->c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!root.is_valid())
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace root open failed"));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(root.get(), &information) == FALSE)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace root inspection failed"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_io_error(a_assertContext, IoError::TypeMismatch, "Workspace root is not a directory"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_io_error(a_assertContext, IoError::UnsupportedEntry, "Workspace root is a reparse point"));
    }

    std::array<wchar_t, MAX_PATH + 1U> fileSystemName{};
    DWORD fileSystemFlags = 0U;
    if (GetVolumeInformationByHandleW(root.get(), nullptr, 0U, nullptr, nullptr, &fileSystemFlags,
                                      fileSystemName.data(), static_cast<DWORD>(fileSystemName.size())) == FALSE)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace volume capability query failed"));
    }
    if (CompareStringOrdinal(fileSystemName.data(), -1, L"NTFS", -1, TRUE) != CSTR_EQUAL)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_io_error(a_assertContext, IoError::UnsupportedEntry, "Workspace volume filesystem is not supported"));
    }
    if ((fileSystemFlags & FILE_SUPPORTS_OPEN_BY_FILE_ID) == 0U)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_io_error(a_assertContext, IoError::UnsupportedEntry,
                          "Workspace volume does not support opening entries by file id"));
    }

    const DWORD finalRequired =
        GetFinalPathNameByHandleW(root.get(), nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (finalRequired == 0U)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace root final path query failed"));
    }
    std::wstring finalPath;
    try
    {
        finalPath.resize(finalRequired);
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    const DWORD finalWritten =
        GetFinalPathNameByHandleW(root.get(), finalPath.data(), finalRequired, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (finalWritten == 0U || finalWritten >= finalRequired)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_windows_error(a_assertContext, GetLastError(), "Workspace root final path query failed"));
    }
    finalPath.resize(finalWritten);
    if (is_unc_path(finalPath))
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(make_io_error(
            a_assertContext, IoError::UnsupportedEntry, "Resolved workspace root is on a remote filesystem"));
    }
    const std::size_t finalDriveOffset = finalPath.starts_with(L"\\\\?\\") ? 4U : 0U;
    if (finalPath.size() < finalDriveOffset + 3U || std::iswalpha(finalPath[finalDriveOffset]) == 0 ||
        finalPath[finalDriveOffset + 1U] != L':')
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(make_io_error(
            a_assertContext, IoError::UnsupportedEntry, "Resolved workspace root is not on a local drive"));
    }
    const std::array<wchar_t, 4U> finalDriveRoot{finalPath[finalDriveOffset], L':', L'\\', L'\0'};
    if (GetDriveTypeW(finalDriveRoot.data()) == DRIVE_REMOTE)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(
            make_io_error(a_assertContext, IoError::UnsupportedEntry, "Resolved workspace root is on a remote drive"));
    }

    constexpr std::size_t k_directoryPatternCharacters = 4U;
    if (finalPath.size() > k_maxWindowsPathLength - k_directoryPatternCharacters)
    {
        return Result<std::unique_ptr<WorkspaceFilesystem>>::failure(make_io_error(
            a_assertContext, IoError::CapacityExceeded, "Workspace root leaves no capacity for directory paths"));
    }
    const std::size_t maxBoundPathCharacters = k_maxWindowsPathLength - finalPath.size() - k_directoryPatternCharacters;

    const RootIdentity identity{information.dwVolumeSerialNumber, information.nFileIndexHigh,
                                information.nFileIndexLow};
    try
    {
        std::unique_ptr<WorkspaceFilesystem> filesystem = std::make_unique<WindowsWorkspaceFilesystem>(
            a_assertContext, std::move(requestedRootPath), std::move(finalPath), std::move(root), identity,
            maxBoundPathCharacters);
        return Result<std::unique_ptr<WorkspaceFilesystem>>::success(std::move(filesystem));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}
} // namespace cue
