#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/WorkspaceFilesystem.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
/// @brief Test中の回復不能失敗を終了Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalをTest終了Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief Message付きFatalをTest終了Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

/// @brief Resultが指定Portable IO分類を保持するか判定する
template <typename T> [[nodiscard]] bool has_io_error(cue::Result<T> &a_result, cue::IoError a_code) noexcept
{
    const cue::Error *error = a_result.try_error();
    return error != nullptr && error->code().domain() == "Cue.IO" &&
           error->code().value() == static_cast<std::int64_t>(a_code);
}

/// @brief Test用の操作可能Entryを構築する
[[nodiscard]] cue::WorkspaceEntry make_entry(cue::WorkspaceFilesystem &a_filesystem, std::string_view a_name,
                                             std::string_view a_locator, cue::WorkspaceEntryType a_type,
                                             std::uint64_t a_generation, const cue::AssertContext &a_assertContext)
{
    auto locator = cue::RelativePath::parse(a_locator, a_assertContext);
    auto name = cue::RelativePath::parse(a_name, a_assertContext);
    if (!locator || !name)
    {
        a_assertContext.fatal_handler().terminate("Workspace test locator parse failed");
    }
    cue::WorkspaceEntry entry;
    entry.parentGeneration = a_generation;
    entry.displayName = a_name;
    entry.sortKey = name.try_value()->comparison_key(a_assertContext);
    auto directory = a_filesystem.bind_directory(std::move(*locator.try_value()), a_assertContext);
    if (!directory || directory.try_value()->locator() == nullptr)
    {
        a_assertContext.fatal_handler().terminate("Workspace test binding failed");
    }
    entry.locator = *directory.try_value()->locator();
    entry.type = a_type;
    entry.byteSize = a_type == cue::WorkspaceEntryType::RegularFile ? 4U : 0U;
    return entry;
}

/// @brief Portable SearchをNative APIなしで検証する決定的Test Double
class FakeWorkspaceFilesystem final : public cue::WorkspaceFilesystem
{
  public:
    /// @brief Test Data生成に必要なAssert Contextを保持する
    explicit FakeWorkspaceFilesystem(const cue::AssertContext &a_assertContext,
                                     std::size_t a_maxBoundPathCharacters = 4096U) noexcept
        : cue::WorkspaceFilesystem(a_maxBoundPathCharacters), m_assertContext(&a_assertContext)
    {
    }
    FakeWorkspaceFilesystem(const FakeWorkspaceFilesystem &) = delete;
    FakeWorkspaceFilesystem &operator=(const FakeWorkspaceFilesystem &) = delete;
    FakeWorkspaceFilesystem(FakeWorkspaceFilesystem &&) = delete;
    FakeWorkspaceFilesystem &operator=(FakeWorkspaceFilesystem &&) = delete;
    ~FakeWorkspaceFilesystem() override = default;

    /// @brief Test Doubleの固定Hard Limitを返す
    [[nodiscard]] cue::TraversalLimits hard_limits() const noexcept override
    {
        return cue::TraversalLimits{8U, 100U, 100U, 16U * 1024U};
    }

    /// @brief Test Doubleの固定Content Hard Limitを返す
    [[nodiscard]] cue::ContentVerificationLimits hard_content_limits() const noexcept override
    {
        return cue::ContentVerificationLimits{1024U, 4096U};
    }

    /// @brief Portable Test DoubleではAbsolute Path取込みを未対応として返す
    [[nodiscard]] cue::Result<cue::BoundWorkspacePath> bind_external_path(std::string_view) noexcept override
    {
        return cue::Result<cue::BoundWorkspacePath>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                               "Workspace core test double does not support external path binding"));
    }

    /// @brief Locatorに対応する決定的Snapshotまたは診断Snapshotを返す
    [[nodiscard]] cue::Result<cue::DirectorySnapshot> list_directory(const cue::WorkspaceDirectory &a_directory,
                                                                     cue::TraversalLimits) noexcept override
    {
        if (!owns_directory(a_directory))
        {
            return cue::Result<cue::DirectorySnapshot>::failure(cue::make_io_error(
                *m_assertContext, cue::IoError::OutsideRoot, "Workspace directory belongs to another root binding"));
        }
        cue::DirectorySnapshot snapshot;
        snapshot.generation = m_nextGeneration++;
        const cue::BoundWorkspacePath *locator = a_directory.locator();
        const std::string_view text = locator == nullptr ? std::string_view{} : locator->text();
        if (text.empty())
        {
            snapshot.entries.push_back(make_entry(*this, "Beta.bin", "Beta.bin", cue::WorkspaceEntryType::RegularFile,
                                                  snapshot.generation, *m_assertContext));
            snapshot.entries.push_back(make_entry(*this, "Folder", "Folder", cue::WorkspaceEntryType::Directory,
                                                  snapshot.generation, *m_assertContext));
        }
        else if (text == "Folder")
        {
            snapshot.entries.push_back(make_entry(*this, "Nested.txt", "Folder/Nested.txt",
                                                  cue::WorkspaceEntryType::RegularFile, snapshot.generation,
                                                  *m_assertContext));
            snapshot.entries.push_back(make_entry(*this, "Shared.txt", "Folder/Shared.txt",
                                                  cue::WorkspaceEntryType::RegularFile, snapshot.generation,
                                                  *m_assertContext));
            snapshot.entries.push_back(make_entry(*this, "Deep", "Folder/Deep", cue::WorkspaceEntryType::Directory,
                                                  snapshot.generation, *m_assertContext));
        }
        else if (text == "Folder/Deep")
        {
            snapshot.entries.push_back(make_entry(*this, "End.txt", "Folder/Deep/End.txt",
                                                  cue::WorkspaceEntryType::RegularFile, snapshot.generation,
                                                  *m_assertContext));
            snapshot.entries.push_back(make_entry(*this, "Shared.txt", "Folder/Deep/Shared.txt",
                                                  cue::WorkspaceEntryType::RegularFile, snapshot.generation,
                                                  *m_assertContext));
        }
        else if (text == "Diagnostics")
        {
            cue::WorkspaceEntry unsupported;
            unsupported.parentGeneration = snapshot.generation;
            unsupported.displayName = "Vanished.bin";
            unsupported.sortKey = "Vanished.bin";
            unsupported.type = cue::WorkspaceEntryType::UnsupportedEntry;
            unsupported.rejection = cue::WorkspaceDiagnosticCode::EntryDisappeared;
            snapshot.entries.push_back(std::move(unsupported));
            snapshot.state = cue::WorkspaceSnapshotState::RescanRequired;
            snapshot.diagnostics.push_back(cue::WorkspaceDiagnostic{
                snapshot.generation, cue::WorkspaceDiagnosticCode::EntryDisappeared, "Vanished.bin", 2});
        }
        cue::sort_workspace_entries(snapshot.entries);
        return cue::Result<cue::DirectorySnapshot>::success(std::move(snapshot));
    }

    /// @brief Test Doubleが所有するDirectory Capabilityを実在扱いで検証する
    [[nodiscard]] cue::Result<void> verify_directory(const cue::WorkspaceDirectory &a_directory) noexcept override
    {
        if (!owns_directory(a_directory))
        {
            return cue::Result<void>::failure(cue::make_io_error(
                *m_assertContext, cue::IoError::OutsideRoot, "Workspace core test directory belongs to another root"));
        }
        return cue::Result<void>::success();
    }

    /// @brief この列挙専用Test DoubleではFile読取りを未対応として返す
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file_bounded(const cue::BoundWorkspacePath &,
                                                                        std::size_t) noexcept override
    {
        return cue::Result<std::vector<std::byte>>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                               "Workspace core test double does not support file reading"));
    }

    /// @brief この列挙専用Test DoubleではFingerprintを未対応として返す
    [[nodiscard]] cue::Result<cue::WorkspaceEntryFingerprint> fingerprint_entry(
        const cue::BoundWorkspacePath &, cue::TraversalLimits, cue::ContentVerificationLimits) noexcept override
    {
        return cue::Result<cue::WorkspaceEntryFingerprint>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                               "Workspace core test double does not support fingerprinting"));
    }

    /// @brief この列挙専用Test DoubleではMutationを未対応として返す
    [[nodiscard]] cue::WorkspaceMutationResult create_directory_new(const cue::BoundWorkspacePath &,
                                                                    std::string_view) noexcept override
    {
        cue::WorkspaceMutationResult result;
        result.outcome = cue::WorkspaceMutationOutcome::NotCommitted;
        result.primaryError = cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                                                 "Workspace core test double does not support mutation");
        return result;
    }

    /// @brief この列挙専用Test DoubleではMutationを未対応として返す
    [[nodiscard]] cue::WorkspaceMutationResult create_file_new_atomic(const cue::BoundWorkspacePath &,
                                                                      std::span<const std::byte>,
                                                                      std::string_view) noexcept override
    {
        cue::WorkspaceMutationResult result;
        result.outcome = cue::WorkspaceMutationOutcome::NotCommitted;
        result.primaryError = cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                                                 "Workspace core test double does not support mutation");
        return result;
    }

    /// @brief この列挙専用Test Doubleでは置換を未対応として返す
    [[nodiscard]] cue::WorkspaceMutationResult replace_file_atomic(const cue::BoundWorkspacePath &,
                                                                   std::span<const std::byte>,
                                                                   std::string_view) noexcept override
    {
        cue::WorkspaceMutationResult result;
        result.outcome = cue::WorkspaceMutationOutcome::NotCommitted;
        result.primaryError = cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                                                 "Workspace core test double does not support replacement");
        return result;
    }

    /// @brief この列挙専用Test DoubleではRenameを未対応として返す
    [[nodiscard]] cue::WorkspaceMutationResult rename_entry(const cue::BoundWorkspacePath &,
                                                            const cue::BoundWorkspacePath &,
                                                            cue::TraversalLimits) noexcept override
    {
        cue::WorkspaceMutationResult result;
        result.outcome = cue::WorkspaceMutationOutcome::NotCommitted;
        result.primaryError = cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                                                 "Workspace core test double does not support rename");
        return result;
    }

    /// @brief この列挙専用Test DoubleではMutation Guard取得を未対応として返す
    [[nodiscard]] cue::Result<cue::GuardedWorkspaceEntry> guard_entry(const cue::BoundWorkspacePath &,
                                                                      cue::TraversalLimits,
                                                                      cue::ContentVerificationLimits) noexcept override
    {
        return cue::Result<cue::GuardedWorkspaceEntry>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::UnsupportedEntry, "Workspace core test double does not support guards"));
    }

    /// @brief この列挙専用Test Doubleでは条件付きMutation Guard取得を未対応として返す
    [[nodiscard]] cue::Result<std::unique_ptr<cue::WorkspaceEntryMutationGuard>> guard_entry_if_matches(
        const cue::BoundWorkspacePath &, const cue::WorkspaceEntryFingerprint &, cue::TraversalLimits,
        cue::ContentVerificationLimits) noexcept override
    {
        return cue::Result<std::unique_ptr<cue::WorkspaceEntryMutationGuard>>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::UnsupportedEntry, "Workspace core test double does not support guards"));
    }

    /// @brief この列挙専用Test DoubleではGuard付きRenameを未対応として返す
    [[nodiscard]] cue::WorkspaceMutationResult rename_guarded_entry(cue::WorkspaceEntryMutationGuard &,
                                                                    const cue::BoundWorkspacePath &,
                                                                    const cue::BoundWorkspacePath &) noexcept override
    {
        cue::WorkspaceMutationResult result;
        result.outcome = cue::WorkspaceMutationOutcome::NotCommitted;
        result.primaryError = cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                                                 "Workspace core test double does not support guarded rename");
        return result;
    }

    /// @brief この列挙専用Test DoubleではMutation Guard完了を未対応として返す
    [[nodiscard]] cue::Result<void> finish_entry_mutation_guard(
        std::unique_ptr<cue::WorkspaceEntryMutationGuard>) noexcept override
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                                                             "Workspace core test double does not support guards"));
    }

    /// @brief この列挙専用Test DoubleではCopyを未対応として返す
    [[nodiscard]] cue::WorkspaceMutationResult copy_entry_new(const cue::BoundWorkspacePath &,
                                                              const cue::BoundWorkspacePath &, cue::TraversalLimits,
                                                              cue::ContentVerificationLimits,
                                                              std::string_view) noexcept override
    {
        cue::WorkspaceMutationResult result;
        result.outcome = cue::WorkspaceMutationOutcome::NotCommitted;
        result.primaryError = cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                                                 "Workspace core test double does not support copy");
        return result;
    }

    /// @brief この列挙専用Test Doubleでは削除を未対応として返す
    [[nodiscard]] cue::WorkspaceMutationResult remove_file_or_empty_directory(
        const cue::BoundWorkspacePath &) noexcept override
    {
        cue::WorkspaceMutationResult result;
        result.outcome = cue::WorkspaceMutationOutcome::NotCommitted;
        result.primaryError = cue::make_io_error(*m_assertContext, cue::IoError::UnsupportedEntry,
                                                 "Workspace core test double does not support removal");
        return result;
    }

  private:
    const cue::AssertContext *m_assertContext;
    std::uint64_t m_nextGeneration = 1U;
};

/// @brief Portable Sort、Filter、操作可否を検証する
[[nodiscard]] bool test_sort_and_filter(const cue::AssertContext &a_assertContext)
{
    FakeWorkspaceFilesystem filesystem(a_assertContext);
    cue::DirectorySnapshot snapshot;
    snapshot.generation = 7U;
    snapshot.entries.push_back(
        make_entry(filesystem, "zeta.bin", "zeta.bin", cue::WorkspaceEntryType::RegularFile, 7U, a_assertContext));
    snapshot.entries.push_back(
        make_entry(filesystem, "FolderB", "FolderB", cue::WorkspaceEntryType::Directory, 7U, a_assertContext));
    cue::WorkspaceEntry unsupported;
    unsupported.parentGeneration = 7U;
    unsupported.displayName = ".Hidden";
    unsupported.sortKey = ".Hidden";
    unsupported.type = cue::WorkspaceEntryType::UnsupportedEntry;
    unsupported.rejection = cue::WorkspaceDiagnosticCode::UnsupportedName;
    snapshot.entries.push_back(std::move(unsupported));
    snapshot.entries.push_back(
        make_entry(filesystem, "folderA", "folderA", cue::WorkspaceEntryType::Directory, 7U, a_assertContext));

    cue::sort_workspace_entries(snapshot.entries);
    auto filtered = cue::filter_workspace_snapshot(snapshot, "FOLDER", 2U, a_assertContext);
    auto limited = cue::filter_workspace_snapshot(snapshot, "folder", 1U, a_assertContext);
    auto zero = cue::filter_workspace_snapshot(snapshot, {}, 0U, a_assertContext);
    return snapshot.entries.size() == 4U && snapshot.entries[0].displayName == "folderA" &&
           snapshot.entries[1].displayName == "FolderB" && snapshot.entries[2].displayName == "zeta.bin" &&
           snapshot.entries[3].displayName == ".Hidden" && snapshot.entries[0].is_operable() &&
           !snapshot.entries[3].is_operable() && filtered && filtered.try_value()->size() == 2U &&
           (*filtered.try_value())[0].displayName == "folderA" &&
           has_io_error(limited, cue::IoError::CapacityExceeded) && has_io_error(zero, cue::IoError::CapacityExceeded);
}

/// @brief Bounded Searchの順序、再帰、全上限を検証する
[[nodiscard]] bool test_bounded_search(const cue::AssertContext &a_assertContext)
{
    FakeWorkspaceFilesystem filesystem(a_assertContext);
    const cue::TraversalLimits generous{4U, 16U, 16U, 4096U};
    auto found = cue::search_workspace(filesystem, cue::WorkspaceDirectory::root(), ".txt", generous, a_assertContext);
    if (!found || found.try_value()->entries.size() != 4U || found.try_value()->visitedEntries != 7U ||
        found.try_value()->entries[0].displayName != "End.txt" ||
        found.try_value()->entries[1].displayName != "Nested.txt" ||
        found.try_value()->entries[2].locator->text() != "Folder/Deep/Shared.txt" ||
        found.try_value()->entries[3].locator->text() != "Folder/Shared.txt")
    {
        return false;
    }

    auto zero = cue::search_workspace(filesystem, cue::WorkspaceDirectory::root(), {}, {}, a_assertContext);
    auto depth = cue::search_workspace(filesystem, cue::WorkspaceDirectory::root(), {},
                                       cue::TraversalLimits{1U, 16U, 16U, 4096U}, a_assertContext);
    auto visited = cue::search_workspace(filesystem, cue::WorkspaceDirectory::root(), {},
                                         cue::TraversalLimits{4U, 1U, 16U, 4096U}, a_assertContext);
    auto results = cue::search_workspace(filesystem, cue::WorkspaceDirectory::root(), {},
                                         cue::TraversalLimits{4U, 16U, 1U, 4096U}, a_assertContext);
    auto metadata = cue::search_workspace(filesystem, cue::WorkspaceDirectory::root(), {},
                                          cue::TraversalLimits{4U, 16U, 16U, 1U}, a_assertContext);
    return has_io_error(zero, cue::IoError::CapacityExceeded) && has_io_error(depth, cue::IoError::CapacityExceeded) &&
           has_io_error(visited, cue::IoError::CapacityExceeded) &&
           has_io_error(results, cue::IoError::CapacityExceeded) &&
           has_io_error(metadata, cue::IoError::CapacityExceeded);
}

/// @brief 不完全Snapshotの取得済みEntryと診断がSearchへ保持されるか検証する
[[nodiscard]] bool test_rescan_diagnostic(const cue::AssertContext &a_assertContext)
{
    FakeWorkspaceFilesystem filesystem(a_assertContext);
    auto locator = cue::RelativePath::parse("Diagnostics", a_assertContext);
    if (!locator)
    {
        return false;
    }
    auto directory = filesystem.bind_directory(std::move(*locator.try_value()), a_assertContext);
    if (!directory)
    {
        return false;
    }
    auto result = cue::search_workspace(filesystem, *directory.try_value(), {}, cue::TraversalLimits{2U, 4U, 4U, 1024U},
                                        a_assertContext);
    return result && result.try_value()->state == cue::WorkspaceSnapshotState::RescanRequired &&
           result.try_value()->entries.size() == 1U && result.try_value()->diagnostics.size() == 1U &&
           result.try_value()->diagnostics[0].code == cue::WorkspaceDiagnosticCode::EntryDisappeared;
}

/// @brief Bindingが発行元WorkspaceとAdapterのPath上限を越えて流用されないか検証する
[[nodiscard]] bool test_workspace_binding(const cue::AssertContext &a_assertContext)
{
    FakeWorkspaceFilesystem first(a_assertContext);
    FakeWorkspaceFilesystem second(a_assertContext);
    auto locator = cue::RelativePath::parse("Folder", a_assertContext);
    if (!locator)
    {
        return false;
    }
    auto firstDirectory = first.bind_directory(std::move(*locator.try_value()), a_assertContext);
    if (!firstDirectory)
    {
        return false;
    }
    auto crossWorkspace = second.list_directory(*firstDirectory.try_value(), second.hard_limits());
    auto crossVerification = second.verify_directory(*firstDirectory.try_value());

    FakeWorkspaceFilesystem limited(a_assertContext, 4U);
    auto tooLongLocator = cue::RelativePath::parse("Folder", a_assertContext);
    if (!tooLongLocator)
    {
        return false;
    }
    auto tooLong = limited.bind_directory(std::move(*tooLongLocator.try_value()), a_assertContext);
    return has_io_error(crossWorkspace, cue::IoError::OutsideRoot) &&
           has_io_error(crossVerification, cue::IoError::OutsideRoot) &&
           has_io_error(tooLong, cue::IoError::CapacityExceeded);
}
} // namespace

/// @brief Workspace Portable Coreの決定的列挙・Filter・Bounded Search契約を検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    if (!test_sort_and_filter(assertContext))
    {
        return 1;
    }
    if (!test_bounded_search(assertContext))
    {
        return 2;
    }
    if (!test_rescan_diagnostic(assertContext))
    {
        return 3;
    }
    return test_workspace_binding(assertContext) ? 0 : 4;
}
