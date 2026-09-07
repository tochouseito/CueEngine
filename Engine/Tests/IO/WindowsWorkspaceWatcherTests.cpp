#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Windows/WindowsWorkspaceFilesystem.h>
#include <Cue/IO/WorkspaceWatcher.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の予期しないFatalをProcess失敗にする
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }
    /// @brief Test中の予期しない詳細付きFatalをProcess失敗にする
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

/// @brief UTF-16 PathをWorkspace Factory用UTF-8へ変換する
[[nodiscard]] std::string to_utf8(std::wstring_view a_path)
{
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_path.data(), static_cast<int>(a_path.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_path.data(), static_cast<int>(a_path.size()),
                            result.data(), count, nullptr, nullptr) != count)
    {
        return {};
    }
    return result;
}

/// @brief File ContentをCreateまたは置換して変更通知を発生させる
[[nodiscard]] bool write_file(std::wstring_view a_path, std::string_view a_content) noexcept
{
    const std::wstring path(a_path);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0U;
    const BOOL succeeded = WriteFile(file, a_content.data(), static_cast<DWORD>(a_content.size()), &written, nullptr);
    const BOOL flushed = FlushFileBuffers(file);
    CloseHandle(file);
    return succeeded != FALSE && flushed != FALSE && written == static_cast<DWORD>(a_content.size());
}

class TestDirectory final
{
  public:
    /// @brief File Watch統合Test用の一意なWorkspaceとSeed Entryを生成する
    TestDirectory()
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0U || length >= temporary.size())
        {
            return;
        }
        static std::atomic_uint64_t sequence{0U};
        m_root = temporary.data();
        m_root += L"CueWorkspaceWatcherTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64()) + L"-" +
                  std::to_wstring(sequence.fetch_add(1U, std::memory_order_relaxed));
        m_outside = m_root + L"-outside";
        m_created =
            CreateDirectoryW(m_root.c_str(), nullptr) != FALSE &&
            CreateDirectoryW(m_outside.c_str(), nullptr) != FALSE &&
            CreateDirectoryW(child(L"Watch").c_str(), nullptr) != FALSE &&
            CreateDirectoryW(child(L"Movable").c_str(), nullptr) != FALSE &&
            write_file(child(L"Watch\\Modify.txt"), "before") && write_file(child(L"Watch\\RenameOld.txt"), "rename") &&
            write_file(child(L"Watch\\RenameDelete.txt"), "rename-delete") &&
            write_file(child(L"Watch\\Delete.txt"), "delete") && write_file(child(L"Watch\\Burst.txt"), "burst");
    }
    /// @brief Test Directoryの複製を禁止する
    TestDirectory(const TestDirectory &) = delete;
    /// @brief Test DirectoryのCopy代入を禁止する
    TestDirectory &operator=(const TestDirectory &) = delete;
    /// @brief Test終了時にWorkspace全体を削除する
    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_root, error);
        error.clear();
        std::filesystem::remove_all(m_outside, error);
    }

    /// @brief Fixture生成が完了したか返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_created;
    }
    /// @brief Workspace RootのUTF-8 Pathを返す
    [[nodiscard]] std::string root_utf8() const
    {
        return to_utf8(m_root);
    }
    /// @brief Root配下のNative Pathを返す
    [[nodiscard]] std::wstring child(std::wstring_view a_relative) const
    {
        return m_root + L"\\" + std::wstring(a_relative);
    }

    /// @brief Workspace Root外に用意したSibling Directory配下のPathを返す
    [[nodiscard]] std::wstring outside_child(std::wstring_view a_relative) const
    {
        return m_outside + L"\\" + std::wstring(a_relative);
    }

  private:
    std::wstring m_root;
    std::wstring m_outside;
    bool m_created = false;
};

/// @brief Debounce確定済みBatchが得られるまで上限付きで待つ
[[nodiscard]] std::optional<cue::WorkspaceChangeBatch> wait_for_batch(cue::WorkspaceWatcher &a_watcher) noexcept
{
    for (std::size_t attempt = 0U; attempt < 300U; ++attempt)
    {
        cue::Result<std::optional<cue::WorkspaceChangeBatch>> drained = a_watcher.drain_changes();
        if (!drained)
        {
            return std::nullopt;
        }
        if (drained.try_value()->has_value())
        {
            return std::move(**drained.try_value());
        }
        Sleep(10U);
    }
    return std::nullopt;
}

/// @brief Batchが指定Pathと種別のHintを保持するか判定する
[[nodiscard]] bool has_change(const cue::WorkspaceChangeBatch &a_batch, cue::WorkspaceChangeHintKind a_kind,
                              std::string_view a_path, std::string_view a_previous = {}) noexcept
{
    for (const cue::WorkspaceChangeHint &change : a_batch.changes)
    {
        if (change.kind != a_kind || change.locator.text() != a_path)
        {
            continue;
        }
        if (a_kind != cue::WorkspaceChangeHintKind::Renamed)
        {
            return !change.previousLocator.has_value();
        }
        return change.previousLocator.has_value() && change.previousLocator->text() == a_previous;
    }
    return false;
}

/// @brief Create、Modify、Rename、Delete、Coalesce、Root境界を検証する
[[nodiscard]] bool test_change_batches(cue::WorkspaceFilesystem &a_workspace, const TestDirectory &a_directory,
                                       const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::RelativePath> locator = cue::RelativePath::parse("Watch", a_assertContext);
    if (!locator)
    {
        return false;
    }
    cue::Result<cue::WorkspaceDirectory> directory =
        a_workspace.bind_directory(std::move(*locator.try_value()), a_assertContext);
    if (!directory)
    {
        return false;
    }
    constexpr cue::WorkspaceWatchLimits k_limits{256U, 64U * 1024U, 128U, 25U, 250U};
    cue::Result<std::unique_ptr<cue::WorkspaceWatcher>> watcher =
        a_workspace.create_watcher(*directory.try_value(), k_limits);
    if (!watcher || !(*watcher.try_value())->is_running())
    {
        return false;
    }

    if (!write_file(a_directory.child(L"Watch\\Created.txt"), "created"))
    {
        return false;
    }
    std::optional<cue::WorkspaceChangeBatch> created = wait_for_batch(**watcher.try_value());
    if (!created || created->state != cue::WorkspaceChangeBatchState::ChangesAvailable ||
        !has_change(*created, cue::WorkspaceChangeHintKind::Created, "Created.txt"))
    {
        return false;
    }

    if (!write_file(a_directory.child(L"Watch\\Modify.txt"), "after"))
    {
        return false;
    }
    std::optional<cue::WorkspaceChangeBatch> modified = wait_for_batch(**watcher.try_value());
    if (!modified || modified->state != cue::WorkspaceChangeBatchState::ChangesAvailable ||
        !has_change(*modified, cue::WorkspaceChangeHintKind::Modified, "Modify.txt"))
    {
        return false;
    }

    if (MoveFileExW(a_directory.child(L"Watch\\RenameOld.txt").c_str(),
                    a_directory.child(L"Watch\\RenameNew.txt").c_str(), 0U) == FALSE)
    {
        return false;
    }
    std::optional<cue::WorkspaceChangeBatch> renamed = wait_for_batch(**watcher.try_value());
    if (!renamed || renamed->state != cue::WorkspaceChangeBatchState::ChangesAvailable ||
        !has_change(*renamed, cue::WorkspaceChangeHintKind::Renamed, "RenameNew.txt", "RenameOld.txt"))
    {
        return false;
    }

    if (DeleteFileW(a_directory.child(L"Watch\\Delete.txt").c_str()) == FALSE)
    {
        return false;
    }
    std::optional<cue::WorkspaceChangeBatch> removed = wait_for_batch(**watcher.try_value());
    if (!removed || removed->state != cue::WorkspaceChangeBatchState::ChangesAvailable ||
        !has_change(*removed, cue::WorkspaceChangeHintKind::Removed, "Delete.txt"))
    {
        return false;
    }

    if (MoveFileExW(a_directory.child(L"Watch\\RenameDelete.txt").c_str(),
                    a_directory.child(L"Watch\\RenamedThenDeleted.txt").c_str(), 0U) == FALSE ||
        DeleteFileW(a_directory.child(L"Watch\\RenamedThenDeleted.txt").c_str()) == FALSE)
    {
        return false;
    }
    std::optional<cue::WorkspaceChangeBatch> renameDeleted = wait_for_batch(**watcher.try_value());
    if (!renameDeleted || renameDeleted->state != cue::WorkspaceChangeBatchState::RescanRequired ||
        renameDeleted->diagnostics.empty() ||
        renameDeleted->diagnostics.front().code != cue::WorkspaceWatchDiagnosticCode::ChangeSequenceConflict)
    {
        return false;
    }

    for (std::size_t index = 0U; index < 12U; ++index)
    {
        if (!write_file(a_directory.child(L"Watch\\Burst.txt"), std::to_string(index)))
        {
            return false;
        }
    }
    std::optional<cue::WorkspaceChangeBatch> burst = wait_for_batch(**watcher.try_value());
    if (!burst || burst->state != cue::WorkspaceChangeBatchState::ChangesAvailable)
    {
        return false;
    }
    std::size_t burstCount = 0U;
    for (const cue::WorkspaceChangeHint &change : burst->changes)
    {
        if (change.locator.text() == "Burst.txt")
        {
            ++burstCount;
        }
    }
    if (burstCount != 1U || !write_file(a_directory.child(L"Outside.txt"), "outside"))
    {
        return false;
    }
    Sleep(300U);
    cue::Result<std::optional<cue::WorkspaceChangeBatch>> outside = (*watcher.try_value())->drain_changes();
    if (!outside || outside.try_value()->has_value())
    {
        return false;
    }
    cue::Result<void> stopped = (*watcher.try_value())->stop();
    cue::Result<void> stoppedAgain = (*watcher.try_value())->stop();
    return stopped && stoppedAgain && !(*watcher.try_value())->is_running();
}

/// @brief 監視Directory自体のRoot外移動をLifetime中のLocation Lockが拒否するか検証する
[[nodiscard]] bool test_watched_directory_move(cue::WorkspaceFilesystem &a_workspace, const TestDirectory &a_directory,
                                               const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::RelativePath> locator = cue::RelativePath::parse("Movable", a_assertContext);
    if (!locator)
    {
        return false;
    }
    cue::Result<cue::WorkspaceDirectory> directory =
        a_workspace.bind_directory(std::move(*locator.try_value()), a_assertContext);
    if (!directory)
    {
        return false;
    }
    constexpr cue::WorkspaceWatchLimits k_limits{64U, 16U * 1024U, 32U, 25U, 250U};
    cue::Result<std::unique_ptr<cue::WorkspaceWatcher>> watcher =
        a_workspace.create_watcher(*directory.try_value(), k_limits);
    if (!watcher)
    {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    if (MoveFileExW(a_directory.child(L"Movable").c_str(), a_directory.outside_child(L"Escaped").c_str(), 0U) !=
            FALSE ||
        GetLastError() != ERROR_SHARING_VIOLATION || !write_file(a_directory.child(L"Movable\\Inside.txt"), "inside"))
    {
        return false;
    }
    std::optional<cue::WorkspaceChangeBatch> batch = wait_for_batch(**watcher.try_value());
    if (!batch || batch->state != cue::WorkspaceChangeBatchState::ChangesAvailable ||
        !has_change(*batch, cue::WorkspaceChangeHintKind::Created, "Inside.txt") ||
        !(*watcher.try_value())->is_running() || !(*watcher.try_value())->stop())
    {
        return false;
    }
    return MoveFileExW(a_directory.child(L"Movable").c_str(), a_directory.outside_child(L"Escaped").c_str(), 0U) !=
           FALSE;
}

/// @brief Bounded Queue OverflowをRescanRequiredへ昇格し停止後通知を残さないか検証する
[[nodiscard]] bool test_overflow_and_shutdown(cue::WorkspaceFilesystem &a_workspace, const TestDirectory &a_directory,
                                              const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::RelativePath> locator = cue::RelativePath::parse("Watch", a_assertContext);
    if (!locator)
    {
        return false;
    }
    cue::Result<cue::WorkspaceDirectory> directory =
        a_workspace.bind_directory(std::move(*locator.try_value()), a_assertContext);
    if (!directory)
    {
        return false;
    }
    constexpr cue::WorkspaceWatchLimits k_limits{1U, 128U, 8U, 25U, 250U};
    cue::Result<std::unique_ptr<cue::WorkspaceWatcher>> watcher =
        a_workspace.create_watcher(*directory.try_value(), k_limits);
    if (!watcher)
    {
        return false;
    }
    for (std::size_t index = 0U; index < 8U; ++index)
    {
        const std::wstring name = L"Watch\\Overflow" + std::to_wstring(index) + L".txt";
        if (!write_file(a_directory.child(name), "overflow"))
        {
            return false;
        }
    }
    std::optional<cue::WorkspaceChangeBatch> overflow = wait_for_batch(**watcher.try_value());
    if (!overflow || overflow->state != cue::WorkspaceChangeBatchState::RescanRequired || overflow->diagnostics.empty())
    {
        return false;
    }
    if (!(*watcher.try_value())->stop() || !write_file(a_directory.child(L"Watch\\AfterStop.txt"), "stopped"))
    {
        return false;
    }
    Sleep(50U);
    cue::Result<std::optional<cue::WorkspaceChangeBatch>> afterStop = (*watcher.try_value())->drain_changes();
    return afterStop && !afterStop.try_value()->has_value() && !(*watcher.try_value())->is_running();
}
} // namespace

/// @brief Windows File Watcherの変更Batch、Overflow、停止Contractを実行する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    const cue::AssertContext assertContext(logger, fatalHandler);
    TestDirectory directory;
    if (!directory.is_created())
    {
        return 1;
    }
    cue::Result<std::unique_ptr<cue::WorkspaceFilesystem>> workspace =
        cue::create_windows_workspace_filesystem(directory.root_utf8(), assertContext);
    if (!workspace)
    {
        return 2;
    }
    if (!test_change_batches(**workspace.try_value(), directory, assertContext))
    {
        return 3;
    }
    if (!test_watched_directory_move(**workspace.try_value(), directory, assertContext))
    {
        return 4;
    }
    return test_overflow_and_shutdown(**workspace.try_value(), directory, assertContext) ? 0 : 5;
}
