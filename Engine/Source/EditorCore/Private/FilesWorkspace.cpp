#include <Cue/EditorCore/FilesWorkspace.h>

#include <Cue/EditorCore/Error.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace
{
/// @brief Area相対LocatorのDirectory深度を比較用に返す
[[nodiscard]] std::size_t locator_depth(std::string_view a_locator) noexcept
{
    return static_cast<std::size_t>(std::ranges::count(a_locator, '/')) + 1U;
}

/// @brief Candidateが同一Locatorまたはその子孫かASCII比較Keyで判定する
[[nodiscard]] bool is_same_or_descendant(std::string_view a_candidate, std::string_view a_ancestor) noexcept
{
    return a_candidate == a_ancestor || (a_candidate.size() > a_ancestor.size() &&
                                         a_candidate.starts_with(a_ancestor) && a_candidate[a_ancestor.size()] == '/');
}

/// @brief ProjectFileServiceとEditorControllerが同じProject Root契約を表すか判定する
[[nodiscard]] bool project_binding_matches(const cue::project_files::ProjectFileService &a_projectFiles,
                                           const cue::ProjectDescriptor &a_descriptor) noexcept
{
    const cue::ProjectRoots &filesRoots = a_projectFiles.roots();
    const cue::ProjectRoots &editorRoots = a_descriptor.roots();
    return a_projectFiles.project_id().text() == a_descriptor.project_id().text() &&
           filesRoots.source_assets().text() == editorRoots.source_assets().text() &&
           filesRoots.runtime_assets().text() == editorRoots.runtime_assets().text() &&
           filesRoots.generated().text() == editorRoots.generated().text() &&
           filesRoots.saved().text() == editorRoots.saved().text();
}
} // namespace

namespace cue::editor_core
{
bool FilesWorkspaceLimits::is_valid() const noexcept
{
    return traversal.is_valid() && contentVerification.is_valid() && watch.is_valid();
}

std::uint64_t FilesViewModel::generation() const noexcept
{
    return m_generation;
}

bool FilesViewModel::is_stale() const noexcept
{
    return m_isStale;
}

std::span<const project_files::ProjectFileDirectorySnapshot> FilesViewModel::directories() const noexcept
{
    return m_directories;
}

std::span<const std::string> FilesViewModel::expanded_directories() const noexcept
{
    return m_expandedDirectories;
}

std::optional<std::string_view> FilesViewModel::selection() const noexcept
{
    return m_selection.has_value() ? std::optional<std::string_view>(*m_selection) : std::nullopt;
}

std::string_view FilesViewModel::search_filter() const noexcept
{
    return m_searchFilter;
}

const project_files::ProjectFileSearchResult *FilesViewModel::try_search_result() const noexcept
{
    return m_searchResult.has_value() ? &*m_searchResult : nullptr;
}

FilesOperationState FilesViewModel::operation_state() const noexcept
{
    return m_operationState;
}

const project_files::ProjectFileOperationResult *FilesViewModel::try_last_operation() const noexcept
{
    return m_lastOperation.has_value() ? &*m_lastOperation : nullptr;
}

const Error *FilesViewModel::try_error() const noexcept
{
    if (m_error.has_value())
    {
        return &*m_error;
    }
    return m_lastOperation.has_value() ? m_lastOperation->try_primary_error() : nullptr;
}

std::span<const project_files::RecoveryEntry> FilesViewModel::recovery_entries() const noexcept
{
    return m_recoveryEntries;
}

FilesWorkspaceService::FilesWorkspaceService(ConstructionKey, project_files::ProjectFileService a_projectFiles,
                                             EditorController &a_editorController, FilesWorkspaceLimits a_limits,
                                             const AssertContext &a_assertContext) noexcept
    : m_projectFiles(std::move(a_projectFiles)), m_editorController(&a_editorController), m_limits(a_limits),
      m_assertContext(a_assertContext), m_ownerThread(std::this_thread::get_id())
{
}

FilesWorkspaceService::~FilesWorkspaceService() noexcept
{
    if (m_watcher != nullptr && std::this_thread::get_id() == m_ownerThread)
    {
        static_cast<void>(m_watcher->stop());
    }
}

Result<std::unique_ptr<FilesWorkspaceService>> FilesWorkspaceService::create(
    project_files::ProjectFileService a_projectFiles, EditorController &a_editorController,
    FilesWorkspaceLimits a_limits, const AssertContext &a_assertContext) noexcept
{
    if (!a_limits.is_valid())
    {
        return Result<std::unique_ptr<FilesWorkspaceService>>::failure(make_editor_core_error(
            a_assertContext, EditorCoreError::InvalidWorkspaceRequest, "Files workspace limits are invalid"));
    }
    if (!project_binding_matches(a_projectFiles, a_editorController.session().project_descriptor()))
    {
        return Result<std::unique_ptr<FilesWorkspaceService>>::failure(
            make_editor_core_error(a_assertContext, EditorCoreError::InvalidWorkspaceRequest,
                                   "Project file service and editor controller describe different projects"));
    }
    Result<void> persistence = a_editorController.require_persistence_services();
    if (!persistence)
    {
        return Result<std::unique_ptr<FilesWorkspaceService>>::failure(std::move(*persistence.try_error()));
    }

    Result<FilesystemIdentity> filesSourceIdentity =
        a_projectFiles.area_root_identity(project_files::ProjectFileArea::SourceAssets);
    Result<FilesystemIdentity> filesSavedIdentity =
        a_projectFiles.area_root_identity(project_files::ProjectFileArea::Saved);
    Result<FilesystemIdentity> editorSourceIdentity = a_editorController.m_sourceAssetsRoot->root_identity();
    Result<FilesystemIdentity> editorSavedIdentity = a_editorController.m_savedRoot->root_identity();
    if (!filesSourceIdentity)
    {
        return Result<std::unique_ptr<FilesWorkspaceService>>::failure(std::move(*filesSourceIdentity.try_error()));
    }
    if (!filesSavedIdentity)
    {
        return Result<std::unique_ptr<FilesWorkspaceService>>::failure(std::move(*filesSavedIdentity.try_error()));
    }
    if (!editorSourceIdentity)
    {
        return Result<std::unique_ptr<FilesWorkspaceService>>::failure(std::move(*editorSourceIdentity.try_error()));
    }
    if (!editorSavedIdentity)
    {
        return Result<std::unique_ptr<FilesWorkspaceService>>::failure(std::move(*editorSavedIdentity.try_error()));
    }
    if (*filesSourceIdentity.try_value() != *editorSourceIdentity.try_value() ||
        *filesSavedIdentity.try_value() != *editorSavedIdentity.try_value())
    {
        return Result<std::unique_ptr<FilesWorkspaceService>>::failure(make_editor_core_error(
            a_assertContext, EditorCoreError::InvalidWorkspaceRequest,
            "Project file service and editor persistence roots identify different workspace entries"));
    }

    std::unique_ptr<FilesWorkspaceService> service;
    try
    {
        service = std::make_unique<FilesWorkspaceService>(ConstructionKey{}, std::move(a_projectFiles),
                                                          a_editorController, a_limits, a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore files workspace allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore files workspace construction failed");
    }

    Result<std::unique_ptr<WorkspaceWatcher>> watcher =
        service->m_projectFiles.create_watcher(project_files::ProjectFileArea::SourceAssets, a_limits.watch);
    if (!watcher)
    {
        return Result<std::unique_ptr<FilesWorkspaceService>>::failure(std::move(*watcher.try_error()));
    }
    service->m_watcher = std::move(*watcher.try_value());

    Result<void> refreshed = service->refresh();
    if (!refreshed)
    {
        return Result<std::unique_ptr<FilesWorkspaceService>>::failure(std::move(*refreshed.try_error()));
    }
    return Result<std::unique_ptr<FilesWorkspaceService>>::success(std::move(service));
}

const FilesViewModel &FilesWorkspaceService::view_model() const noexcept
{
    return m_view;
}

Result<void> FilesWorkspaceService::refresh() noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return owner;
    }
    Result<void> recovery = refresh_recovery_catalog();
    if (!recovery)
    {
        m_view.m_isStale = true;
        return recovery;
    }

    std::vector<project_files::ProjectFileDirectorySnapshot> nextDirectories;
    std::vector<std::string> nextExpanded;
    std::optional<project_files::ProjectFileSearchResult> nextSearch;
    try
    {
        nextDirectories.reserve(m_view.m_expandedDirectories.size() + 1U);
        nextExpanded.reserve(m_view.m_expandedDirectories.size());
    }
    catch (...)
    {
        terminate_allocation();
    }

    Result<project_files::ProjectFileDirectorySnapshot> root =
        m_projectFiles.list_directory(project_files::ProjectFileArea::SourceAssets, {}, m_limits.traversal);
    if (!root)
    {
        m_view.m_isStale = true;
        return Result<void>::failure(retain_error(std::move(*root.try_error()), EditorCoreError::WorkspaceUnavailable,
                                                  "Source assets root could not be listed"));
    }
    try
    {
        nextDirectories.push_back(std::move(*root.try_value()));
    }
    catch (...)
    {
        terminate_allocation();
    }
    if (nextDirectories.back().state == WorkspaceSnapshotState::RescanRequired)
    {
        m_view.m_isStale = true;
        return Result<void>::failure(
            retain_error(make_editor_core_error(m_assertContext, EditorCoreError::WorkspaceUnavailable,
                                                "Source assets root requires an authoritative rescan"),
                         EditorCoreError::WorkspaceUnavailable, "Source assets root requires an authoritative rescan"));
    }

    std::vector<std::string> orderedExpanded;
    try
    {
        orderedExpanded = m_view.m_expandedDirectories;
        std::ranges::sort(orderedExpanded,
                          /// @brief 展開済みDirectoryを親から子、同一深度はLocator順に並べる
                          [](const std::string &a_left, const std::string &a_right) noexcept
                          {
                              const std::size_t leftDepth = locator_depth(a_left);
                              const std::size_t rightDepth = locator_depth(a_right);
                              return leftDepth != rightDepth ? leftDepth < rightDepth : a_left < a_right;
                          });
    }
    catch (...)
    {
        terminate_allocation();
    }

    for (const std::string &expanded : orderedExpanded)
    {
        const std::size_t separator = expanded.rfind('/');
        const std::string_view parent =
            separator == std::string::npos ? std::string_view{} : std::string_view(expanded).substr(0U, separator);
        const auto parentSnapshot =
            std::ranges::find_if(nextDirectories,
                                 /// @brief 展開候補の親Directory SnapshotをLocatorで検索する
                                 [parent](const project_files::ProjectFileDirectorySnapshot &a_snapshot) noexcept
                                 { return a_snapshot.directory == parent; });
        const bool stillDirectory = parentSnapshot != nextDirectories.end() &&
                                    std::ranges::any_of(parentSnapshot->entries,
                                                        /// @brief 展開候補が現在も操作可能Directoryか判定する
                                                        [&expanded](const project_files::ProjectFileEntry &a_entry)
                                                        {
                                                            return a_entry.locator == expanded &&
                                                                   a_entry.is_operable() &&
                                                                   a_entry.type == WorkspaceEntryType::Directory;
                                                        });
        if (!stillDirectory)
        {
            continue;
        }

        Result<project_files::ProjectFileDirectorySnapshot> listed =
            m_projectFiles.list_directory(project_files::ProjectFileArea::SourceAssets, expanded, m_limits.traversal);
        if (!listed)
        {
            m_view.m_isStale = true;
            return Result<void>::failure(retain_error(std::move(*listed.try_error()),
                                                      EditorCoreError::WorkspaceUnavailable,
                                                      "Expanded project directory could not be listed"));
        }
        if (listed.try_value()->state == WorkspaceSnapshotState::RescanRequired)
        {
            m_view.m_isStale = true;
            return Result<void>::failure(retain_error(
                make_editor_core_error(m_assertContext, EditorCoreError::WorkspaceUnavailable,
                                       "Expanded project directory requires an authoritative rescan"),
                EditorCoreError::WorkspaceUnavailable, "Expanded project directory requires an authoritative rescan"));
        }
        try
        {
            nextExpanded.push_back(expanded);
            nextDirectories.push_back(std::move(*listed.try_value()));
        }
        catch (...)
        {
            terminate_allocation();
        }
    }

    if (!m_view.m_searchFilter.empty())
    {
        Result<project_files::ProjectFileSearchResult> searched = m_projectFiles.search(
            project_files::ProjectFileArea::SourceAssets, {}, m_view.m_searchFilter, m_limits.traversal);
        if (!searched)
        {
            m_view.m_isStale = true;
            return Result<void>::failure(retain_error(std::move(*searched.try_error()),
                                                      EditorCoreError::WorkspaceUnavailable,
                                                      "Project file search could not be refreshed"));
        }
        nextSearch.emplace(std::move(*searched.try_value()));
        if (nextSearch->state == WorkspaceSnapshotState::RescanRequired)
        {
            m_view.m_isStale = true;
            return Result<void>::failure(retain_error(
                make_editor_core_error(m_assertContext, EditorCoreError::WorkspaceUnavailable,
                                       "Project file search requires an authoritative rescan"),
                EditorCoreError::WorkspaceUnavailable, "Project file search requires an authoritative rescan"));
        }
    }

    if (m_view.m_generation == std::numeric_limits<std::uint64_t>::max())
    {
        m_view.m_isStale = true;
        return Result<void>::failure(retain_error(
            make_editor_core_error(m_assertContext, EditorCoreError::WorkspaceGenerationExhausted,
                                   "Files workspace generation space is exhausted"),
            EditorCoreError::WorkspaceGenerationExhausted, "Files workspace generation space is exhausted"));
    }

    /// @brief 次Snapshot群またはSearch結果にSelection Locatorが存在するか判定する
    const auto containsSelection = [&](std::string_view a_locator) noexcept
    {
        const bool inDirectories = std::ranges::any_of(
            nextDirectories,
            /// @brief 一つのDirectory SnapshotにSelection Locatorがあるか判定する
            [a_locator](const auto &a_snapshot)
            {
                return std::ranges::any_of(a_snapshot.entries,
                                           /// @brief 一つのEntryがSelection Locatorと一致するか判定する
                                           [a_locator](const auto &a_entry) { return a_entry.locator == a_locator; });
            });
        return inDirectories ||
               (nextSearch.has_value() &&
                std::ranges::any_of(nextSearch->entries,
                                    /// @brief Search EntryがSelection Locatorと一致するか判定する
                                    [a_locator](const auto &a_entry) { return a_entry.locator == a_locator; }));
    };

    const bool selectionStillExists = !m_view.m_selection.has_value() || containsSelection(*m_view.m_selection);
    m_view.m_directories = std::move(nextDirectories);
    m_view.m_expandedDirectories = std::move(nextExpanded);
    m_view.m_searchResult = std::move(nextSearch);
    if (!selectionStillExists)
    {
        m_view.m_selection.reset();
    }
    ++m_view.m_generation;
    m_view.m_isStale = false;
    dismiss_error();
    return Result<void>::success();
}

Result<void> FilesWorkspaceService::set_expanded(std::string_view a_locator, bool a_expanded) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return owner;
    }
    Result<RelativePath> locator = parse_locator(a_locator);
    if (!locator)
    {
        return Result<void>::failure(retain_error(std::move(*locator.try_error()),
                                                  EditorCoreError::InvalidWorkspaceRequest,
                                                  "Expanded directory locator is invalid"));
    }

    if (a_expanded)
    {
        const bool isDirectory = std::ranges::any_of(m_view.m_directories,
                                                     /// @brief 一つのSnapshotに展開可能Directoryがあるか判定する
                                                     [a_locator](const auto &a_snapshot)
                                                     {
                                                         return std::ranges::any_of(
                                                             a_snapshot.entries,
                                                             /// @brief Entryが指定された操作可能Directoryか判定する
                                                             [a_locator](const auto &a_entry)
                                                             {
                                                                 return a_entry.locator == a_locator &&
                                                                        a_entry.type == WorkspaceEntryType::Directory &&
                                                                        a_entry.is_operable();
                                                             });
                                                     });
        if (!isDirectory)
        {
            return Result<void>::failure(
                retain_error(make_editor_core_error(m_assertContext, EditorCoreError::InvalidWorkspaceRequest,
                                                    "Expanded locator is not an available directory"),
                             EditorCoreError::InvalidWorkspaceRequest, "Directory expansion was rejected"));
        }
    }

    std::vector<std::string> previous;
    try
    {
        previous = m_view.m_expandedDirectories;
        if (a_expanded)
        {
            if (std::ranges::find(m_view.m_expandedDirectories, a_locator) == m_view.m_expandedDirectories.end())
            {
                m_view.m_expandedDirectories.emplace_back(a_locator);
            }
        }
        else
        {
            std::erase_if(m_view.m_expandedDirectories,
                          /// @brief 折り畳むDirectory自身と子孫の展開状態を除去する
                          [a_locator](const std::string &a_entry) noexcept
                          { return is_same_or_descendant(a_entry, a_locator); });
        }
    }
    catch (...)
    {
        terminate_allocation();
    }

    Result<void> refreshed = refresh();
    if (!refreshed)
    {
        m_view.m_expandedDirectories = std::move(previous);
    }
    return refreshed;
}

Result<void> FilesWorkspaceService::select(std::string_view a_locator) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return owner;
    }
    Result<RelativePath> locator = parse_locator(a_locator);
    if (!locator || !contains_operable_entry(a_locator))
    {
        Error error = locator ? make_editor_core_error(m_assertContext, EditorCoreError::InvalidWorkspaceRequest,
                                                       "Selected entry is unavailable")
                              : std::move(*locator.try_error());
        return Result<void>::failure(
            retain_error(std::move(error), EditorCoreError::InvalidWorkspaceRequest, "File selection was rejected"));
    }
    try
    {
        m_view.m_selection.emplace(a_locator);
    }
    catch (...)
    {
        terminate_allocation();
    }
    dismiss_error();
    return Result<void>::success();
}

Result<void> FilesWorkspaceService::clear_selection() noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return owner;
    }
    m_view.m_selection.reset();
    return Result<void>::success();
}

Result<void> FilesWorkspaceService::set_search_filter(std::string_view a_filter) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return owner;
    }
    if (a_filter.empty())
    {
        m_view.m_searchFilter.clear();
        m_view.m_searchResult.reset();
        if (m_view.m_selection.has_value() && !contains_operable_entry(*m_view.m_selection))
        {
            m_view.m_selection.reset();
        }
        dismiss_error();
        return Result<void>::success();
    }

    Result<project_files::ProjectFileSearchResult> searched =
        m_projectFiles.search(project_files::ProjectFileArea::SourceAssets, {}, a_filter, m_limits.traversal);
    if (!searched)
    {
        return Result<void>::failure(retain_error(std::move(*searched.try_error()),
                                                  EditorCoreError::InvalidWorkspaceRequest,
                                                  "Project file search was rejected"));
    }
    if (searched.try_value()->state == WorkspaceSnapshotState::RescanRequired)
    {
        m_view.m_isStale = true;
        return Result<void>::failure(retain_error(
            make_editor_core_error(m_assertContext, EditorCoreError::WorkspaceUnavailable,
                                   "Project file search requires an authoritative rescan"),
            EditorCoreError::WorkspaceUnavailable, "Project file search requires an authoritative rescan"));
    }
    try
    {
        m_view.m_searchFilter.assign(a_filter);
    }
    catch (...)
    {
        terminate_allocation();
    }
    m_view.m_searchResult.emplace(std::move(*searched.try_value()));
    if (m_view.m_selection.has_value() && !contains_operable_entry(*m_view.m_selection))
    {
        m_view.m_selection.reset();
    }
    dismiss_error();
    return Result<void>::success();
}

Result<project_files::ProjectFileOperationOutcome> FilesWorkspaceService::create_directory(
    std::string_view a_destination) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return Result<project_files::ProjectFileOperationOutcome>::failure(std::move(*owner.try_error()));
    }
    Result<RelativePath> destination = parse_locator(a_destination);
    if (!destination)
    {
        return reject_operation(std::move(*destination.try_error()), EditorCoreError::InvalidWorkspaceRequest,
                                "Directory creation destination is invalid");
    }
    return apply_operation(m_projectFiles.create_directory(project_files::ProjectFileArea::SourceAssets,
                                                           std::move(*destination.try_value())));
}

Result<project_files::ProjectFileOperationOutcome> FilesWorkspaceService::create_file(
    std::string_view a_destination, std::span<const std::byte> a_bytes) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return Result<project_files::ProjectFileOperationOutcome>::failure(std::move(*owner.try_error()));
    }
    Result<RelativePath> destination = parse_locator(a_destination);
    if (!destination)
    {
        return reject_operation(std::move(*destination.try_error()), EditorCoreError::InvalidWorkspaceRequest,
                                "File creation destination is invalid");
    }
    return apply_operation(m_projectFiles.create_file(project_files::ProjectFileArea::SourceAssets,
                                                      std::move(*destination.try_value()), a_bytes));
}

Result<project_files::ProjectFileOperationOutcome> FilesWorkspaceService::rename(
    std::string_view a_source, std::string_view a_destination) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return Result<project_files::ProjectFileOperationOutcome>::failure(std::move(*owner.try_error()));
    }
    Result<RelativePath> source = parse_locator(a_source);
    Result<RelativePath> destination = parse_locator(a_destination);
    if (!source || !destination)
    {
        Error error = !source ? std::move(*source.try_error()) : std::move(*destination.try_error());
        return reject_operation(std::move(error), EditorCoreError::InvalidWorkspaceRequest,
                                "Rename request is invalid");
    }
    Result<void> guarded = guard_open_documents(*source.try_value());
    if (!guarded)
    {
        return reject_operation(std::move(*guarded.try_error()), EditorCoreError::WorkspaceEntryInUse,
                                "Rename was blocked by an open document");
    }
    return apply_operation(m_projectFiles.rename(project_files::ProjectFileArea::SourceAssets,
                                                 std::move(*source.try_value()), std::move(*destination.try_value()),
                                                 m_limits.traversal));
}

Result<project_files::ProjectFileOperationOutcome> FilesWorkspaceService::move(std::string_view a_source,
                                                                               std::string_view a_destination) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return Result<project_files::ProjectFileOperationOutcome>::failure(std::move(*owner.try_error()));
    }
    Result<RelativePath> source = parse_locator(a_source);
    Result<RelativePath> destination = parse_locator(a_destination);
    if (!source || !destination)
    {
        Error error = !source ? std::move(*source.try_error()) : std::move(*destination.try_error());
        return reject_operation(std::move(error), EditorCoreError::InvalidWorkspaceRequest, "Move request is invalid");
    }
    Result<void> guarded = guard_open_documents(*source.try_value());
    if (!guarded)
    {
        return reject_operation(std::move(*guarded.try_error()), EditorCoreError::WorkspaceEntryInUse,
                                "Move was blocked by an open document");
    }
    return apply_operation(m_projectFiles.move(project_files::ProjectFileArea::SourceAssets,
                                               std::move(*source.try_value()), std::move(*destination.try_value()),
                                               m_limits.traversal));
}

Result<project_files::ProjectFileOperationOutcome> FilesWorkspaceService::copy(std::string_view a_source,
                                                                               std::string_view a_destination) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return Result<project_files::ProjectFileOperationOutcome>::failure(std::move(*owner.try_error()));
    }
    Result<RelativePath> source = parse_locator(a_source);
    Result<RelativePath> destination = parse_locator(a_destination);
    if (!source || !destination)
    {
        Error error = !source ? std::move(*source.try_error()) : std::move(*destination.try_error());
        return reject_operation(std::move(error), EditorCoreError::InvalidWorkspaceRequest, "Copy request is invalid");
    }
    return apply_operation(m_projectFiles.copy(project_files::ProjectFileArea::SourceAssets,
                                               std::move(*source.try_value()), std::move(*destination.try_value()),
                                               m_limits.traversal, m_limits.contentVerification));
}

Result<project_files::ProjectFileOperationOutcome> FilesWorkspaceService::delete_entry(
    std::string_view a_source) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return Result<project_files::ProjectFileOperationOutcome>::failure(std::move(*owner.try_error()));
    }
    Result<RelativePath> source = parse_locator(a_source);
    if (!source)
    {
        return reject_operation(std::move(*source.try_error()), EditorCoreError::InvalidWorkspaceRequest,
                                "Recoverable delete source is invalid");
    }
    Result<void> guarded = guard_open_documents(*source.try_value());
    if (!guarded)
    {
        return reject_operation(std::move(*guarded.try_error()), EditorCoreError::WorkspaceEntryInUse,
                                "Recoverable delete was blocked by an open document");
    }
    return apply_operation(m_projectFiles.delete_entry(project_files::ProjectFileArea::SourceAssets,
                                                       std::move(*source.try_value()), m_limits.traversal,
                                                       m_limits.contentVerification));
}

Result<project_files::ProjectFileOperationOutcome> FilesWorkspaceService::restore(
    std::string_view a_operationId) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return Result<project_files::ProjectFileOperationOutcome>::failure(std::move(*owner.try_error()));
    }
    return apply_operation(m_projectFiles.restore(a_operationId, m_limits.traversal, m_limits.contentVerification));
}

Result<bool> FilesWorkspaceService::poll_external_changes() noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return Result<bool>::failure(std::move(*owner.try_error()));
    }
    if (m_watcher == nullptr)
    {
        return Result<bool>::failure(mark_watcher_unavailable("Files workspace watcher is stopped"));
    }

    if (!m_pendingExternalChanges.has_value())
    {
        Result<std::optional<WorkspaceChangeBatch>> drained = m_watcher->drain_changes();
        if (!drained)
        {
            m_view.m_isStale = true;
            if (!m_watcher->is_running())
            {
                m_isWatcherUnavailable = true;
            }
            return Result<bool>::failure(retain_error(std::move(*drained.try_error()),
                                                      EditorCoreError::WorkspaceUnavailable,
                                                      "Files workspace watcher drain failed"));
        }
        if (!drained.try_value()->has_value())
        {
            if (!m_watcher->is_running())
            {
                return Result<bool>::failure(mark_watcher_unavailable("Files workspace watcher terminated"));
            }
            return Result<bool>::success(false);
        }
        m_pendingExternalChanges.emplace(std::move(**drained.try_value()));
    }

    m_view.m_isStale = true;
    for (const EditorDocument &document : m_editorController->session().documents())
    {
        if (!affects_document(*m_pendingExternalChanges, document.scene_locator().text()))
        {
            continue;
        }
        Result<ExternalChangeState> polled = m_editorController->poll_external_change(document.id());
        if (!polled)
        {
            return Result<bool>::failure(retain_error(std::move(*polled.try_error()),
                                                      EditorCoreError::WorkspaceUnavailable,
                                                      "Open document external state could not be refreshed"));
        }
    }

    Result<void> refreshed = refresh();
    if (!refreshed)
    {
        return Result<bool>::failure(std::move(*refreshed.try_error()));
    }
    m_pendingExternalChanges.reset();
    if (!m_watcher->is_running())
    {
        return Result<bool>::failure(mark_watcher_unavailable("Files workspace watcher terminated"));
    }
    return Result<bool>::success(true);
}

Result<void> FilesWorkspaceService::stop() noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return owner;
    }
    if (m_watcher == nullptr)
    {
        return Result<void>::success();
    }
    Result<void> stopped = m_watcher->stop();
    if (!stopped)
    {
        return Result<void>::failure(retain_error(std::move(*stopped.try_error()),
                                                  EditorCoreError::WorkspaceUnavailable,
                                                  "Files workspace watcher stop failed"));
    }
    m_watcher.reset();
    m_pendingExternalChanges.reset();
    static_cast<void>(mark_watcher_unavailable("Files workspace watcher is stopped"));
    return Result<void>::success();
}

Result<RelativePath> FilesWorkspaceService::parse_locator(std::string_view a_locator) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return Result<RelativePath>::failure(std::move(*owner.try_error()));
    }
    if (a_locator.empty())
    {
        return Result<RelativePath>::failure(make_editor_core_error(
            m_assertContext, EditorCoreError::InvalidWorkspaceRequest, "Project file locator is empty"));
    }
    return RelativePath::parse(a_locator, m_assertContext);
}

Result<void> FilesWorkspaceService::guard_open_documents(const RelativePath &a_source) noexcept
{
    const std::string sourceKey = a_source.comparison_key(m_assertContext);
    for (const EditorDocument &document : m_editorController->session().documents())
    {
        const std::string documentKey = document.scene_locator().comparison_key(m_assertContext);
        if (is_same_or_descendant(documentKey, sourceKey))
        {
            return Result<void>::failure(make_editor_core_error(
                m_assertContext, EditorCoreError::WorkspaceEntryInUse,
                "Workspace entry contains an open editor document and cannot be moved or deleted"));
        }
    }
    return Result<void>::success();
}

Result<project_files::ProjectFileOperationOutcome> FilesWorkspaceService::apply_operation(
    Result<project_files::ProjectFileOperationResult> a_operation) noexcept
{
    Result<void> owner = require_owner_thread();
    if (!owner)
    {
        return Result<project_files::ProjectFileOperationOutcome>::failure(std::move(*owner.try_error()));
    }
    m_view.m_operationState = FilesOperationState::InProgress;
    m_view.m_lastOperation.reset();
    m_view.m_error.reset();
    if (!a_operation)
    {
        m_view.m_operationState = FilesOperationState::Failed;
        return Result<project_files::ProjectFileOperationOutcome>::failure(
            retain_error(std::move(*a_operation.try_error()), EditorCoreError::WorkspaceUnavailable,
                         "Project file operation could not be started"));
    }

    project_files::ProjectFileOperationOutcome outcome = a_operation.try_value()->outcome();
    m_view.m_lastOperation.emplace(std::move(*a_operation.try_value()));
    switch (outcome)
    {
    case project_files::ProjectFileOperationOutcome::Committed:
        m_view.m_operationState = FilesOperationState::Succeeded;
        break;
    case project_files::ProjectFileOperationOutcome::NotCommitted:
        m_view.m_operationState = FilesOperationState::Failed;
        break;
    case project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown:
    case project_files::ProjectFileOperationOutcome::ReconciliationRequired:
        m_view.m_operationState = FilesOperationState::ReconciliationRequired;
        break;
    }

    Result<void> refreshed = refresh();
    if (!refreshed)
    {
        m_view.m_operationState = FilesOperationState::ReconciliationRequired;
        return Result<project_files::ProjectFileOperationOutcome>::failure(std::move(*refreshed.try_error()));
    }
    return Result<project_files::ProjectFileOperationOutcome>::success(std::move(outcome));
}

Result<project_files::ProjectFileOperationOutcome> FilesWorkspaceService::reject_operation(
    Error a_error, EditorCoreError a_code, std::string_view a_summary) noexcept
{
    m_view.m_operationState = FilesOperationState::Failed;
    m_view.m_lastOperation.reset();
    return Result<project_files::ProjectFileOperationOutcome>::failure(
        retain_error(std::move(a_error), a_code, a_summary));
}

Result<void> FilesWorkspaceService::refresh_recovery_catalog() noexcept
{
    Result<void> refreshed = m_projectFiles.refresh_recovery_catalog(m_limits.traversal, m_limits.contentVerification);
    if (!refreshed)
    {
        return Result<void>::failure(retain_error(std::move(*refreshed.try_error()),
                                                  EditorCoreError::WorkspaceUnavailable,
                                                  "Project trash catalog could not be refreshed"));
    }
    try
    {
        const std::span<const project_files::RecoveryEntry> entries = m_projectFiles.recovery_entries();
        m_view.m_recoveryEntries.assign(entries.begin(), entries.end());
    }
    catch (...)
    {
        terminate_allocation();
    }
    return Result<void>::success();
}

bool FilesWorkspaceService::contains_operable_entry(std::string_view a_locator) const noexcept
{
    const bool inDirectories = std::ranges::any_of(
        m_view.m_directories,
        /// @brief 一つのDirectory Snapshotに操作可能Locatorがあるか判定する
        [a_locator](const auto &a_snapshot)
        {
            return std::ranges::any_of(a_snapshot.entries,
                                       /// @brief 一つのEntryが指定Locatorと一致し操作可能か判定する
                                       [a_locator](const auto &a_entry)
                                       { return a_entry.locator == a_locator && a_entry.is_operable(); });
        });
    return inDirectories || (m_view.m_searchResult.has_value() &&
                             std::ranges::any_of(m_view.m_searchResult->entries,
                                                 /// @brief Search Entryが指定Locatorと一致し操作可能か判定する
                                                 [a_locator](const auto &a_entry)
                                                 { return a_entry.locator == a_locator && a_entry.is_operable(); }));
}

bool FilesWorkspaceService::affects_document(const WorkspaceChangeBatch &a_batch,
                                             std::string_view a_documentLocator) const noexcept
{
    if (a_batch.state == WorkspaceChangeBatchState::RescanRequired)
    {
        return true;
    }
    Result<RelativePath> documentLocator = RelativePath::parse(a_documentLocator, m_assertContext);
    if (!documentLocator)
    {
        return true;
    }
    const std::string documentKey = documentLocator.try_value()->comparison_key(m_assertContext);
    return std::ranges::any_of(a_batch.changes,
                               /// @brief 一つの変更HintがDocument自身または親Directoryへ影響するか判定する
                               [&](const WorkspaceChangeHint &a_change)
                               {
                                   const std::string locatorKey = a_change.locator.comparison_key(m_assertContext);
                                   if (is_same_or_descendant(documentKey, locatorKey))
                                   {
                                       return true;
                                   }
                                   return a_change.previousLocator.has_value() &&
                                          is_same_or_descendant(
                                              documentKey, a_change.previousLocator->comparison_key(m_assertContext));
                               });
}

Error FilesWorkspaceService::retain_error(Error a_error, EditorCoreError a_code, std::string_view a_summary) noexcept
{
    m_view.m_error.emplace(std::move(a_error));
    return make_editor_core_error(m_assertContext, a_code, a_summary);
}

Error FilesWorkspaceService::mark_watcher_unavailable(std::string_view a_summary) noexcept
{
    m_isWatcherUnavailable = true;
    m_view.m_isStale = true;
    return retain_error(make_editor_core_error(m_assertContext, EditorCoreError::WorkspaceUnavailable, a_summary),
                        EditorCoreError::WorkspaceUnavailable, a_summary);
}

void FilesWorkspaceService::dismiss_error() noexcept
{
    if (m_isWatcherUnavailable)
    {
        m_view.m_isStale = true;
        m_view.m_error.emplace(make_editor_core_error(m_assertContext, EditorCoreError::WorkspaceUnavailable,
                                                      "Files workspace watcher is unavailable"));
        if (m_view.m_operationState == FilesOperationState::Failed && !m_view.m_lastOperation.has_value())
        {
            m_view.m_operationState = FilesOperationState::Idle;
        }
        return;
    }
    m_view.m_error.reset();
    if (m_view.m_operationState == FilesOperationState::Failed && !m_view.m_lastOperation.has_value())
    {
        m_view.m_operationState = FilesOperationState::Idle;
    }
}

Result<void> FilesWorkspaceService::require_owner_thread() const noexcept
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        return Result<void>::failure(make_editor_core_error(m_assertContext, EditorCoreError::InvalidWorkspaceRequest,
                                                            "Files workspace service was called from another thread"));
    }
    return Result<void>::success();
}

[[noreturn]] void FilesWorkspaceService::terminate_allocation() const noexcept
{
    m_assertContext.fatal_handler().terminate("Cue.EditorCore files workspace allocation failed");
    std::abort();
}
} // namespace cue::editor_core
