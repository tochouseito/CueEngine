#include <Cue/IO/WorkspaceFilesystem.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/IO/Error.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <utility>

namespace
{
/// @brief noexcept処理中のAllocation失敗をProject Fatal Policyへ接続する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Workspace filesystem allocation failed");
    std::abort();
}

/// @brief Entry種別をPortable Sort順へ変換する
[[nodiscard]] std::uint8_t entry_rank(cue::WorkspaceEntryType a_type) noexcept
{
    switch (a_type)
    {
    case cue::WorkspaceEntryType::Directory:
        return 0U;
    case cue::WorkspaceEntryType::RegularFile:
        return 1U;
    case cue::WorkspaceEntryType::UnsupportedEntry:
        return 2U;
    }
    return 2U;
}

/// @brief ASCIIだけをLocale非依存Lowercaseへ変換する
[[nodiscard]] char ascii_lower(char a_character) noexcept
{
    if (a_character >= 'A' && a_character <= 'Z')
    {
        return static_cast<char>(a_character + ('a' - 'A'));
    }
    return a_character;
}

/// @brief UTF-8 Byte列中のASCIIを大小文字非依存として部分一致判定する
[[nodiscard]] bool matches_filter(std::string_view a_text, std::string_view a_filter) noexcept
{
    if (a_filter.empty())
    {
        return true;
    }
    if (a_filter.size() > a_text.size())
    {
        return false;
    }
    for (std::size_t start = 0U; start <= a_text.size() - a_filter.size(); ++start)
    {
        bool matches = true;
        for (std::size_t index = 0U; index < a_filter.size(); ++index)
        {
            if (ascii_lower(a_text[start + index]) != ascii_lower(a_filter[index]))
            {
                matches = false;
                break;
            }
        }
        if (matches)
        {
            return true;
        }
    }
    return false;
}

/// @brief Snapshot Entryが使用するBounded Metadata Byte数を返す
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

/// @brief Search診断が使用するBounded Metadata Byte数を返す
[[nodiscard]] std::size_t metadata_bytes(const cue::WorkspaceDiagnostic &a_diagnostic) noexcept
{
    if (a_diagnostic.displayName.size() > std::numeric_limits<std::size_t>::max() - sizeof(cue::WorkspaceDiagnostic))
    {
        return std::numeric_limits<std::size_t>::max();
    }
    return sizeof(cue::WorkspaceDiagnostic) + a_diagnostic.displayName.size();
}

/// @brief 二つのTraversal Limitの小さい方を要素ごとに返す
[[nodiscard]] cue::TraversalLimits intersect_limits(cue::TraversalLimits a_caller, cue::TraversalLimits a_hard) noexcept
{
    return cue::TraversalLimits{
        std::min(a_caller.maxDepth, a_hard.maxDepth), std::min(a_caller.maxVisitedEntries, a_hard.maxVisitedEntries),
        std::min(a_caller.maxResults, a_hard.maxResults), std::min(a_caller.maxMetadataBytes, a_hard.maxMetadataBytes)};
}

/// @brief Search Stack上のDirectoryとRootからのDepthを所有する
struct PendingDirectory final
{
    cue::WorkspaceDirectory directory;
    std::size_t depth = 0U;
};
} // namespace

namespace cue
{
bool TraversalLimits::is_valid() const noexcept
{
    return maxDepth != 0U && maxVisitedEntries != 0U && maxResults != 0U && maxMetadataBytes != 0U;
}

bool ContentVerificationLimits::is_valid() const noexcept
{
    return maxFileBytes != 0U && maxTotalBytes != 0U;
}

std::string_view BoundWorkspacePath::text() const noexcept
{
    return m_text;
}

BoundWorkspacePath::BoundWorkspacePath(std::string a_text, std::uint64_t a_bindingToken) noexcept
    : m_text(std::move(a_text)), m_bindingToken(a_bindingToken)
{
}

WorkspaceDirectory WorkspaceDirectory::root() noexcept
{
    return WorkspaceDirectory(std::nullopt);
}

WorkspaceDirectory WorkspaceDirectory::from_bound_path(BoundWorkspacePath a_locator) noexcept
{
    return WorkspaceDirectory(std::move(a_locator));
}

bool WorkspaceDirectory::is_root() const noexcept
{
    return !m_locator.has_value();
}

const BoundWorkspacePath *WorkspaceDirectory::locator() const noexcept
{
    return m_locator.has_value() ? &*m_locator : nullptr;
}

WorkspaceDirectory::WorkspaceDirectory(std::optional<BoundWorkspacePath> a_locator) noexcept
    : m_locator(std::move(a_locator))
{
}

WorkspaceFilesystem::WorkspaceFilesystem(std::size_t a_maxBoundPathCharacters) noexcept
    : m_bindingToken(0U), m_maxBoundPathCharacters(a_maxBoundPathCharacters)
{
    static std::atomic<std::uint64_t> nextToken{1U};
    m_bindingToken = nextToken.fetch_add(1U, std::memory_order_relaxed);
    if (m_bindingToken == 0U)
    {
        std::abort();
    }
}

FilesystemIdentity WorkspaceFilesystem::make_filesystem_identity(std::uint64_t a_providerScope,
                                                                 std::uint64_t a_entry) noexcept
{
    return FilesystemIdentity(a_providerScope, a_entry);
}

Result<WorkspaceDirectory> WorkspaceFilesystem::bind_directory(RelativePath a_locator,
                                                               const AssertContext &a_assertContext) const noexcept
{
    if (a_locator.text().size() > m_maxBoundPathCharacters)
    {
        return Result<WorkspaceDirectory>::failure(make_io_error(a_assertContext, IoError::CapacityExceeded,
                                                                 "Workspace directory exceeds the native path limit"));
    }
    try
    {
        return Result<WorkspaceDirectory>::success(
            WorkspaceDirectory::from_bound_path(BoundWorkspacePath(std::string(a_locator.text()), m_bindingToken)));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}

Result<BoundWorkspacePath> WorkspaceFilesystem::bind_path(RelativePath a_areaRoot, RelativePath a_locator,
                                                          const AssertContext &a_assertContext) const noexcept
{
    Result<WorkspaceDirectory> area = bind_directory(std::move(a_areaRoot), a_assertContext);
    if (!area)
    {
        return Result<BoundWorkspacePath>::failure(std::move(*area.try_error()));
    }
    return append_path(*area.try_value(), std::move(a_locator), a_assertContext);
}

Result<BoundWorkspacePath> WorkspaceFilesystem::bind_root_path(RelativePath a_locator,
                                                               const AssertContext &a_assertContext) const noexcept
{
    return append_path(WorkspaceDirectory::root(), std::move(a_locator), a_assertContext);
}

Result<BoundWorkspacePath> WorkspaceFilesystem::bind_operation_staging_path(
    const BoundWorkspacePath &a_parent, std::string_view a_operationId, std::string_view a_kind,
    const AssertContext &a_assertContext) const noexcept
{
    const bool validId = a_operationId.size() == 36U && a_operationId[8U] == '-' && a_operationId[13U] == '-' &&
                         a_operationId[18U] == '-' && a_operationId[23U] == '-' && a_operationId[14U] == '4' &&
                         (a_operationId[19U] == '8' || a_operationId[19U] == '9' || a_operationId[19U] == 'a' ||
                          a_operationId[19U] == 'b');
    bool validIdCharacters = validId;
    for (std::size_t index = 0U; validIdCharacters && index < a_operationId.size(); ++index)
    {
        if (index == 8U || index == 13U || index == 18U || index == 23U)
        {
            continue;
        }
        const char value = a_operationId[index];
        validIdCharacters = (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    }
    const bool validKind =
        !a_kind.empty() && a_kind.size() <= 32U &&
        std::ranges::all_of(a_kind,
                            /// @brief Staging kindがlowercase ASCIIまたは内部区切りだけか判定する
                            [](char a_value) noexcept { return (a_value >= 'a' && a_value <= 'z') || a_value == '-'; });
    if (!owns_path(a_parent) || !validIdCharacters || !validKind)
    {
        return Result<BoundWorkspacePath>::failure(
            make_io_error(a_assertContext, owns_path(a_parent) ? IoError::InvalidPath : IoError::OutsideRoot,
                          "Workspace operation staging request is invalid"));
    }
    std::string path;
    try
    {
        path.reserve(a_parent.text().size() + a_operationId.size() + a_kind.size() + 12U);
        path.assign(a_parent.text());
        path.append("/.");
        path.append(a_operationId);
        path.push_back('.');
        path.append(a_kind);
        path.append("-staging");
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    if (path.size() > m_maxBoundPathCharacters)
    {
        return Result<BoundWorkspacePath>::failure(make_io_error(
            a_assertContext, IoError::CapacityExceeded, "Workspace operation staging path exceeds the native limit"));
    }
    return Result<BoundWorkspacePath>::success(BoundWorkspacePath(std::move(path), m_bindingToken));
}

Result<BoundWorkspacePath> WorkspaceFilesystem::bind_child_path(const BoundWorkspacePath &a_parent,
                                                                RelativePath a_child,
                                                                const AssertContext &a_assertContext) const noexcept
{
    if (!owns_path(a_parent))
    {
        return Result<BoundWorkspacePath>::failure(
            make_io_error(a_assertContext, IoError::OutsideRoot, "Workspace parent belongs to another root binding"));
    }
    return append_path(WorkspaceDirectory::from_bound_path(a_parent), std::move(a_child), a_assertContext);
}

bool WorkspaceFilesystem::owns_directory(const WorkspaceDirectory &a_directory) const noexcept
{
    const BoundWorkspacePath *locator = a_directory.locator();
    return locator == nullptr || locator->m_bindingToken == m_bindingToken;
}

bool WorkspaceFilesystem::owns_path(const BoundWorkspacePath &a_path) const noexcept
{
    return a_path.m_bindingToken == m_bindingToken;
}

Result<WorkspaceDirectory> WorkspaceFilesystem::parent_directory(const BoundWorkspacePath &a_path,
                                                                 const AssertContext &a_assertContext) const noexcept
{
    if (!owns_path(a_path))
    {
        return Result<WorkspaceDirectory>::failure(
            make_io_error(a_assertContext, IoError::OutsideRoot, "Workspace path belongs to another root binding"));
    }

    const std::size_t separator = a_path.text().rfind('/');
    if (separator == std::string_view::npos)
    {
        return Result<WorkspaceDirectory>::success(WorkspaceDirectory::root());
    }
    try
    {
        std::string parent(a_path.text().substr(0U, separator));
        return Result<WorkspaceDirectory>::success(
            WorkspaceDirectory::from_bound_path(BoundWorkspacePath(std::move(parent), m_bindingToken)));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}

Result<BoundWorkspacePath> WorkspaceFilesystem::append_path(const WorkspaceDirectory &a_parent, RelativePath a_child,
                                                            const AssertContext &a_assertContext) const noexcept
{
    if (!owns_directory(a_parent))
    {
        return Result<BoundWorkspacePath>::failure(make_io_error(
            a_assertContext, IoError::OutsideRoot, "Workspace directory belongs to another root binding"));
    }

    const BoundWorkspacePath *parent = a_parent.locator();
    const std::size_t parentLength = parent == nullptr ? 0U : parent->text().size();
    const std::size_t separatorLength = parent == nullptr ? 0U : 1U;
    if (parentLength > m_maxBoundPathCharacters ||
        separatorLength + a_child.text().size() > m_maxBoundPathCharacters - parentLength)
    {
        return Result<BoundWorkspacePath>::failure(
            make_io_error(a_assertContext, IoError::CapacityExceeded, "Workspace path exceeds the native path limit"));
    }

    try
    {
        std::string text;
        if (parent != nullptr)
        {
            text.assign(parent->text());
            text.push_back('/');
        }
        text.append(a_child.text());
        return Result<BoundWorkspacePath>::success(BoundWorkspacePath(std::move(text), m_bindingToken));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}

bool WorkspaceEntry::is_operable() const noexcept
{
    return locator.has_value() && !rejection.has_value() && type != WorkspaceEntryType::UnsupportedEntry;
}

void sort_workspace_entries(std::vector<WorkspaceEntry> &a_entries) noexcept
{
    std::sort(a_entries.begin(), a_entries.end(),
              [](const WorkspaceEntry &a_left, const WorkspaceEntry &a_right)
              {
                  const std::uint8_t leftRank = entry_rank(a_left.type);
                  const std::uint8_t rightRank = entry_rank(a_right.type);
                  if (leftRank != rightRank)
                  {
                      return leftRank < rightRank;
                  }
                  if (a_left.sortKey != a_right.sortKey)
                  {
                      return a_left.sortKey < a_right.sortKey;
                  }
                  if (a_left.displayName != a_right.displayName)
                  {
                      return a_left.displayName < a_right.displayName;
                  }
                  const std::string_view leftLocator =
                      a_left.locator.has_value() ? a_left.locator->text() : std::string_view{};
                  const std::string_view rightLocator =
                      a_right.locator.has_value() ? a_right.locator->text() : std::string_view{};
                  return leftLocator < rightLocator;
              });
}

Result<std::vector<WorkspaceEntry>> filter_workspace_snapshot(const DirectorySnapshot &a_snapshot,
                                                              std::string_view a_filter, std::size_t a_maxResults,
                                                              const AssertContext &a_assertContext) noexcept
{
    if (a_maxResults == 0U)
    {
        return Result<std::vector<WorkspaceEntry>>::failure(make_io_error(
            a_assertContext, IoError::CapacityExceeded, "Workspace filter result limit must be non-zero"));
    }

    std::vector<WorkspaceEntry> filtered;
    try
    {
        filtered.reserve(std::min(a_snapshot.entries.size(), a_maxResults));
        for (const WorkspaceEntry &entry : a_snapshot.entries)
        {
            if (!matches_filter(entry.displayName, a_filter))
            {
                continue;
            }
            if (filtered.size() == a_maxResults)
            {
                return Result<std::vector<WorkspaceEntry>>::failure(make_io_error(
                    a_assertContext, IoError::CapacityExceeded, "Workspace filter result limit was exceeded"));
            }
            filtered.push_back(entry);
        }
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    return Result<std::vector<WorkspaceEntry>>::success(std::move(filtered));
}

Result<WorkspaceSearchResult> search_workspace(WorkspaceFilesystem &a_filesystem,
                                               const WorkspaceDirectory &a_startDirectory, std::string_view a_filter,
                                               TraversalLimits a_limits, const AssertContext &a_assertContext) noexcept
{
    const TraversalLimits hardLimits = a_filesystem.hard_limits();
    if (!a_limits.is_valid() || !hardLimits.is_valid())
    {
        return Result<WorkspaceSearchResult>::failure(
            make_io_error(a_assertContext, IoError::CapacityExceeded, "Workspace search limits must all be non-zero"));
    }
    const TraversalLimits effective = intersect_limits(a_limits, hardLimits);

    WorkspaceSearchResult result;
    std::vector<PendingDirectory> pending;
    std::size_t metadataBytes = 0U;
    try
    {
        pending.push_back(PendingDirectory{a_startDirectory, 1U});
        while (!pending.empty())
        {
            PendingDirectory current = std::move(pending.back());
            pending.pop_back();

            TraversalLimits directLimits = effective;
            directLimits.maxDepth = 1U;
            directLimits.maxVisitedEntries = effective.maxVisitedEntries - result.visitedEntries;
            directLimits.maxResults = directLimits.maxVisitedEntries;
            directLimits.maxMetadataBytes = effective.maxMetadataBytes - metadataBytes;
            if (directLimits.maxVisitedEntries == 0U || directLimits.maxResults == 0U ||
                directLimits.maxMetadataBytes == 0U)
            {
                return Result<WorkspaceSearchResult>::failure(make_io_error(
                    a_assertContext, IoError::CapacityExceeded, "Workspace search traversal limit was exceeded"));
            }

            Result<DirectorySnapshot> listed = a_filesystem.list_directory(current.directory, directLimits);
            if (!listed)
            {
                return Result<WorkspaceSearchResult>::failure(std::move(*listed.try_error()));
            }
            DirectorySnapshot snapshot = std::move(*listed.try_value());
            if (snapshot.state == WorkspaceSnapshotState::RescanRequired)
            {
                result.state = WorkspaceSnapshotState::RescanRequired;
            }
            for (const WorkspaceDiagnostic &diagnostic : snapshot.diagnostics)
            {
                const std::size_t diagnosticBytes = metadata_bytes(diagnostic);
                if (diagnosticBytes > effective.maxMetadataBytes - metadataBytes)
                {
                    return Result<WorkspaceSearchResult>::failure(make_io_error(
                        a_assertContext, IoError::CapacityExceeded, "Workspace search metadata limit was exceeded"));
                }
                metadataBytes += diagnosticBytes;
            }
            result.diagnostics.insert(result.diagnostics.end(), std::make_move_iterator(snapshot.diagnostics.begin()),
                                      std::make_move_iterator(snapshot.diagnostics.end()));

            std::vector<WorkspaceDirectory> children;
            for (WorkspaceEntry &entry : snapshot.entries)
            {
                if (result.visitedEntries == effective.maxVisitedEntries)
                {
                    return Result<WorkspaceSearchResult>::failure(
                        make_io_error(a_assertContext, IoError::CapacityExceeded,
                                      "Workspace search visited-entry limit was exceeded"));
                }
                ++result.visitedEntries;
                const std::size_t entryBytes = metadata_bytes(entry);
                if (entryBytes > effective.maxMetadataBytes - metadataBytes)
                {
                    return Result<WorkspaceSearchResult>::failure(make_io_error(
                        a_assertContext, IoError::CapacityExceeded, "Workspace search metadata limit was exceeded"));
                }
                metadataBytes += entryBytes;

                if (entry.type == WorkspaceEntryType::Directory && entry.is_operable())
                {
                    if (current.depth == effective.maxDepth)
                    {
                        return Result<WorkspaceSearchResult>::failure(make_io_error(
                            a_assertContext, IoError::CapacityExceeded, "Workspace search depth limit was exceeded"));
                    }
                    children.push_back(WorkspaceDirectory::from_bound_path(*entry.locator));
                }
                if (matches_filter(entry.displayName, a_filter))
                {
                    if (result.entries.size() == effective.maxResults)
                    {
                        return Result<WorkspaceSearchResult>::failure(make_io_error(
                            a_assertContext, IoError::CapacityExceeded, "Workspace search result limit was exceeded"));
                    }
                    result.entries.push_back(std::move(entry));
                }
            }

            for (auto iterator = children.rbegin(); iterator != children.rend(); ++iterator)
            {
                pending.push_back(PendingDirectory{std::move(*iterator), current.depth + 1U});
            }
        }
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }

    sort_workspace_entries(result.entries);
    return Result<WorkspaceSearchResult>::success(std::move(result));
}
} // namespace cue
