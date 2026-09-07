#include <Cue/ProjectFiles/Service.h>

#include "TrashRecord.h"

#include <Cue/IO/Error.h>
#include <Cue/ProjectFiles/Error.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace
{
constexpr std::size_t k_maximumProjectDescriptorBytes = 1024U * 1024U;
constexpr std::size_t k_maximumTrashRecordBytes = 16U * 1024U * 1024U;

/// @brief noexcept境界でのAllocation失敗をFatal終了へ変換する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Project file service allocation failed");
    std::abort();
}

/// @brief Workspace列挙診断を保持可能なPortable IO Errorへ分類する
[[nodiscard]] cue::IoError classify_workspace_diagnostic(cue::WorkspaceDiagnosticCode a_code) noexcept
{
    switch (a_code)
    {
    case cue::WorkspaceDiagnosticCode::UnsupportedName:
        return cue::IoError::InvalidPath;
    case cue::WorkspaceDiagnosticCode::ReparsePoint:
        return cue::IoError::UnsupportedEntry;
    case cue::WorkspaceDiagnosticCode::EntryDisappeared:
        return cue::IoError::NotFound;
    case cue::WorkspaceDiagnosticCode::PermissionDenied:
        return cue::IoError::PermissionDenied;
    case cue::WorkspaceDiagnosticCode::TypeChanged:
        return cue::IoError::TypeMismatch;
    case cue::WorkspaceDiagnosticCode::EnumerationFailed:
        return cue::IoError::IoFailure;
    }
    return cue::IoError::IoFailure;
}

class BusyReset final
{
  public:
    /// @brief Scope中の再入Mutationを拒否するBusy状態を開始する
    explicit BusyReset(bool &a_isBusy) noexcept : m_isBusy(&a_isBusy)
    {
        *m_isBusy = true;
    }
    /// @brief 単一Busy状態の解除責務を保つためCopy構築を禁止する
    BusyReset(const BusyReset &) = delete;
    /// @brief 単一Busy状態の解除責務を保つためCopy代入を禁止する
    BusyReset &operator=(const BusyReset &) = delete;
    /// @brief Scope終了時にBusy状態を解除する
    ~BusyReset()
    {
        *m_isBusy = false;
    }

  private:
    bool *m_isBusy;
};

/// @brief IO ErrorとMutation結果をProject File Errorへ分類する
[[nodiscard]] cue::project_files::ProjectFileError classify_project_file_error(
    const cue::Error &a_error, cue::WorkspaceMutationOutcome a_outcome) noexcept
{
    if (a_outcome == cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown ||
        a_outcome == cue::WorkspaceMutationOutcome::ReconciliationRequired)
    {
        return cue::project_files::ProjectFileError::RecoveryRequired;
    }

    const cue::ErrorCode &code = a_error.root_code();
    if (code.domain() != "Cue.IO")
    {
        return cue::project_files::ProjectFileError::StorageFailure;
    }
    switch (static_cast<cue::IoError>(code.value()))
    {
    case cue::IoError::AlreadyExists:
        return cue::project_files::ProjectFileError::Conflict;
    case cue::IoError::CapacityExceeded:
        return cue::project_files::ProjectFileError::LimitExceeded;
    case cue::IoError::Busy:
        return cue::project_files::ProjectFileError::Busy;
    case cue::IoError::InvalidPath:
    case cue::IoError::OutsideRoot:
    case cue::IoError::NotFound:
    case cue::IoError::TypeMismatch:
    case cue::IoError::UnsupportedEntry:
    case cue::IoError::PreconditionFailed:
        return cue::project_files::ProjectFileError::InvalidRequest;
    case cue::IoError::PermissionDenied:
    case cue::IoError::IoFailure:
    case cue::IoError::DurabilityUnknown:
        return cue::project_files::ProjectFileError::StorageFailure;
    }
    return cue::project_files::ProjectFileError::StorageFailure;
}

/// @brief IO Mutation結果をProject File Operation結果へ変換する
[[nodiscard]] cue::project_files::ProjectFileOperationOutcome convert_outcome(
    cue::WorkspaceMutationOutcome a_outcome) noexcept
{
    switch (a_outcome)
    {
    case cue::WorkspaceMutationOutcome::Committed:
        return cue::project_files::ProjectFileOperationOutcome::Committed;
    case cue::WorkspaceMutationOutcome::NotCommitted:
        return cue::project_files::ProjectFileOperationOutcome::NotCommitted;
    case cue::WorkspaceMutationOutcome::CommittedButDurabilityUnknown:
        return cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown;
    case cue::WorkspaceMutationOutcome::ReconciliationRequired:
        return cue::project_files::ProjectFileOperationOutcome::ReconciliationRequired;
    }
    return cue::project_files::ProjectFileOperationOutcome::ReconciliationRequired;
}

/// @brief Error ChainのRootが指定IO分類か判定する
[[nodiscard]] bool is_io_error(const cue::Error &a_error, cue::IoError a_code) noexcept
{
    const cue::ErrorCode &code = a_error.root_code();
    return code.domain() == "Cue.IO" && code.value() == static_cast<std::int64_t>(a_code);
}

/// @brief Area相対Pathの親Locatorを空文字許容で返す
[[nodiscard]] std::string parent_locator(std::string_view a_path, const cue::AssertContext &a_assertContext) noexcept
{
    std::string parent;
    try
    {
        const std::size_t separator = a_path.rfind('/');
        if (separator != std::string_view::npos)
        {
            parent.assign(a_path.substr(0U, separator));
        }
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    return parent;
}

/// @brief Fingerprintが保持する全Regular FileのLogical Byte数を飽和加算する
[[nodiscard]] std::uint64_t fingerprint_byte_size(const cue::WorkspaceEntryFingerprint &a_fingerprint) noexcept
{
    if (a_fingerprint.file.has_value())
    {
        return a_fingerprint.file->byteSize;
    }
    std::uint64_t total = 0U;
    for (const cue::WorkspaceManifestEntry &entry : a_fingerprint.manifest)
    {
        if (entry.file.has_value())
        {
            if (entry.file->byteSize > std::numeric_limits<std::uint64_t>::max() - total)
            {
                return std::numeric_limits<std::uint64_t>::max();
            }
            total += entry.file->byteSize;
        }
    }
    return total;
}

/// @brief Workspace Root相対EntryをProject Area相対の所有値へ変換する
[[nodiscard]] cue::Result<cue::project_files::ProjectFileEntry> make_project_file_entry(
    const cue::WorkspaceEntry &a_entry, std::string_view a_areaRoot, const cue::AssertContext &a_assertContext) noexcept
{
    cue::project_files::ProjectFileEntry converted;
    converted.parentGeneration = a_entry.parentGeneration;
    converted.type = a_entry.type;
    converted.byteSize = a_entry.byteSize;
    converted.rejection = a_entry.rejection;
    try
    {
        converted.displayName = a_entry.displayName;
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }

    if (a_entry.locator.has_value())
    {
        const std::string_view rootRelative = a_entry.locator->text();
        if (rootRelative.size() <= a_areaRoot.size() || !rootRelative.starts_with(a_areaRoot) ||
            rootRelative[a_areaRoot.size()] != '/')
        {
            return cue::Result<cue::project_files::ProjectFileEntry>::failure(
                cue::project_files::make_project_file_error(
                    a_assertContext, cue::project_files::ProjectFileError::StorageFailure,
                    "Workspace listing returned an entry outside the requested project area"));
        }
        try
        {
            converted.locator.assign(rootRelative.substr(a_areaRoot.size() + 1U));
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
    }
    return cue::Result<cue::project_files::ProjectFileEntry>::success(std::move(converted));
}
} // namespace

namespace cue::project_files
{
ProjectFileAccessPolicy ProjectFileAccessPolicy::editor_default() noexcept
{
    return ProjectFileAccessPolicy();
}

bool ProjectFileAccessPolicy::can_list(ProjectFileArea a_area) const noexcept
{
    return a_area == ProjectFileArea::SourceAssets;
}

bool ProjectFileAccessPolicy::can_mutate(ProjectFileArea a_area) const noexcept
{
    return a_area == ProjectFileArea::SourceAssets;
}

bool ProjectFileEntry::is_operable() const noexcept
{
    return !locator.empty() && !rejection.has_value() &&
           (type == WorkspaceEntryType::Directory || type == WorkspaceEntryType::RegularFile);
}

ProjectFileOperationResult::ProjectFileOperationResult(std::string a_operationId, ProjectFileOperationKind a_kind,
                                                       ProjectFileArea a_area, std::optional<std::string> a_source,
                                                       std::string a_destination, ProjectFileOperationStage a_stage,
                                                       ProjectFileOperationOutcome a_outcome,
                                                       std::optional<Error> a_primaryError,
                                                       std::vector<Error> a_secondaryDiagnostics,
                                                       std::vector<std::string> a_rescanDirectories) noexcept
    : m_operationId(std::move(a_operationId)), m_kind(a_kind), m_area(a_area), m_source(std::move(a_source)),
      m_destination(std::move(a_destination)), m_stage(a_stage), m_outcome(a_outcome),
      m_primaryError(std::move(a_primaryError)), m_secondaryDiagnostics(std::move(a_secondaryDiagnostics)),
      m_rescanDirectories(std::move(a_rescanDirectories))
{
}

std::string_view ProjectFileOperationResult::operation_id() const noexcept
{
    return m_operationId;
}

ProjectFileOperationKind ProjectFileOperationResult::kind() const noexcept
{
    return m_kind;
}

ProjectFileArea ProjectFileOperationResult::area() const noexcept
{
    return m_area;
}

std::optional<std::string_view> ProjectFileOperationResult::source() const noexcept
{
    return m_source.has_value() ? std::optional<std::string_view>(*m_source) : std::nullopt;
}

std::string_view ProjectFileOperationResult::destination() const noexcept
{
    return m_destination;
}

ProjectFileOperationStage ProjectFileOperationResult::stage() const noexcept
{
    return m_stage;
}

ProjectFileOperationOutcome ProjectFileOperationResult::outcome() const noexcept
{
    return m_outcome;
}

const Error *ProjectFileOperationResult::try_primary_error() const noexcept
{
    return m_primaryError.has_value() ? &*m_primaryError : nullptr;
}

std::span<const Error> ProjectFileOperationResult::secondary_diagnostics() const noexcept
{
    return m_secondaryDiagnostics;
}

std::span<const std::string> ProjectFileOperationResult::rescan_directories() const noexcept
{
    return m_rescanDirectories;
}

ProjectFileService::ProjectFileService(ProjectId a_projectId, ProjectRoots a_roots, ProjectFileAccessPolicy a_policy,
                                       std::unique_ptr<WorkspaceFilesystem> a_workspace,
                                       std::unique_ptr<ProjectFileOperationIdSource> a_operationIdSource,
                                       const AssertContext &a_assertContext) noexcept
    : m_projectId(std::move(a_projectId)), m_roots(std::move(a_roots)), m_policy(a_policy),
      m_workspace(std::move(a_workspace)), m_operationIdSource(std::move(a_operationIdSource)),
      m_assertContext(a_assertContext), m_ownerThread(std::this_thread::get_id())
{
}

Result<ProjectFileService> ProjectFileService::create(const ProjectDescriptor &a_descriptor,
                                                      std::unique_ptr<WorkspaceFilesystem> a_workspace,
                                                      std::unique_ptr<ProjectFileOperationIdSource> a_operationIdSource,
                                                      const AssertContext &a_assertContext) noexcept
{
    if (a_workspace == nullptr || a_operationIdSource == nullptr)
    {
        return Result<ProjectFileService>::failure(make_project_file_error(
            a_assertContext, ProjectFileError::InvalidRequest, "Project file service dependency is missing"));
    }
    Result<void> validated = validate_project_descriptor(a_descriptor, a_assertContext);
    if (!validated)
    {
        return Result<ProjectFileService>::failure(
            reclassify_project_file_error(a_assertContext, ProjectFileError::InvalidRequest,
                                          "Project descriptor is invalid", std::move(*validated.try_error())));
    }

    Result<RelativePath> descriptorLocator = RelativePath::parse("CueProject.json", a_assertContext);
    if (!descriptorLocator)
    {
        return Result<ProjectFileService>::failure(reclassify_project_file_error(
            a_assertContext, ProjectFileError::InvalidRequest, "Project descriptor locator is invalid",
            std::move(*descriptorLocator.try_error())));
    }
    Result<BoundWorkspacePath> boundDescriptor =
        a_workspace->bind_root_path(std::move(*descriptorLocator.try_value()), a_assertContext);
    if (!boundDescriptor)
    {
        return Result<ProjectFileService>::failure(reclassify_project_file_error(
            a_assertContext, ProjectFileError::InvalidRequest, "Project descriptor could not be bound to workspace",
            std::move(*boundDescriptor.try_error())));
    }
    Result<std::vector<std::byte>> descriptorBytes =
        a_workspace->read_file_bounded(*boundDescriptor.try_value(), k_maximumProjectDescriptorBytes);
    if (!descriptorBytes)
    {
        const ProjectFileError classification =
            classify_project_file_error(*descriptorBytes.try_error(), WorkspaceMutationOutcome::NotCommitted);
        Error cause = std::move(*descriptorBytes.try_error());
        return Result<ProjectFileService>::failure(reclassify_project_file_error(
            a_assertContext, classification, "Project descriptor could not be read from workspace", std::move(cause)));
    }
    const std::string_view descriptorText(reinterpret_cast<const char *>(descriptorBytes.try_value()->data()),
                                          descriptorBytes.try_value()->size());
    Result<ProjectDescriptor> workspaceDescriptor = parse_project_descriptor(descriptorText, a_assertContext);
    if (!workspaceDescriptor || !a_descriptor.equivalent_to(*workspaceDescriptor.try_value()))
    {
        std::optional<Error> cause;
        if (!workspaceDescriptor)
        {
            cause.emplace(std::move(*workspaceDescriptor.try_error()));
        }
        return Result<ProjectFileService>::failure(
            cause.has_value()
                ? reclassify_project_file_error(a_assertContext, ProjectFileError::InvalidRequest,
                                                "Workspace project descriptor is invalid", std::move(*cause))
                : make_project_file_error(a_assertContext, ProjectFileError::InvalidRequest,
                                          "Workspace project descriptor does not match the requested project"));
    }

    Result<ProjectId> projectId = ProjectId::parse(a_descriptor.project_id().text(), a_assertContext);
    Result<RelativePath> sourceAssets =
        RelativePath::parse(a_descriptor.roots().source_assets().text(), a_assertContext);
    Result<RelativePath> runtimeAssets =
        RelativePath::parse(a_descriptor.roots().runtime_assets().text(), a_assertContext);
    Result<RelativePath> generated = RelativePath::parse(a_descriptor.roots().generated().text(), a_assertContext);
    Result<RelativePath> saved = RelativePath::parse(a_descriptor.roots().saved().text(), a_assertContext);
    if (!projectId || !sourceAssets || !runtimeAssets || !generated || !saved)
    {
        return Result<ProjectFileService>::failure(make_project_file_error(
            a_assertContext, ProjectFileError::InvalidRequest, "Project descriptor snapshot could not be cloned"));
    }

    ProjectRoots roots(std::move(*sourceAssets.try_value()), std::move(*runtimeAssets.try_value()),
                       std::move(*generated.try_value()), std::move(*saved.try_value()));
    Result<WorkspaceDirectory> sourceRoot = a_workspace->bind_directory(roots.source_assets(), a_assertContext);
    if (!sourceRoot)
    {
        const ProjectFileError classification =
            classify_project_file_error(*sourceRoot.try_error(), WorkspaceMutationOutcome::NotCommitted);
        Error cause = std::move(*sourceRoot.try_error());
        return Result<ProjectFileService>::failure(reclassify_project_file_error(
            a_assertContext, classification, "Project source assets root could not be bound", std::move(cause)));
    }
    Result<void> verifiedSourceRoot = a_workspace->verify_directory(*sourceRoot.try_value());
    if (!verifiedSourceRoot)
    {
        const ProjectFileError classification =
            classify_project_file_error(*verifiedSourceRoot.try_error(), WorkspaceMutationOutcome::NotCommitted);
        Error cause = std::move(*verifiedSourceRoot.try_error());
        return Result<ProjectFileService>::failure(reclassify_project_file_error(
            a_assertContext, classification, "Project source assets root is unavailable", std::move(cause)));
    }

    ProjectFileService service(std::move(*projectId.try_value()), std::move(roots),
                               ProjectFileAccessPolicy::editor_default(), std::move(a_workspace),
                               std::move(a_operationIdSource), a_assertContext);
    return Result<ProjectFileService>::success(std::move(service));
}

const ProjectId &ProjectFileService::project_id() const noexcept
{
    return m_projectId;
}

const ProjectRoots &ProjectFileService::roots() const noexcept
{
    return m_roots;
}

const ProjectFileAccessPolicy &ProjectFileService::access_policy() const noexcept
{
    return m_policy;
}

/// @brief Project Area Directoryを再帰監視する独立Watcherを生成する
Result<std::unique_ptr<WorkspaceWatcher>> ProjectFileService::create_watcher(ProjectFileArea a_area,
                                                                             WorkspaceWatchLimits a_limits) noexcept
{
    if (std::this_thread::get_id() != m_ownerThread || m_isBusy || !m_policy.can_list(a_area) || !a_limits.is_valid())
    {
        return Result<std::unique_ptr<WorkspaceWatcher>>::failure(make_project_file_error(
            m_assertContext, m_isBusy ? ProjectFileError::Busy : ProjectFileError::InvalidRequest,
            m_isBusy ? "Project file mutation is already active" : "Project file watcher request is invalid"));
    }
    Result<WorkspaceDirectory> directory = m_workspace->bind_directory(area_root(a_area), m_assertContext);
    if (!directory)
    {
        const ProjectFileError classification =
            classify_project_file_error(*directory.try_error(), WorkspaceMutationOutcome::NotCommitted);
        return Result<std::unique_ptr<WorkspaceWatcher>>::failure(reclassify_project_file_error(
            m_assertContext, classification, "Project file watcher area could not be bound",
            std::move(*directory.try_error())));
    }
    Result<std::unique_ptr<WorkspaceWatcher>> watcher = m_workspace->create_watcher(*directory.try_value(), a_limits);
    if (!watcher)
    {
        const ProjectFileError classification =
            classify_project_file_error(*watcher.try_error(), WorkspaceMutationOutcome::NotCommitted);
        return Result<std::unique_ptr<WorkspaceWatcher>>::failure(
            reclassify_project_file_error(m_assertContext, classification, "Project file watcher could not be created",
                                          std::move(*watcher.try_error())));
    }
    return watcher;
}

Result<ProjectFileDirectorySnapshot> ProjectFileService::list_directory(ProjectFileArea a_area,
                                                                        std::string_view a_directory,
                                                                        TraversalLimits a_limits) noexcept
{
    if (std::this_thread::get_id() != m_ownerThread || m_isBusy || !m_policy.can_list(a_area) || !a_limits.is_valid())
    {
        return Result<ProjectFileDirectorySnapshot>::failure(make_project_file_error(
            m_assertContext, m_isBusy ? ProjectFileError::Busy : ProjectFileError::InvalidRequest,
            m_isBusy ? "Project file mutation is already active" : "Project file listing request is invalid"));
    }

    Result<WorkspaceDirectory> directory = m_workspace->bind_directory(area_root(a_area), m_assertContext);
    if (!a_directory.empty())
    {
        Result<RelativePath> relative = RelativePath::parse(a_directory, m_assertContext);
        if (!relative)
        {
            return Result<ProjectFileDirectorySnapshot>::failure(reclassify_project_file_error(
                m_assertContext, ProjectFileError::InvalidRequest, "Project file directory locator is invalid",
                std::move(*relative.try_error())));
        }
        Result<BoundWorkspacePath> bound =
            m_workspace->bind_path(area_root(a_area), std::move(*relative.try_value()), m_assertContext);
        if (!bound)
        {
            return Result<ProjectFileDirectorySnapshot>::failure(reclassify_project_file_error(
                m_assertContext, ProjectFileError::InvalidRequest, "Project file directory could not be bound",
                std::move(*bound.try_error())));
        }
        directory =
            Result<WorkspaceDirectory>::success(WorkspaceDirectory::from_bound_path(std::move(*bound.try_value())));
    }
    if (!directory)
    {
        return Result<ProjectFileDirectorySnapshot>::failure(
            reclassify_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                          "Project file area could not be bound", std::move(*directory.try_error())));
    }

    Result<DirectorySnapshot> listed = m_workspace->list_directory(*directory.try_value(), a_limits);
    if (!listed)
    {
        const ProjectFileError classification =
            classify_project_file_error(*listed.try_error(), WorkspaceMutationOutcome::NotCommitted);
        return Result<ProjectFileDirectorySnapshot>::failure(reclassify_project_file_error(
            m_assertContext, classification, "Project file directory listing failed", std::move(*listed.try_error())));
    }

    ProjectFileDirectorySnapshot snapshot;
    snapshot.generation = listed.try_value()->generation;
    snapshot.state = listed.try_value()->state;
    try
    {
        snapshot.directory.assign(a_directory);
        snapshot.diagnostics = std::move(listed.try_value()->diagnostics);
        snapshot.entries.reserve(listed.try_value()->entries.size());
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    for (const WorkspaceEntry &entry : listed.try_value()->entries)
    {
        Result<ProjectFileEntry> converted = make_project_file_entry(entry, area_root(a_area).text(), m_assertContext);
        if (!converted)
        {
            return Result<ProjectFileDirectorySnapshot>::failure(std::move(*converted.try_error()));
        }
        try
        {
            snapshot.entries.push_back(std::move(*converted.try_value()));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
    }
    return Result<ProjectFileDirectorySnapshot>::success(std::move(snapshot));
}

Result<ProjectFileSearchResult> ProjectFileService::search(ProjectFileArea a_area, std::string_view a_directory,
                                                           std::string_view a_filter, TraversalLimits a_limits) noexcept
{
    if (std::this_thread::get_id() != m_ownerThread || m_isBusy || !m_policy.can_list(a_area) || !a_limits.is_valid() ||
        a_filter.empty())
    {
        return Result<ProjectFileSearchResult>::failure(make_project_file_error(
            m_assertContext, m_isBusy ? ProjectFileError::Busy : ProjectFileError::InvalidRequest,
            m_isBusy ? "Project file mutation is already active" : "Project file search request is invalid"));
    }

    Result<WorkspaceDirectory> directory = m_workspace->bind_directory(area_root(a_area), m_assertContext);
    if (!a_directory.empty())
    {
        Result<RelativePath> relative = RelativePath::parse(a_directory, m_assertContext);
        if (!relative)
        {
            return Result<ProjectFileSearchResult>::failure(reclassify_project_file_error(
                m_assertContext, ProjectFileError::InvalidRequest, "Project file search directory is invalid",
                std::move(*relative.try_error())));
        }
        Result<BoundWorkspacePath> bound =
            m_workspace->bind_path(area_root(a_area), std::move(*relative.try_value()), m_assertContext);
        if (!bound)
        {
            return Result<ProjectFileSearchResult>::failure(reclassify_project_file_error(
                m_assertContext, ProjectFileError::InvalidRequest, "Project file search directory could not be bound",
                std::move(*bound.try_error())));
        }
        directory =
            Result<WorkspaceDirectory>::success(WorkspaceDirectory::from_bound_path(std::move(*bound.try_value())));
    }
    if (!directory)
    {
        return Result<ProjectFileSearchResult>::failure(reclassify_project_file_error(
            m_assertContext, ProjectFileError::InvalidRequest, "Project file search area could not be bound",
            std::move(*directory.try_error())));
    }

    Result<WorkspaceSearchResult> searched =
        search_workspace(*m_workspace, *directory.try_value(), a_filter, a_limits, m_assertContext);
    if (!searched)
    {
        const ProjectFileError classification =
            classify_project_file_error(*searched.try_error(), WorkspaceMutationOutcome::NotCommitted);
        return Result<ProjectFileSearchResult>::failure(reclassify_project_file_error(
            m_assertContext, classification, "Project file search failed", std::move(*searched.try_error())));
    }

    ProjectFileSearchResult result;
    result.state = searched.try_value()->state;
    result.visitedEntries = searched.try_value()->visitedEntries;
    try
    {
        result.diagnostics = std::move(searched.try_value()->diagnostics);
        result.entries.reserve(searched.try_value()->entries.size());
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    for (const WorkspaceEntry &entry : searched.try_value()->entries)
    {
        Result<ProjectFileEntry> converted = make_project_file_entry(entry, area_root(a_area).text(), m_assertContext);
        if (!converted)
        {
            return Result<ProjectFileSearchResult>::failure(std::move(*converted.try_error()));
        }
        try
        {
            result.entries.push_back(std::move(*converted.try_value()));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
    }
    return Result<ProjectFileSearchResult>::success(std::move(result));
}

/// @brief 未検証Absolute PathをArea境界、親Chain、Entry種別に照らして再検証する
Result<RelativePath> ProjectFileService::revalidate_external_selection(ProjectFileArea a_area,
                                                                       std::string_view a_unverifiedAbsolutePath,
                                                                       ProjectFileSelectionPurpose a_purpose) noexcept
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        return Result<RelativePath>::failure(
            make_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                    "Project file selection was revalidated from another thread"));
    }
    if (m_isBusy)
    {
        return Result<RelativePath>::failure(make_project_file_error(m_assertContext, ProjectFileError::Busy,
                                                                     "Project file mutation is already active"));
    }
    const bool validPurpose = a_purpose == ProjectFileSelectionPurpose::OpenExistingFile ||
                              a_purpose == ProjectFileSelectionPurpose::SaveFileDestination ||
                              a_purpose == ProjectFileSelectionPurpose::SelectExistingDirectory;
    const bool allowedArea = a_purpose == ProjectFileSelectionPurpose::SaveFileDestination ? m_policy.can_mutate(a_area)
                                                                                           : m_policy.can_list(a_area);
    if (!validPurpose || !allowedArea)
    {
        return Result<RelativePath>::failure(make_project_file_error(
            m_assertContext, allowedArea ? ProjectFileError::InvalidRequest : ProjectFileError::ProtectedEntry,
            allowedArea ? "Project file selection purpose is invalid" : "Project file area is protected"));
    }

    Result<BoundWorkspacePath> bound = m_workspace->bind_external_path(a_unverifiedAbsolutePath);
    if (!bound)
    {
        const ProjectFileError classification =
            classify_project_file_error(*bound.try_error(), WorkspaceMutationOutcome::NotCommitted);
        return Result<RelativePath>::failure(reclassify_project_file_error(
            m_assertContext, classification, "Native File Dialog selection is outside the project workspace",
            std::move(*bound.try_error())));
    }

    const RelativePath &areaRoot = area_root(a_area);
    const std::string_view rootRelativeTextView = bound.try_value()->text();
    if (rootRelativeTextView.size() <= areaRoot.text().size() + 1U ||
        !rootRelativeTextView.starts_with(areaRoot.text()) || rootRelativeTextView[areaRoot.text().size()] != '/')
    {
        return Result<RelativePath>::failure(
            make_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                    "Native File Dialog selection is outside the requested project area"));
    }

    std::string rootRelativeText;
    std::string areaRelativeText;
    std::string parentText;
    std::string leafText;
    try
    {
        rootRelativeText.assign(bound.try_value()->text());
        areaRelativeText.assign(rootRelativeText.substr(areaRoot.text().size() + 1U));
        const std::size_t separator = areaRelativeText.rfind('/');
        if (separator != std::string::npos)
        {
            parentText.assign(areaRelativeText.substr(0U, separator));
            leafText.assign(areaRelativeText.substr(separator + 1U));
        }
        else
        {
            leafText.assign(areaRelativeText);
        }
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }

    Result<RelativePath> areaRelative = RelativePath::parse(areaRelativeText, m_assertContext);
    Result<RelativePath> leafLocator = RelativePath::parse(leafText, m_assertContext);
    if (!areaRelative || !leafLocator)
    {
        Error cause = !areaRelative ? std::move(*areaRelative.try_error()) : std::move(*leafLocator.try_error());
        return Result<RelativePath>::failure(reclassify_project_file_error(
            m_assertContext, ProjectFileError::InvalidRequest,
            "Native File Dialog selection is not a portable project path", std::move(cause)));
    }

    Result<WorkspaceDirectory> parentDirectory = m_workspace->bind_directory(areaRoot, m_assertContext);
    if (!parentText.empty())
    {
        Result<RelativePath> parentLocator = RelativePath::parse(parentText, m_assertContext);
        if (!parentLocator)
        {
            return Result<RelativePath>::failure(
                reclassify_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                              "Native File Dialog selection parent is not a portable area path",
                                              std::move(*parentLocator.try_error())));
        }
        Result<BoundWorkspacePath> boundParent =
            m_workspace->bind_path(areaRoot, std::move(*parentLocator.try_value()), m_assertContext);
        if (!boundParent)
        {
            const ProjectFileError classification =
                classify_project_file_error(*boundParent.try_error(), WorkspaceMutationOutcome::NotCommitted);
            return Result<RelativePath>::failure(reclassify_project_file_error(
                m_assertContext, classification, "Native File Dialog selection parent is not safe",
                std::move(*boundParent.try_error())));
        }
        parentDirectory = Result<WorkspaceDirectory>::success(
            WorkspaceDirectory::from_bound_path(std::move(*boundParent.try_value())));
    }
    if (!parentDirectory)
    {
        const ProjectFileError classification =
            classify_project_file_error(*parentDirectory.try_error(), WorkspaceMutationOutcome::NotCommitted);
        return Result<RelativePath>::failure(reclassify_project_file_error(
            m_assertContext, classification, "Native File Dialog selection parent is not safe",
            std::move(*parentDirectory.try_error())));
    }
    Result<DirectorySnapshot> snapshot =
        m_workspace->list_directory(*parentDirectory.try_value(), m_workspace->hard_limits());
    if (!snapshot)
    {
        const ProjectFileError classification =
            classify_project_file_error(*snapshot.try_error(), WorkspaceMutationOutcome::NotCommitted);
        return Result<RelativePath>::failure(reclassify_project_file_error(
            m_assertContext, classification, "Native File Dialog selection parent could not be inspected",
            std::move(*snapshot.try_error())));
    }
    if (snapshot.try_value()->state != WorkspaceSnapshotState::Complete)
    {
        return Result<RelativePath>::failure(
            make_project_file_error(m_assertContext, ProjectFileError::StorageFailure,
                                    "Native File Dialog selection parent changed while it was inspected"));
    }

    const std::string leafKey = leafLocator.try_value()->comparison_key(m_assertContext);
    const WorkspaceEntry *exactEntry = nullptr;
    std::size_t portableMatchCount = 0U;
    for (const WorkspaceEntry &entry : snapshot.try_value()->entries)
    {
        Result<RelativePath> entryName = RelativePath::parse(entry.displayName, m_assertContext);
        if (!entryName)
        {
            if (entry.displayName == leafText)
            {
                return Result<RelativePath>::failure(
                    make_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                            "Native File Dialog selection names an unsupported workspace entry"));
            }
            continue;
        }
        if (entryName.try_value()->comparison_key(m_assertContext) == leafKey)
        {
            ++portableMatchCount;
            if (entry.displayName == leafText)
            {
                exactEntry = &entry;
            }
        }
    }

    if (exactEntry == nullptr)
    {
        if (portableMatchCount != 0U || a_purpose != ProjectFileSelectionPurpose::SaveFileDestination)
        {
            return Result<RelativePath>::failure(make_project_file_error(
                m_assertContext, ProjectFileError::InvalidRequest,
                portableMatchCount != 0U ? "Native File Dialog selection uses a non-canonical path spelling"
                                         : "Native File Dialog selection no longer exists"));
        }
        return Result<RelativePath>::success(std::move(*areaRelative.try_value()));
    }
    if (portableMatchCount != 1U)
    {
        return Result<RelativePath>::failure(
            make_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                    "Native File Dialog selection collides with another portable path spelling"));
    }
    if (!exactEntry->is_operable())
    {
        return Result<RelativePath>::failure(
            make_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                    "Native File Dialog selection is a reparse point or unsupported entry"));
    }

    const bool validType = (a_purpose == ProjectFileSelectionPurpose::OpenExistingFile &&
                            exactEntry->type == WorkspaceEntryType::RegularFile) ||
                           (a_purpose == ProjectFileSelectionPurpose::SaveFileDestination &&
                            exactEntry->type == WorkspaceEntryType::RegularFile) ||
                           (a_purpose == ProjectFileSelectionPurpose::SelectExistingDirectory &&
                            exactEntry->type == WorkspaceEntryType::Directory);
    if (!validType)
    {
        return Result<RelativePath>::failure(
            make_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                    "Native File Dialog selection has the wrong workspace entry type"));
    }
    return Result<RelativePath>::success(std::move(*areaRelative.try_value()));
}

std::span<const RecoveryEntry> ProjectFileService::recovery_entries() const noexcept
{
    return m_recoveryEntries;
}

std::span<const Error> ProjectFileService::recovery_diagnostics() const noexcept
{
    return m_recoveryDiagnostics;
}

WorkspaceMutationResult ProjectFileService::ensure_trash_root(std::string_view a_operationId) noexcept
{
    WorkspaceMutationResult aggregate;
    aggregate.outcome = WorkspaceMutationOutcome::Committed;
    /// @brief Commit済みStageの診断を発生順にAggregateへ移送する
    const auto captureCommitted = [&](WorkspaceMutationResult &a_mutation) noexcept
    {
        try
        {
            if (a_mutation.primaryError.has_value())
            {
                if (!aggregate.primaryError.has_value())
                {
                    aggregate.primaryError = std::move(*a_mutation.primaryError);
                }
                else
                {
                    aggregate.secondaryDiagnostics.push_back(std::move(*a_mutation.primaryError));
                }
            }
            for (Error &diagnostic : a_mutation.secondaryDiagnostics)
            {
                aggregate.secondaryDiagnostics.push_back(std::move(diagnostic));
            }
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        if (a_mutation.outcome == WorkspaceMutationOutcome::CommittedButDurabilityUnknown)
        {
            aggregate.outcome = WorkspaceMutationOutcome::CommittedButDurabilityUnknown;
        }
    };
    /// @brief 現在Stageの失敗をPrimaryに保ち、先行Commit診断をSecondaryへ移送する
    const auto fail = [&](WorkspaceMutationOutcome a_outcome, Error a_primary,
                          std::vector<Error> a_secondary = {}) noexcept -> WorkspaceMutationResult
    {
        try
        {
            if (aggregate.primaryError.has_value())
            {
                a_secondary.push_back(std::move(*aggregate.primaryError));
            }
            for (Error &diagnostic : aggregate.secondaryDiagnostics)
            {
                a_secondary.push_back(std::move(diagnostic));
            }
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        WorkspaceMutationResult failed;
        failed.outcome = a_outcome;
        failed.primaryError = std::move(a_primary);
        failed.secondaryDiagnostics = std::move(a_secondary);
        return failed;
    };

    constexpr std::array<std::string_view, 2U> k_directories{"Editor", "Editor/Trash"};
    for (const std::string_view text : k_directories)
    {
        Result<RelativePath> relative = RelativePath::parse(text, m_assertContext);
        if (!relative)
        {
            return fail(WorkspaceMutationOutcome::NotCommitted, std::move(*relative.try_error()));
        }
        Result<BoundWorkspacePath> bound =
            m_workspace->bind_path(m_roots.saved(), std::move(*relative.try_value()), m_assertContext);
        if (!bound)
        {
            return fail(WorkspaceMutationOutcome::NotCommitted, std::move(*bound.try_error()));
        }
        WorkspaceMutationResult created = m_workspace->create_directory_new(*bound.try_value(), a_operationId);
        const bool alreadyExists =
            created.primaryError.has_value() && is_io_error(*created.primaryError, IoError::AlreadyExists);
        if (created.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
        {
            Error primary = created.primaryError.has_value()
                                ? std::move(*created.primaryError)
                                : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                          "Project trash directory creation requires reconciliation");
            return fail(created.outcome, std::move(primary), std::move(created.secondaryDiagnostics));
        }
        if (created.outcome == WorkspaceMutationOutcome::NotCommitted && !alreadyExists)
        {
            Error primary = created.primaryError.has_value()
                                ? std::move(*created.primaryError)
                                : make_project_file_error(m_assertContext, ProjectFileError::StorageFailure,
                                                          "Project trash directory creation failed");
            return fail(created.outcome, std::move(primary), std::move(created.secondaryDiagnostics));
        }
        if (created.outcome != WorkspaceMutationOutcome::NotCommitted)
        {
            captureCommitted(created);
        }
        WorkspaceDirectory directory = WorkspaceDirectory::from_bound_path(std::move(*bound.try_value()));
        Result<void> verified = m_workspace->verify_directory(directory);
        if (!verified)
        {
            return fail(WorkspaceMutationOutcome::ReconciliationRequired, std::move(*verified.try_error()));
        }
    }
    return aggregate;
}

Result<std::vector<std::byte>> ProjectFileService::read_trash_record_bytes(std::string_view a_operationId) noexcept
{
    std::string path;
    try
    {
        path = "Editor/Trash/";
        path.append(a_operationId);
        path.append("/Record.cuetrash");
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    Result<RelativePath> relative = RelativePath::parse(path, m_assertContext);
    if (!relative)
    {
        return Result<std::vector<std::byte>>::failure(std::move(*relative.try_error()));
    }
    Result<BoundWorkspacePath> bound =
        m_workspace->bind_path(m_roots.saved(), std::move(*relative.try_value()), m_assertContext);
    if (!bound)
    {
        return Result<std::vector<std::byte>>::failure(std::move(*bound.try_error()));
    }
    return m_workspace->read_file_bounded(*bound.try_value(), k_maximumTrashRecordBytes);
}

Result<ProjectFileOperationResult> ProjectFileService::create_directory(ProjectFileArea a_area,
                                                                        RelativePath a_destination) noexcept
{
    return execute_create(ProjectFileOperationKind::DirectoryCreation, a_area, std::move(a_destination), {});
}

Result<ProjectFileOperationResult> ProjectFileService::create_file(ProjectFileArea a_area, RelativePath a_destination,
                                                                   std::span<const std::byte> a_bytes) noexcept
{
    return execute_create(ProjectFileOperationKind::FileCreation, a_area, std::move(a_destination), a_bytes);
}

Result<ProjectFileOperationResult> ProjectFileService::rename(ProjectFileArea a_area, RelativePath a_source,
                                                              RelativePath a_destination,
                                                              TraversalLimits a_limits) noexcept
{
    return execute_transfer(ProjectFileOperationKind::Rename, a_area, std::move(a_source), std::move(a_destination),
                            a_limits, ContentVerificationLimits{1U, 1U});
}

Result<ProjectFileOperationResult> ProjectFileService::move(ProjectFileArea a_area, RelativePath a_source,
                                                            RelativePath a_destination,
                                                            TraversalLimits a_limits) noexcept
{
    return execute_transfer(ProjectFileOperationKind::Move, a_area, std::move(a_source), std::move(a_destination),
                            a_limits, ContentVerificationLimits{1U, 1U});
}

Result<ProjectFileOperationResult> ProjectFileService::copy(ProjectFileArea a_area, RelativePath a_source,
                                                            RelativePath a_destination,
                                                            TraversalLimits a_traversalLimits,
                                                            ContentVerificationLimits a_contentLimits) noexcept
{
    return execute_transfer(ProjectFileOperationKind::Copy, a_area, std::move(a_source), std::move(a_destination),
                            a_traversalLimits, a_contentLimits);
}

Result<ProjectFileOperationResult> ProjectFileService::delete_entry(ProjectFileArea a_area, RelativePath a_source,
                                                                    TraversalLimits a_traversalLimits,
                                                                    ContentVerificationLimits a_contentLimits) noexcept
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        return Result<ProjectFileOperationResult>::failure(make_project_file_error(
            m_assertContext, ProjectFileError::InvalidRequest, "Project file service was called from another thread"));
    }
    if (m_isBusy)
    {
        return Result<ProjectFileOperationResult>::failure(make_project_file_error(
            m_assertContext, ProjectFileError::Busy, "Project file mutation is already active"));
    }
    BusyReset busy(m_isBusy);

    Result<std::string> generatedId = m_operationIdSource->next_operation_id();
    if (!generatedId)
    {
        return Result<ProjectFileOperationResult>::failure(reclassify_project_file_error(
            m_assertContext, ProjectFileError::InvalidRequest, "Project file operation id generation failed",
            std::move(*generatedId.try_error())));
    }
    Result<ProjectId> validatedId = ProjectId::parse(*generatedId.try_value(), m_assertContext);
    if (!validatedId)
    {
        return Result<ProjectFileOperationResult>::failure(
            reclassify_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                          "Project file operation id is invalid", std::move(*validatedId.try_error())));
    }
    std::string operationId = std::move(*generatedId.try_value());
    std::string source;
    try
    {
        source.assign(a_source.text());
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    std::vector<Error> committedDiagnostics;

    /// @brief Delete経路の共通Metadataを保持したOperation Resultを構築する
    const auto makeResult = [&](ProjectFileOperationStage a_stage, ProjectFileOperationOutcome a_outcome,
                                std::optional<Error> a_primary,
                                std::vector<Error> a_secondary) noexcept -> Result<ProjectFileOperationResult>
    {
        std::vector<std::string> rescan;
        try
        {
            if (a_outcome == ProjectFileOperationOutcome::CommittedButDurabilityUnknown &&
                !committedDiagnostics.empty())
            {
                a_primary = std::move(committedDiagnostics.front());
                for (std::size_t index = 1U; index < committedDiagnostics.size(); ++index)
                {
                    a_secondary.push_back(std::move(committedDiagnostics[index]));
                }
            }
            else
            {
                for (Error &diagnostic : committedDiagnostics)
                {
                    a_secondary.push_back(std::move(diagnostic));
                }
            }
            rescan.push_back(parent_locator(source, m_assertContext));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        ProjectFileOperationResult result(operationId, ProjectFileOperationKind::RecoverableDelete, a_area, source,
                                          operationId, a_stage, a_outcome, std::move(a_primary), std::move(a_secondary),
                                          std::move(rescan));
        return Result<ProjectFileOperationResult>::success(std::move(result));
    };
    /// @brief Delete失敗CauseをProject File Errorへ再分類してOperation Resultへ保持する
    const auto fail = [&](ProjectFileOperationStage a_stage, ProjectFileError a_code, std::string_view a_message,
                          Error a_cause, ProjectFileOperationOutcome a_outcome,
                          std::vector<Error> a_secondary = {}) noexcept -> Result<ProjectFileOperationResult>
    {
        std::optional<Error> primary(
            reclassify_project_file_error(m_assertContext, a_code, a_message, std::move(a_cause)));
        return makeResult(a_stage, a_outcome, std::move(primary), std::move(a_secondary));
    };
    /// @brief Commit済みStageのPrimaryとSecondary診断を最終Operation Resultへ移送する
    const auto captureCommittedMutation = [&](WorkspaceMutationResult &a_mutation) noexcept
    {
        try
        {
            if (a_mutation.primaryError.has_value())
            {
                committedDiagnostics.push_back(std::move(*a_mutation.primaryError));
            }
            for (Error &diagnostic : a_mutation.secondaryDiagnostics)
            {
                committedDiagnostics.push_back(std::move(diagnostic));
            }
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
    };

    if (!m_policy.can_mutate(a_area) || !a_traversalLimits.is_valid() || !a_contentLimits.is_valid())
    {
        std::optional<Error> primary(make_project_file_error(
            m_assertContext,
            !m_policy.can_mutate(a_area) ? ProjectFileError::ProtectedEntry : ProjectFileError::InvalidRequest,
            !m_policy.can_mutate(a_area) ? "Project file area is protected from deletion"
                                         : "Project file delete limits are invalid"));
        return makeResult(ProjectFileOperationStage::ValidateRequest, ProjectFileOperationOutcome::NotCommitted,
                          std::move(primary), {});
    }
    Result<BoundWorkspacePath> boundSource = m_workspace->bind_path(area_root(a_area), a_source, m_assertContext);
    if (!boundSource)
    {
        const ProjectFileError classification =
            classify_project_file_error(*boundSource.try_error(), WorkspaceMutationOutcome::NotCommitted);
        Error cause = std::move(*boundSource.try_error());
        return fail(ProjectFileOperationStage::BindSource, classification, "Project file delete source binding failed",
                    std::move(cause), ProjectFileOperationOutcome::NotCommitted);
    }
    Result<GuardedWorkspaceEntry> guardedSource =
        m_workspace->guard_entry(*boundSource.try_value(), a_traversalLimits, a_contentLimits);
    if (!guardedSource)
    {
        const ProjectFileError classification =
            classify_project_file_error(*guardedSource.try_error(), WorkspaceMutationOutcome::NotCommitted);
        Error cause = std::move(*guardedSource.try_error());
        return fail(ProjectFileOperationStage::ValidateRequest, classification,
                    "Project file delete source verification failed", std::move(cause),
                    ProjectFileOperationOutcome::NotCommitted);
    }
    /// @brief Delete失敗時にSource Guardを確定し、変更検出診断を保持する
    const auto finishFailureGuard = [&](std::vector<Error> &a_secondary) noexcept -> bool
    {
        Result<void> finished = m_workspace->finish_entry_mutation_guard(std::move(guardedSource.try_value()->guard));
        if (finished)
        {
            return true;
        }
        try
        {
            a_secondary.push_back(std::move(*finished.try_error()));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        return false;
    };
    WorkspaceMutationResult trashRoot = ensure_trash_root(operationId);
    if (trashRoot.outcome == WorkspaceMutationOutcome::NotCommitted ||
        trashRoot.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        Error cause = trashRoot.primaryError.has_value()
                          ? std::move(*trashRoot.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::StorageFailure,
                                                    "Project trash root preparation failed");
        const ProjectFileError classification = classify_project_file_error(cause, trashRoot.outcome);
        std::vector<Error> secondary = std::move(trashRoot.secondaryDiagnostics);
        const bool guardFinished = finishFailureGuard(secondary);
        return fail(ProjectFileOperationStage::PrepareRecovery, classification, "Project trash root preparation failed",
                    std::move(cause),
                    guardFinished ? convert_outcome(trashRoot.outcome)
                                  : ProjectFileOperationOutcome::ReconciliationRequired,
                    std::move(secondary));
    }
    captureCommittedMutation(trashRoot);

    project_files_private::TrashRecord record;
    try
    {
        record.projectId.assign(m_projectId.text());
        record.operationId = operationId;
        record.originalPath = source;
        record.fingerprint = guardedSource.try_value()->fingerprint;
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    /// @brief 同じTrash Recordを指定状態のCanonical Byte列へ変換する
    const auto serializeState =
        [&](project_files_private::TrashRecordState a_state) noexcept -> Result<std::vector<std::byte>>
    {
        record.state = a_state;
        return project_files_private::serialize_trash_record(record, m_assertContext);
    };
    Result<std::vector<std::byte>> allocating = serializeState(project_files_private::TrashRecordState::Allocating);
    Result<std::vector<std::byte>> prepared = serializeState(project_files_private::TrashRecordState::Prepared);
    Result<std::vector<std::byte>> trashed = serializeState(project_files_private::TrashRecordState::Trashed);
    Result<std::vector<std::byte>> aborting = serializeState(project_files_private::TrashRecordState::Aborting);
    Result<std::vector<std::byte>> aborted = serializeState(project_files_private::TrashRecordState::Aborted);
    if (!allocating || !prepared || !trashed || !aborting || !aborted)
    {
        Error cause = !allocating ? std::move(*allocating.try_error())
                      : !prepared ? std::move(*prepared.try_error())
                      : !trashed  ? std::move(*trashed.try_error())
                      : !aborting ? std::move(*aborting.try_error())
                                  : std::move(*aborted.try_error());
        std::vector<Error> secondary;
        const bool guardFinished = finishFailureGuard(secondary);
        return fail(ProjectFileOperationStage::PrepareRecovery, ProjectFileError::RecoveryRequired,
                    "Project trash record serialization failed", std::move(cause),
                    guardFinished ? ProjectFileOperationOutcome::NotCommitted
                                  : ProjectFileOperationOutcome::ReconciliationRequired,
                    std::move(secondary));
    }

    /// @brief Saved Root内の内部Recovery PathをCapabilityへ変換する
    const auto bindSaved = [&](std::string_view a_path) noexcept -> Result<BoundWorkspacePath>
    {
        Result<RelativePath> relative = RelativePath::parse(a_path, m_assertContext);
        if (!relative)
        {
            return Result<BoundWorkspacePath>::failure(std::move(*relative.try_error()));
        }
        return m_workspace->bind_path(m_roots.saved(), std::move(*relative.try_value()), m_assertContext);
    };
    std::string operationPath;
    std::string recordPath;
    std::string payloadPath;
    try
    {
        operationPath = "Editor/Trash/" + operationId;
        recordPath = operationPath + "/Record.cuetrash";
        payloadPath = operationPath + "/Payload";
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    Result<BoundWorkspacePath> trashParent = bindSaved("Editor/Trash");
    Result<BoundWorkspacePath> staging =
        trashParent ? m_workspace->bind_operation_staging_path(*trashParent.try_value(), operationId, "cuetrash",
                                                               m_assertContext)
                    : Result<BoundWorkspacePath>::failure(std::move(*trashParent.try_error()));
    Result<BoundWorkspacePath> operation = bindSaved(operationPath);
    Result<RelativePath> recordChild = RelativePath::parse("Record.cuetrash", m_assertContext);
    Result<BoundWorkspacePath> stagingRecord =
        staging && recordChild
            ? m_workspace->bind_child_path(*staging.try_value(), std::move(*recordChild.try_value()), m_assertContext)
            : Result<BoundWorkspacePath>::failure(
                  !staging ? make_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                                     "Project trash staging path is unavailable")
                           : std::move(*recordChild.try_error()));
    Result<BoundWorkspacePath> finalRecord = bindSaved(recordPath);
    Result<BoundWorkspacePath> payload = bindSaved(payloadPath);
    if (!staging || !operation || !stagingRecord || !finalRecord || !payload)
    {
        Error cause = !staging         ? std::move(*staging.try_error())
                      : !operation     ? std::move(*operation.try_error())
                      : !stagingRecord ? std::move(*stagingRecord.try_error())
                      : !finalRecord   ? std::move(*finalRecord.try_error())
                                       : std::move(*payload.try_error());
        std::vector<Error> secondary;
        const bool guardFinished = finishFailureGuard(secondary);
        return fail(ProjectFileOperationStage::PrepareRecovery, ProjectFileError::InvalidRequest,
                    "Project trash path binding failed", std::move(cause),
                    guardFinished ? ProjectFileOperationOutcome::NotCommitted
                                  : ProjectFileOperationOutcome::ReconciliationRequired,
                    std::move(secondary));
    }

    /// @brief 下位MutationのSecondary DiagnosticsをProject Operationへ移送する
    const auto appendMutationDiagnostics =
        [&](WorkspaceMutationResult &a_mutation, std::vector<Error> &a_secondary) noexcept
    {
        try
        {
            for (Error &diagnostic : a_mutation.secondaryDiagnostics)
            {
                a_secondary.push_back(std::move(diagnostic));
            }
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
    };
    /// @brief Payload公開前のRecordと空ContainerだけをCleanupして診断を保持する
    const auto cleanupContainer = [&](const BoundWorkspacePath &a_recordPath, const BoundWorkspacePath &a_directoryPath,
                                      std::vector<Error> &a_secondary) noexcept
    {
        WorkspaceMutationResult removedRecord = m_workspace->remove_file_or_empty_directory(a_recordPath);
        appendMutationDiagnostics(removedRecord, a_secondary);
        if (removedRecord.primaryError.has_value())
        {
            try
            {
                a_secondary.push_back(std::move(*removedRecord.primaryError));
            }
            catch (...)
            {
                terminate_allocation(m_assertContext);
            }
        }
        if (removedRecord.outcome == WorkspaceMutationOutcome::NotCommitted ||
            removedRecord.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
        {
            return;
        }
        WorkspaceMutationResult removedDirectory = m_workspace->remove_file_or_empty_directory(a_directoryPath);
        appendMutationDiagnostics(removedDirectory, a_secondary);
        if (removedDirectory.primaryError.has_value())
        {
            try
            {
                a_secondary.push_back(std::move(*removedDirectory.primaryError));
            }
            catch (...)
            {
                terminate_allocation(m_assertContext);
            }
        }
    };
    /// @brief Mutationの全診断をDelete失敗結果のSecondaryへ移送する
    const auto appendMutationOutcomeDiagnostics =
        [&](WorkspaceMutationResult &a_mutation, std::vector<Error> &a_secondary) noexcept
    {
        appendMutationDiagnostics(a_mutation, a_secondary);
        if (a_mutation.primaryError.has_value())
        {
            try
            {
                a_secondary.push_back(std::move(*a_mutation.primaryError));
            }
            catch (...)
            {
                terminate_allocation(m_assertContext);
            }
        }
    };
    /// @brief 既存Recovery RecordをGuard付き中間状態から終端状態へ確定してCleanupする
    const auto abortAndCleanupContainer = [&](const BoundWorkspacePath &a_recordPath,
                                              const BoundWorkspacePath &a_directoryPath,
                                              std::vector<Error> &a_secondary) noexcept -> bool
    {
        WorkspaceMutationResult wroteAborting =
            m_workspace->replace_file_atomic(a_recordPath, *aborting.try_value(), operationId);
        appendMutationOutcomeDiagnostics(wroteAborting, a_secondary);
        if (wroteAborting.outcome == WorkspaceMutationOutcome::NotCommitted ||
            wroteAborting.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
        {
            static_cast<void>(finishFailureGuard(a_secondary));
            return false;
        }

        if (!finishFailureGuard(a_secondary))
        {
            return false;
        }

        WorkspaceMutationResult wroteAborted =
            m_workspace->replace_file_atomic(a_recordPath, *aborted.try_value(), operationId);
        appendMutationOutcomeDiagnostics(wroteAborted, a_secondary);
        if (wroteAborted.outcome == WorkspaceMutationOutcome::NotCommitted ||
            wroteAborted.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
        {
            return false;
        }
        cleanupContainer(a_recordPath, a_directoryPath, a_secondary);
        return true;
    };

    WorkspaceMutationResult createdDirectory = m_workspace->create_directory_new(*staging.try_value(), operationId);
    if (createdDirectory.outcome == WorkspaceMutationOutcome::NotCommitted ||
        createdDirectory.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        Error cause = createdDirectory.primaryError.has_value()
                          ? std::move(*createdDirectory.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::StorageFailure,
                                                    "Project trash staging creation failed");
        const ProjectFileError classification = classify_project_file_error(cause, createdDirectory.outcome);
        std::vector<Error> secondary = std::move(createdDirectory.secondaryDiagnostics);
        const bool guardFinished = finishFailureGuard(secondary);
        return fail(ProjectFileOperationStage::PrepareRecovery, classification, "Project trash staging creation failed",
                    std::move(cause),
                    guardFinished ? convert_outcome(createdDirectory.outcome)
                                  : ProjectFileOperationOutcome::ReconciliationRequired,
                    std::move(secondary));
    }
    captureCommittedMutation(createdDirectory);
    WorkspaceMutationResult wroteAllocating =
        m_workspace->create_file_new_atomic(*stagingRecord.try_value(), *allocating.try_value(), operationId);
    if (wroteAllocating.outcome == WorkspaceMutationOutcome::NotCommitted ||
        wroteAllocating.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        std::vector<Error> secondary = std::move(wroteAllocating.secondaryDiagnostics);
        const bool guardFinished = finishFailureGuard(secondary);
        if (guardFinished && wroteAllocating.outcome == WorkspaceMutationOutcome::NotCommitted)
        {
            cleanupContainer(*stagingRecord.try_value(), *staging.try_value(), secondary);
        }
        Error cause = wroteAllocating.primaryError.has_value()
                          ? std::move(*wroteAllocating.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::StorageFailure,
                                                    "Project trash allocating record write failed");
        const ProjectFileError classification = classify_project_file_error(cause, wroteAllocating.outcome);
        return fail(ProjectFileOperationStage::PrepareRecovery, classification,
                    "Project trash allocating record write failed", std::move(cause),
                    guardFinished ? convert_outcome(wroteAllocating.outcome)
                                  : ProjectFileOperationOutcome::ReconciliationRequired,
                    std::move(secondary));
    }
    captureCommittedMutation(wroteAllocating);
    WorkspaceMutationResult publishedContainer =
        m_workspace->rename_entry(*staging.try_value(), *operation.try_value(), a_traversalLimits);
    if (publishedContainer.outcome == WorkspaceMutationOutcome::NotCommitted ||
        publishedContainer.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        Error cause = publishedContainer.primaryError.has_value()
                          ? std::move(*publishedContainer.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                    "Project trash container publish requires reconciliation");
        const ProjectFileError classification = classify_project_file_error(cause, publishedContainer.outcome);
        std::vector<Error> secondary = std::move(publishedContainer.secondaryDiagnostics);
        const bool guardFinished = finishFailureGuard(secondary);
        return fail(ProjectFileOperationStage::PrepareRecovery, classification,
                    "Project trash container publish failed", std::move(cause),
                    guardFinished ? convert_outcome(publishedContainer.outcome)
                                  : ProjectFileOperationOutcome::ReconciliationRequired,
                    std::move(secondary));
    }
    captureCommittedMutation(publishedContainer);
    WorkspaceMutationResult wrotePrepared =
        m_workspace->replace_file_atomic(*finalRecord.try_value(), *prepared.try_value(), operationId);
    if (wrotePrepared.outcome == WorkspaceMutationOutcome::NotCommitted ||
        wrotePrepared.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        std::vector<Error> secondary = std::move(wrotePrepared.secondaryDiagnostics);
        bool safelyAborted = false;
        if (wrotePrepared.outcome == WorkspaceMutationOutcome::NotCommitted)
        {
            safelyAborted = abortAndCleanupContainer(*finalRecord.try_value(), *operation.try_value(), secondary);
        }
        else
        {
            static_cast<void>(finishFailureGuard(secondary));
        }
        Error cause = wrotePrepared.primaryError.has_value()
                          ? std::move(*wrotePrepared.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                    "Project trash prepared record write failed");
        const ProjectFileError classification = classify_project_file_error(cause, wrotePrepared.outcome);
        return fail(ProjectFileOperationStage::UpdateRecoveryRecord, classification,
                    "Project trash prepared record write failed", std::move(cause),
                    safelyAborted ? ProjectFileOperationOutcome::NotCommitted
                                  : ProjectFileOperationOutcome::ReconciliationRequired,
                    std::move(secondary));
    }
    captureCommittedMutation(wrotePrepared);

    WorkspaceMutationResult moved = m_workspace->rename_guarded_entry(*guardedSource.try_value()->guard,
                                                                      *boundSource.try_value(), *payload.try_value());
    if (moved.outcome == WorkspaceMutationOutcome::NotCommitted)
    {
        std::vector<Error> secondary = std::move(moved.secondaryDiagnostics);
        const bool safelyAborted =
            abortAndCleanupContainer(*finalRecord.try_value(), *operation.try_value(), secondary);
        Error cause = moved.primaryError.has_value()
                          ? std::move(*moved.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::StorageFailure,
                                                    "Project file delete rename failed");
        const ProjectFileError classification = classify_project_file_error(cause, moved.outcome);
        return fail(ProjectFileOperationStage::NativePublish, classification, "Project file delete rename failed",
                    std::move(cause),
                    safelyAborted ? ProjectFileOperationOutcome::NotCommitted
                                  : ProjectFileOperationOutcome::ReconciliationRequired,
                    std::move(secondary));
    }
    if (moved.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        Error cause = moved.primaryError.has_value()
                          ? std::move(*moved.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                    "Project file delete rename requires reconciliation");
        std::vector<Error> secondary = std::move(moved.secondaryDiagnostics);
        static_cast<void>(finishFailureGuard(secondary));
        return fail(ProjectFileOperationStage::Verify, ProjectFileError::RecoveryRequired,
                    "Project file delete rename requires reconciliation", std::move(cause),
                    ProjectFileOperationOutcome::ReconciliationRequired, std::move(secondary));
    }
    captureCommittedMutation(moved);
    WorkspaceMutationResult wroteTrashed =
        m_workspace->replace_file_atomic(*finalRecord.try_value(), *trashed.try_value(), operationId);
    if (wroteTrashed.outcome == WorkspaceMutationOutcome::NotCommitted ||
        wroteTrashed.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        Error cause = wroteTrashed.primaryError.has_value()
                          ? std::move(*wroteTrashed.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                    "Project trash final record write requires reconciliation");
        std::vector<Error> secondary = std::move(wroteTrashed.secondaryDiagnostics);
        static_cast<void>(finishFailureGuard(secondary));
        return fail(ProjectFileOperationStage::UpdateRecoveryRecord, ProjectFileError::RecoveryRequired,
                    "Project trash final record write failed", std::move(cause),
                    ProjectFileOperationOutcome::ReconciliationRequired, std::move(secondary));
    }
    captureCommittedMutation(wroteTrashed);

    Result<void> guardFinished = m_workspace->finish_entry_mutation_guard(std::move(guardedSource.try_value()->guard));
    if (!guardFinished)
    {
        return fail(ProjectFileOperationStage::Verify, ProjectFileError::RecoveryRequired,
                    "Project file delete guard detected a concurrent change", std::move(*guardFinished.try_error()),
                    ProjectFileOperationOutcome::ReconciliationRequired);
    }

    RecoveryEntry recovery;
    try
    {
        recovery.operationId = operationId;
        recovery.originalPath = source;
        recovery.originalArea = a_area;
        recovery.entryType = guardedSource.try_value()->fingerprint.type;
        recovery.byteSize = fingerprint_byte_size(guardedSource.try_value()->fingerprint);
        recovery.descendantCount = guardedSource.try_value()->fingerprint.manifest.size();
        recovery.state = RecoveryEntryState::Recoverable;
        m_recoveryEntries.push_back(std::move(recovery));
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    std::optional<Error> primary(
        make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                "Project file delete is visible but one or more durability barriers are unknown"));
    return makeResult(ProjectFileOperationStage::Verify, ProjectFileOperationOutcome::CommittedButDurabilityUnknown,
                      std::move(primary), {});
}

Result<ProjectFileOperationResult> ProjectFileService::restore(std::string_view a_operationId,
                                                               TraversalLimits a_traversalLimits,
                                                               ContentVerificationLimits a_contentLimits) noexcept
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        return Result<ProjectFileOperationResult>::failure(make_project_file_error(
            m_assertContext, ProjectFileError::InvalidRequest, "Project file service was called from another thread"));
    }
    if (m_isBusy)
    {
        return Result<ProjectFileOperationResult>::failure(make_project_file_error(
            m_assertContext, ProjectFileError::Busy, "Project file mutation is already active"));
    }
    BusyReset busy(m_isBusy);
    Result<ProjectId> validatedId = ProjectId::parse(a_operationId, m_assertContext);
    if (!validatedId || !a_traversalLimits.is_valid() || !a_contentLimits.is_valid())
    {
        return Result<ProjectFileOperationResult>::failure(
            !validatedId ? reclassify_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                                         "Project restore operation id is invalid",
                                                         std::move(*validatedId.try_error()))
                         : make_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                                   "Project restore limits are invalid"));
    }
    std::string operationId;
    try
    {
        operationId.assign(a_operationId);
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }

    Result<std::vector<std::byte>> recordBytes = read_trash_record_bytes(operationId);
    if (!recordBytes)
    {
        return Result<ProjectFileOperationResult>::failure(reclassify_project_file_error(
            m_assertContext, ProjectFileError::RecoveryRequired, "Project trash record could not be read",
            std::move(*recordBytes.try_error())));
    }
    Result<project_files_private::TrashRecord> record = project_files_private::parse_trash_record(
        *recordBytes.try_value(), project_files_private::trash_record_hard_limits(), m_assertContext);
    if (!record || record.try_value()->projectId != m_projectId.text() ||
        record.try_value()->operationId != operationId ||
        record.try_value()->state != project_files_private::TrashRecordState::Trashed)
    {
        return Result<ProjectFileOperationResult>::failure(
            !record ? std::move(*record.try_error())
                    : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                              "Project trash record is not a recoverable entry"));
    }
    Result<RelativePath> original = RelativePath::parse(record.try_value()->originalPath, m_assertContext);
    if (!original)
    {
        return Result<ProjectFileOperationResult>::failure(std::move(*original.try_error()));
    }
    std::string destination = record.try_value()->originalPath;
    std::vector<Error> committedDiagnostics;

    /// @brief Restore経路の共通Metadataを保持したOperation Resultを構築する
    const auto makeResult = [&](ProjectFileOperationStage a_stage, ProjectFileOperationOutcome a_outcome,
                                std::optional<Error> a_primary,
                                std::vector<Error> a_secondary) noexcept -> Result<ProjectFileOperationResult>
    {
        std::vector<std::string> rescan;
        try
        {
            if (a_outcome == ProjectFileOperationOutcome::CommittedButDurabilityUnknown &&
                !committedDiagnostics.empty())
            {
                a_primary = std::move(committedDiagnostics.front());
                for (std::size_t index = 1U; index < committedDiagnostics.size(); ++index)
                {
                    a_secondary.push_back(std::move(committedDiagnostics[index]));
                }
            }
            else
            {
                for (Error &diagnostic : committedDiagnostics)
                {
                    a_secondary.push_back(std::move(diagnostic));
                }
            }
            rescan.push_back(parent_locator(destination, m_assertContext));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        ProjectFileOperationResult result(operationId, ProjectFileOperationKind::Restore, ProjectFileArea::SourceAssets,
                                          operationId, destination, a_stage, a_outcome, std::move(a_primary),
                                          std::move(a_secondary), std::move(rescan));
        return Result<ProjectFileOperationResult>::success(std::move(result));
    };
    /// @brief Restore失敗CauseをProject File Errorへ再分類してOperation Resultへ保持する
    const auto fail = [&](ProjectFileOperationStage a_stage, ProjectFileError a_code, std::string_view a_message,
                          Error a_cause, ProjectFileOperationOutcome a_outcome,
                          std::vector<Error> a_secondary = {}) noexcept -> Result<ProjectFileOperationResult>
    {
        std::optional<Error> primary(
            reclassify_project_file_error(m_assertContext, a_code, a_message, std::move(a_cause)));
        return makeResult(a_stage, a_outcome, std::move(primary), std::move(a_secondary));
    };
    /// @brief Commit済みRestore StageのPrimaryとSecondary診断を最終Resultへ移送する
    const auto captureCommittedMutation = [&](WorkspaceMutationResult &a_mutation) noexcept
    {
        try
        {
            if (a_mutation.primaryError.has_value())
            {
                committedDiagnostics.push_back(std::move(*a_mutation.primaryError));
            }
            for (Error &diagnostic : a_mutation.secondaryDiagnostics)
            {
                committedDiagnostics.push_back(std::move(diagnostic));
            }
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
    };
    /// @brief Saved Root内のRecovery Entry PathをCapabilityへ変換する
    const auto bindSaved = [&](std::string_view a_path) noexcept -> Result<BoundWorkspacePath>
    {
        Result<RelativePath> relative = RelativePath::parse(a_path, m_assertContext);
        if (!relative)
        {
            return Result<BoundWorkspacePath>::failure(std::move(*relative.try_error()));
        }
        return m_workspace->bind_path(m_roots.saved(), std::move(*relative.try_value()), m_assertContext);
    };

    std::string operationPath;
    std::string recordPath;
    std::string payloadPath;
    try
    {
        operationPath = "Editor/Trash/" + operationId;
        recordPath = operationPath + "/Record.cuetrash";
        payloadPath = operationPath + "/Payload";
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    Result<BoundWorkspacePath> boundRecord = bindSaved(recordPath);
    Result<BoundWorkspacePath> payload = bindSaved(payloadPath);
    Result<BoundWorkspacePath> operation = bindSaved(operationPath);
    Result<BoundWorkspacePath> boundDestination =
        m_workspace->bind_path(m_roots.source_assets(), std::move(*original.try_value()), m_assertContext);
    if (!boundRecord || !payload || !operation || !boundDestination)
    {
        Error cause = !boundRecord ? std::move(*boundRecord.try_error())
                      : !payload   ? std::move(*payload.try_error())
                      : !operation ? std::move(*operation.try_error())
                                   : std::move(*boundDestination.try_error());
        return fail(ProjectFileOperationStage::BindDestination, ProjectFileError::RecoveryRequired,
                    "Project restore path binding failed", std::move(cause), ProjectFileOperationOutcome::NotCommitted);
    }
    Result<std::unique_ptr<WorkspaceEntryMutationGuard>> payloadGuard = m_workspace->guard_entry_if_matches(
        *payload.try_value(), record.try_value()->fingerprint, a_traversalLimits, a_contentLimits);
    if (!payloadGuard)
    {
        return fail(ProjectFileOperationStage::Verify, ProjectFileError::RecoveryRequired,
                    "Project trash payload verification failed", std::move(*payloadGuard.try_error()),
                    ProjectFileOperationOutcome::NotCommitted);
    }
    /// @brief Restore失敗時にPayload Guardを確定し、変更検出診断を保持する
    const auto finishFailureGuard = [&](std::vector<Error> &a_secondary) noexcept -> bool
    {
        Result<void> finished = m_workspace->finish_entry_mutation_guard(std::move(*payloadGuard.try_value()));
        if (finished)
        {
            return true;
        }
        try
        {
            a_secondary.push_back(std::move(*finished.try_error()));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        return false;
    };

    record.try_value()->state = project_files_private::TrashRecordState::Restoring;
    Result<std::vector<std::byte>> restoring =
        project_files_private::serialize_trash_record(*record.try_value(), m_assertContext);
    record.try_value()->state = project_files_private::TrashRecordState::Restored;
    Result<std::vector<std::byte>> restored =
        project_files_private::serialize_trash_record(*record.try_value(), m_assertContext);
    record.try_value()->state = project_files_private::TrashRecordState::Trashed;
    Result<std::vector<std::byte>> trashed =
        project_files_private::serialize_trash_record(*record.try_value(), m_assertContext);
    if (!restoring || !restored || !trashed)
    {
        Error cause = !restoring  ? std::move(*restoring.try_error())
                      : !restored ? std::move(*restored.try_error())
                                  : std::move(*trashed.try_error());
        std::vector<Error> secondary;
        const bool guardFinished = finishFailureGuard(secondary);
        return fail(ProjectFileOperationStage::PrepareRecovery, ProjectFileError::RecoveryRequired,
                    "Project restore record serialization failed", std::move(cause),
                    guardFinished ? ProjectFileOperationOutcome::NotCommitted
                                  : ProjectFileOperationOutcome::ReconciliationRequired,
                    std::move(secondary));
    }
    WorkspaceMutationResult wroteRestoring =
        m_workspace->replace_file_atomic(*boundRecord.try_value(), *restoring.try_value(), operationId);
    if (wroteRestoring.outcome == WorkspaceMutationOutcome::NotCommitted ||
        wroteRestoring.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        Error cause = wroteRestoring.primaryError.has_value()
                          ? std::move(*wroteRestoring.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                    "Project restore checkpoint write failed");
        std::vector<Error> secondary = std::move(wroteRestoring.secondaryDiagnostics);
        const bool guardFinished = finishFailureGuard(secondary);
        return fail(ProjectFileOperationStage::UpdateRecoveryRecord, ProjectFileError::RecoveryRequired,
                    "Project restore checkpoint write failed", std::move(cause),
                    guardFinished ? convert_outcome(wroteRestoring.outcome)
                                  : ProjectFileOperationOutcome::ReconciliationRequired,
                    std::move(secondary));
    }
    captureCommittedMutation(wroteRestoring);

    WorkspaceMutationResult moved = m_workspace->rename_guarded_entry(**payloadGuard.try_value(), *payload.try_value(),
                                                                      *boundDestination.try_value());
    if (moved.outcome == WorkspaceMutationOutcome::NotCommitted)
    {
        std::vector<Error> secondary = std::move(moved.secondaryDiagnostics);
        Result<void> guardFinished = m_workspace->finish_entry_mutation_guard(std::move(*payloadGuard.try_value()));
        if (!guardFinished)
        {
            try
            {
                if (moved.primaryError.has_value())
                {
                    secondary.push_back(std::move(*moved.primaryError));
                }
            }
            catch (...)
            {
                terminate_allocation(m_assertContext);
            }
            return fail(ProjectFileOperationStage::Verify, ProjectFileError::RecoveryRequired,
                        "Project trash payload changed after an unsuccessful restore",
                        std::move(*guardFinished.try_error()), ProjectFileOperationOutcome::ReconciliationRequired,
                        std::move(secondary));
        }

        WorkspaceMutationResult reverted =
            m_workspace->replace_file_atomic(*boundRecord.try_value(), *trashed.try_value(), operationId);
        try
        {
            for (Error &diagnostic : reverted.secondaryDiagnostics)
            {
                secondary.push_back(std::move(diagnostic));
            }
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        if (reverted.primaryError.has_value())
        {
            try
            {
                secondary.push_back(std::move(*reverted.primaryError));
            }
            catch (...)
            {
                terminate_allocation(m_assertContext);
            }
        }
        Error cause = moved.primaryError.has_value()
                          ? std::move(*moved.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                    "Project restore rename failed");
        const ProjectFileError classification = is_io_error(cause, IoError::AlreadyExists) ? ProjectFileError::Conflict
                                                : is_io_error(cause, IoError::NotFound)
                                                    ? ProjectFileError::RecoveryRequired
                                                    : classify_project_file_error(cause, moved.outcome);
        return fail(ProjectFileOperationStage::NativePublish, classification,
                    "Project restore destination is unavailable", std::move(cause),
                    reverted.outcome == WorkspaceMutationOutcome::NotCommitted ||
                            reverted.outcome == WorkspaceMutationOutcome::ReconciliationRequired
                        ? ProjectFileOperationOutcome::ReconciliationRequired
                        : ProjectFileOperationOutcome::NotCommitted,
                    std::move(secondary));
    }
    if (moved.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        std::vector<Error> secondary = std::move(moved.secondaryDiagnostics);
        static_cast<void>(finishFailureGuard(secondary));
        Error cause = moved.primaryError.has_value()
                          ? std::move(*moved.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                    "Project restore rename requires reconciliation");
        return fail(ProjectFileOperationStage::Verify, ProjectFileError::RecoveryRequired,
                    "Project restore rename requires reconciliation", std::move(cause),
                    ProjectFileOperationOutcome::ReconciliationRequired, std::move(secondary));
    }
    captureCommittedMutation(moved);
    WorkspaceMutationResult wroteRestored =
        m_workspace->replace_file_atomic(*boundRecord.try_value(), *restored.try_value(), operationId);
    if (wroteRestored.outcome == WorkspaceMutationOutcome::NotCommitted ||
        wroteRestored.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        Error cause = wroteRestored.primaryError.has_value()
                          ? std::move(*wroteRestored.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                    "Project restore final record write requires reconciliation");
        std::vector<Error> secondary = std::move(wroteRestored.secondaryDiagnostics);
        static_cast<void>(finishFailureGuard(secondary));
        return fail(ProjectFileOperationStage::UpdateRecoveryRecord, ProjectFileError::RecoveryRequired,
                    "Project restore final record write failed", std::move(cause),
                    ProjectFileOperationOutcome::ReconciliationRequired, std::move(secondary));
    }
    captureCommittedMutation(wroteRestored);

    Result<void> guardFinished = m_workspace->finish_entry_mutation_guard(std::move(*payloadGuard.try_value()));
    if (!guardFinished)
    {
        return fail(ProjectFileOperationStage::Verify, ProjectFileError::RecoveryRequired,
                    "Project restore guard detected a concurrent change", std::move(*guardFinished.try_error()),
                    ProjectFileOperationOutcome::ReconciliationRequired);
    }

    std::vector<Error> cleanupDiagnostics;
    WorkspaceMutationResult removedRecord = m_workspace->remove_file_or_empty_directory(*boundRecord.try_value());
    try
    {
        for (Error &diagnostic : removedRecord.secondaryDiagnostics)
        {
            cleanupDiagnostics.push_back(std::move(diagnostic));
        }
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    if (removedRecord.primaryError.has_value())
    {
        try
        {
            cleanupDiagnostics.push_back(std::move(*removedRecord.primaryError));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
    }
    if (removedRecord.outcome != WorkspaceMutationOutcome::NotCommitted &&
        removedRecord.outcome != WorkspaceMutationOutcome::ReconciliationRequired)
    {
        WorkspaceMutationResult removedOperation = m_workspace->remove_file_or_empty_directory(*operation.try_value());
        try
        {
            for (Error &diagnostic : removedOperation.secondaryDiagnostics)
            {
                cleanupDiagnostics.push_back(std::move(diagnostic));
            }
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        if (removedOperation.primaryError.has_value())
        {
            try
            {
                cleanupDiagnostics.push_back(std::move(*removedOperation.primaryError));
            }
            catch (...)
            {
                terminate_allocation(m_assertContext);
            }
        }
    }
    std::erase_if(m_recoveryEntries,
                  /// @brief Restore済みOperationを現在のCatalogから除去する
                  [&](const RecoveryEntry &a_entry) noexcept { return a_entry.operationId == operationId; });
    std::optional<Error> primary(
        make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                "Project restore is visible but one or more durability barriers are unknown"));
    return makeResult(ProjectFileOperationStage::Verify, ProjectFileOperationOutcome::CommittedButDurabilityUnknown,
                      std::move(primary), std::move(cleanupDiagnostics));
}

Result<void> ProjectFileService::refresh_recovery_catalog(TraversalLimits a_traversalLimits,
                                                          ContentVerificationLimits a_contentLimits) noexcept
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        return Result<void>::failure(make_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                                             "Project file service was called from another thread"));
    }
    if (m_isBusy)
    {
        return Result<void>::failure(make_project_file_error(m_assertContext, ProjectFileError::Busy,
                                                             "Project file mutation is already active"));
    }
    if (!a_traversalLimits.is_valid() || !a_contentLimits.is_valid())
    {
        return Result<void>::failure(make_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                                             "Project recovery catalog limits are invalid"));
    }
    BusyReset busy(m_isBusy);

    constexpr std::string_view k_catalogOperationId = "00000000-0000-4000-8000-000000000001";
    WorkspaceMutationResult trashRoot = ensure_trash_root(k_catalogOperationId);
    if (trashRoot.outcome == WorkspaceMutationOutcome::NotCommitted ||
        trashRoot.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
    {
        Error cause = trashRoot.primaryError.has_value()
                          ? std::move(*trashRoot.primaryError)
                          : make_project_file_error(m_assertContext, ProjectFileError::StorageFailure,
                                                    "Project trash root preparation failed");
        for (const Error &diagnostic : trashRoot.secondaryDiagnostics)
        {
            cause.append_secondary_diagnostics(
                m_assertContext, diagnostic, "Project trash root preparation also reported a diagnostic", "TrashRoot");
        }
        const ProjectFileError classification = classify_project_file_error(cause, trashRoot.outcome);
        return Result<void>::failure(reclassify_project_file_error(
            m_assertContext, classification, "Project trash root preparation failed", std::move(cause)));
    }
    /// @brief Catalog EntryのSaved Root内PathをCapabilityへ変換する
    const auto bindSaved = [&](std::string_view a_path) noexcept -> Result<BoundWorkspacePath>
    {
        Result<RelativePath> relative = RelativePath::parse(a_path, m_assertContext);
        if (!relative)
        {
            return Result<BoundWorkspacePath>::failure(std::move(*relative.try_error()));
        }
        return m_workspace->bind_path(m_roots.saved(), std::move(*relative.try_value()), m_assertContext);
    };
    Result<BoundWorkspacePath> trashPath = bindSaved("Editor/Trash");
    if (!trashPath)
    {
        return Result<void>::failure(std::move(*trashPath.try_error()));
    }
    WorkspaceDirectory trashDirectory = WorkspaceDirectory::from_bound_path(*trashPath.try_value());
    Result<DirectorySnapshot> snapshot = m_workspace->list_directory(trashDirectory, a_traversalLimits);
    if (!snapshot)
    {
        return Result<void>::failure(reclassify_project_file_error(m_assertContext, ProjectFileError::StorageFailure,
                                                                   "Project trash catalog enumeration failed",
                                                                   std::move(*snapshot.try_error())));
    }

    std::vector<RecoveryEntry> entries;
    std::vector<Error> diagnostics;
    /// @brief Catalog走査を継続しながらRecovery診断を保持する
    const auto addDiagnostic = [&](std::string_view a_message, std::optional<Error> a_cause = std::nullopt) noexcept
    {
        try
        {
            diagnostics.push_back(
                a_cause.has_value()
                    ? reclassify_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired, a_message,
                                                    std::move(*a_cause))
                    : make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired, a_message));
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
    };
    /// @brief 不完全SnapshotのEntry単位分類とNative CodeをRecovery診断へ移送する
    const auto addSnapshotDiagnostics = [&](const DirectorySnapshot &a_snapshot, std::string_view a_context) noexcept
    {
        for (const WorkspaceDiagnostic &diagnostic : a_snapshot.diagnostics)
        {
            Error cause = diagnostic.nativeCode != 0
                              ? make_io_error(m_assertContext, classify_workspace_diagnostic(diagnostic.code),
                                              "Workspace directory enumeration diagnostic", diagnostic.nativeCode)
                              : make_io_error(m_assertContext, classify_workspace_diagnostic(diagnostic.code),
                                              "Workspace directory enumeration diagnostic");
            addDiagnostic(a_context, std::move(cause));
        }
    };
    if (trashRoot.primaryError.has_value())
    {
        addDiagnostic("Project trash root preparation was not durable", std::move(*trashRoot.primaryError));
    }
    for (Error &diagnostic : trashRoot.secondaryDiagnostics)
    {
        addDiagnostic("Project trash root preparation reported a secondary failure", std::move(diagnostic));
    }
    if (snapshot.try_value()->state != WorkspaceSnapshotState::Complete)
    {
        addSnapshotDiagnostics(*snapshot.try_value(), "Project trash catalog enumeration diagnostic");
        addDiagnostic("Project trash catalog enumeration requires a rescan");
        try
        {
            m_recoveryEntries = std::move(entries);
            m_recoveryDiagnostics = std::move(diagnostics);
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        return Result<void>::success();
    }

    for (const WorkspaceEntry &container : snapshot.try_value()->entries)
    {
        constexpr std::string_view k_stagingPrefix = ".";
        constexpr std::string_view k_stagingSuffix = ".cuetrash-staging";
        if (container.displayName.starts_with(k_stagingPrefix) && container.displayName.ends_with(k_stagingSuffix))
        {
            if (container.displayName.size() != k_stagingPrefix.size() + 36U + k_stagingSuffix.size())
            {
                addDiagnostic("Project trash contains a malformed staging container name");
                continue;
            }
            const std::size_t idLength = container.displayName.size() - k_stagingPrefix.size() - k_stagingSuffix.size();
            const std::string_view stagingId =
                std::string_view(container.displayName).substr(k_stagingPrefix.size(), idLength);
            Result<ProjectId> validatedStagingId = ProjectId::parse(stagingId, m_assertContext);
            Result<BoundWorkspacePath> staging =
                validatedStagingId ? m_workspace->bind_operation_staging_path(*trashPath.try_value(), stagingId,
                                                                              "cuetrash", m_assertContext)
                                   : Result<BoundWorkspacePath>::failure(
                                         make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                                 "Project trash staging operation id is invalid"));
            if (!staging)
            {
                addDiagnostic("Project trash contains an unrecognized staging container",
                              std::move(*staging.try_error()));
                continue;
            }
            WorkspaceDirectory stagingDirectory = WorkspaceDirectory::from_bound_path(*staging.try_value());
            Result<DirectorySnapshot> stagingSnapshot =
                m_workspace->list_directory(stagingDirectory, a_traversalLimits);
            if (!stagingSnapshot || stagingSnapshot.try_value()->state != WorkspaceSnapshotState::Complete)
            {
                if (!stagingSnapshot)
                {
                    addDiagnostic("Project trash staging container could not be verified",
                                  std::move(*stagingSnapshot.try_error()));
                }
                else
                {
                    addSnapshotDiagnostics(*stagingSnapshot.try_value(),
                                           "Project trash staging enumeration diagnostic");
                    addDiagnostic("Project trash staging container enumeration requires a rescan");
                }
                continue;
            }
            bool removeRecordFirst = false;
            std::optional<BoundWorkspacePath> boundStagingRecord;
            if (!stagingSnapshot.try_value()->entries.empty())
            {
                const bool hasOnlyRecord =
                    stagingSnapshot.try_value()->entries.size() == 1U &&
                    stagingSnapshot.try_value()->entries[0U].displayName == "Record.cuetrash" &&
                    stagingSnapshot.try_value()->entries[0U].type == WorkspaceEntryType::RegularFile;
                Result<RelativePath> recordChild = RelativePath::parse("Record.cuetrash", m_assertContext);
                Result<BoundWorkspacePath> stagingRecord =
                    hasOnlyRecord && recordChild
                        ? m_workspace->bind_child_path(*staging.try_value(), std::move(*recordChild.try_value()),
                                                       m_assertContext)
                        : Result<BoundWorkspacePath>::failure(make_project_file_error(
                              m_assertContext, ProjectFileError::RecoveryRequired,
                              "Project trash staging contains entries other than its allocating record"));
                Result<std::vector<std::byte>> stagingBytes =
                    stagingRecord
                        ? m_workspace->read_file_bounded(*stagingRecord.try_value(), k_maximumTrashRecordBytes)
                        : Result<std::vector<std::byte>>::failure(std::move(*stagingRecord.try_error()));
                Result<project_files_private::TrashRecord> stagingRecordValue =
                    stagingBytes
                        ? project_files_private::parse_trash_record(*stagingBytes.try_value(),
                                                                    project_files_private::trash_record_hard_limits(),
                                                                    m_assertContext)
                        : Result<project_files_private::TrashRecord>::failure(std::move(*stagingBytes.try_error()));
                if (!stagingRecordValue || stagingRecordValue.try_value()->projectId != m_projectId.text() ||
                    stagingRecordValue.try_value()->operationId != stagingId ||
                    stagingRecordValue.try_value()->state != project_files_private::TrashRecordState::Allocating)
                {
                    if (!stagingRecordValue)
                    {
                        addDiagnostic("Project trash staging allocating record is invalid",
                                      std::move(*stagingRecordValue.try_error()));
                    }
                    else
                    {
                        addDiagnostic("Project trash staging allocating record identity or state is invalid");
                    }
                    continue;
                }
                boundStagingRecord.emplace(std::move(*stagingRecord.try_value()));
                removeRecordFirst = true;
            }
            if (removeRecordFirst)
            {
                WorkspaceMutationResult removedRecord =
                    m_workspace->remove_file_or_empty_directory(*boundStagingRecord);
                if (removedRecord.primaryError.has_value())
                {
                    addDiagnostic("Project trash staging record cleanup was not durable",
                                  std::move(*removedRecord.primaryError));
                }
                for (Error &diagnostic : removedRecord.secondaryDiagnostics)
                {
                    addDiagnostic("Project trash staging record cleanup reported a secondary failure",
                                  std::move(diagnostic));
                }
                if (removedRecord.outcome == WorkspaceMutationOutcome::NotCommitted ||
                    removedRecord.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
                {
                    addDiagnostic("Project trash staging allocating record could not be removed");
                    continue;
                }
            }
            WorkspaceMutationResult removedStaging = m_workspace->remove_file_or_empty_directory(*staging.try_value());
            if (removedStaging.primaryError.has_value())
            {
                addDiagnostic("Project trash empty staging cleanup was not durable",
                              std::move(*removedStaging.primaryError));
            }
            for (Error &diagnostic : removedStaging.secondaryDiagnostics)
            {
                addDiagnostic("Project trash empty staging cleanup reported a secondary failure",
                              std::move(diagnostic));
            }
            if (removedStaging.outcome == WorkspaceMutationOutcome::NotCommitted ||
                removedStaging.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
            {
                addDiagnostic("Project trash empty staging container could not be removed");
            }
            continue;
        }

        Result<ProjectId> operationId = ProjectId::parse(container.displayName, m_assertContext);
        if (container.type != WorkspaceEntryType::Directory || !container.locator.has_value() || !operationId)
        {
            addDiagnostic("Project trash contains an unrecognized operation container");
            continue;
        }
        std::string operationPath;
        std::string recordPath;
        std::string payloadPath;
        try
        {
            operationPath = "Editor/Trash/" + container.displayName;
            recordPath = operationPath + "/Record.cuetrash";
            payloadPath = operationPath + "/Payload";
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        Result<BoundWorkspacePath> boundRecord = bindSaved(recordPath);
        Result<BoundWorkspacePath> payload = bindSaved(payloadPath);
        Result<BoundWorkspacePath> operation = bindSaved(operationPath);
        if (!boundRecord || !payload || !operation)
        {
            Error cause = !boundRecord ? std::move(*boundRecord.try_error())
                          : !payload   ? std::move(*payload.try_error())
                                       : std::move(*operation.try_error());
            addDiagnostic("Project trash operation paths are invalid", std::move(cause));
            continue;
        }
        Result<BoundWorkspacePath> replacementStaging = m_workspace->bind_operation_staging_path(
            *operation.try_value(), container.displayName, "cuefile-replace", m_assertContext);
        if (!replacementStaging)
        {
            addDiagnostic("Project trash record replacement staging path is invalid",
                          std::move(*replacementStaging.try_error()));
            continue;
        }
        WorkspaceDirectory operationDirectory = WorkspaceDirectory::from_bound_path(*operation.try_value());
        Result<DirectorySnapshot> operationSnapshot =
            m_workspace->list_directory(operationDirectory, a_traversalLimits);
        if (!operationSnapshot)
        {
            addDiagnostic("Project trash operation could not be enumerated", std::move(*operationSnapshot.try_error()));
            continue;
        }
        if (operationSnapshot.try_value()->state != WorkspaceSnapshotState::Complete)
        {
            addSnapshotDiagnostics(*operationSnapshot.try_value(), "Project trash operation enumeration diagnostic");
            addDiagnostic("Project trash operation enumeration requires a rescan");
            continue;
        }
        const WorkspaceEntry *recordEntry = nullptr;
        const WorkspaceEntry *payloadEntry = nullptr;
        bool hasReplacementStaging = false;
        bool hasUnknownEntry = false;
        std::string replacementStagingName;
        try
        {
            replacementStagingName = "." + container.displayName + ".cuefile-replace-staging";
        }
        catch (...)
        {
            terminate_allocation(m_assertContext);
        }
        for (const WorkspaceEntry &entry : operationSnapshot.try_value()->entries)
        {
            if (entry.displayName == "Record.cuetrash" && entry.type == WorkspaceEntryType::RegularFile)
            {
                recordEntry = &entry;
            }
            else if (entry.displayName == "Payload" &&
                     (entry.type == WorkspaceEntryType::RegularFile || entry.type == WorkspaceEntryType::Directory))
            {
                payloadEntry = &entry;
            }
            else if (entry.displayName == replacementStagingName)
            {
                if (hasReplacementStaging)
                {
                    hasUnknownEntry = true;
                }
                else
                {
                    hasReplacementStaging = true;
                }
            }
            else
            {
                hasUnknownEntry = true;
            }
        }
        if (operationSnapshot.try_value()->entries.empty())
        {
            WorkspaceMutationResult removedOperation =
                m_workspace->remove_file_or_empty_directory(*operation.try_value());
            if (removedOperation.primaryError.has_value())
            {
                addDiagnostic("Project trash empty operation cleanup was not durable",
                              std::move(*removedOperation.primaryError));
            }
            for (Error &diagnostic : removedOperation.secondaryDiagnostics)
            {
                addDiagnostic("Project trash empty operation cleanup reported a secondary failure",
                              std::move(diagnostic));
            }
            if (removedOperation.outcome == WorkspaceMutationOutcome::NotCommitted ||
                removedOperation.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
            {
                addDiagnostic("Project trash empty operation container could not be removed");
            }
            continue;
        }
        if (recordEntry == nullptr || hasUnknownEntry)
        {
            addDiagnostic("Project trash operation contains an invalid entry set");
            continue;
        }
        Result<std::vector<std::byte>> bytes =
            m_workspace->read_file_bounded(*boundRecord.try_value(), k_maximumTrashRecordBytes);
        if (!bytes)
        {
            addDiagnostic("Project trash record could not be read", std::move(*bytes.try_error()));
            continue;
        }
        Result<project_files_private::TrashRecord> record = project_files_private::parse_trash_record(
            *bytes.try_value(), project_files_private::trash_record_hard_limits(), m_assertContext);
        if (!record || record.try_value()->projectId != m_projectId.text() ||
            record.try_value()->operationId != container.displayName)
        {
            if (!record)
            {
                addDiagnostic("Project trash record is invalid", std::move(*record.try_error()));
            }
            else
            {
                addDiagnostic("Project trash record identity does not match its container");
            }
            continue;
        }
        if (hasReplacementStaging)
        {
            constexpr ContentVerificationLimits k_replacementStagingLimits{k_maximumTrashRecordBytes,
                                                                           k_maximumTrashRecordBytes};
            Result<WorkspaceEntryFingerprint> replacementStagingFingerprint = m_workspace->fingerprint_entry(
                *replacementStaging.try_value(), a_traversalLimits, k_replacementStagingLimits);
            if (!replacementStagingFingerprint ||
                replacementStagingFingerprint.try_value()->type != WorkspaceEntryType::RegularFile)
            {
                if (!replacementStagingFingerprint)
                {
                    addDiagnostic("Project trash record replacement staging could not be verified",
                                  std::move(*replacementStagingFingerprint.try_error()));
                }
                else
                {
                    addDiagnostic("Project trash record replacement staging is not a regular file");
                }
                continue;
            }
            WorkspaceMutationResult removedReplacementStaging =
                m_workspace->remove_file_or_empty_directory(*replacementStaging.try_value());
            if (removedReplacementStaging.primaryError.has_value())
            {
                addDiagnostic("Project trash record replacement staging cleanup was not durable",
                              std::move(*removedReplacementStaging.primaryError));
            }
            for (Error &diagnostic : removedReplacementStaging.secondaryDiagnostics)
            {
                addDiagnostic("Project trash record replacement staging cleanup reported a secondary failure",
                              std::move(diagnostic));
            }
            if (removedReplacementStaging.outcome == WorkspaceMutationOutcome::NotCommitted ||
                removedReplacementStaging.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
            {
                addDiagnostic("Project trash record replacement staging could not be removed");
                continue;
            }
        }
        Result<RelativePath> original = RelativePath::parse(record.try_value()->originalPath, m_assertContext);
        if (!original)
        {
            addDiagnostic("Project trash original path is invalid", std::move(*original.try_error()));
            continue;
        }
        Result<BoundWorkspacePath> boundOriginal =
            m_workspace->bind_path(m_roots.source_assets(), std::move(*original.try_value()), m_assertContext);
        if (!boundOriginal)
        {
            addDiagnostic("Project trash original path could not be bound", std::move(*boundOriginal.try_error()));
            continue;
        }

        Result<WorkspaceEntryFingerprint> originalFingerprint =
            m_workspace->fingerprint_entry(*boundOriginal.try_value(), a_traversalLimits, a_contentLimits);
        Result<WorkspaceEntryFingerprint> payloadFingerprint =
            m_workspace->fingerprint_entry(*payload.try_value(), a_traversalLimits, a_contentLimits);
        const bool originalMissing =
            !originalFingerprint && is_io_error(*originalFingerprint.try_error(), IoError::NotFound);
        const bool payloadMissing =
            !payloadFingerprint && is_io_error(*payloadFingerprint.try_error(), IoError::NotFound);
        const bool originalMatches =
            originalFingerprint && *originalFingerprint.try_value() == record.try_value()->fingerprint;
        const bool payloadMatches =
            payloadFingerprint && *payloadFingerprint.try_value() == record.try_value()->fingerprint;
        if (!originalFingerprint && !originalMissing)
        {
            addDiagnostic("Project trash original entry fingerprint inspection failed",
                          std::move(*originalFingerprint.try_error()));
        }
        if (!payloadFingerprint && !payloadMissing)
        {
            addDiagnostic("Project trash payload fingerprint inspection failed",
                          std::move(*payloadFingerprint.try_error()));
        }

        /// @brief 存在側EntryをRecord Fingerprintで固定してMutation Guardを取得する
        const auto guardMatchingEntry =
            [&](const BoundWorkspacePath &a_entry) noexcept -> Result<std::unique_ptr<WorkspaceEntryMutationGuard>>
        {
            return m_workspace->guard_entry_if_matches(a_entry, record.try_value()->fingerprint, a_traversalLimits,
                                                       a_contentLimits);
        };
        /// @brief Reconciliationで確定した状態をEntry Guard保持中にRecordへAtomic反映する
        const auto updateRecord = [&](project_files_private::TrashRecordState a_state,
                                      const BoundWorkspacePath &a_guardedEntry) noexcept -> bool
        {
            Result<std::unique_ptr<WorkspaceEntryMutationGuard>> guard = guardMatchingEntry(a_guardedEntry);
            if (!guard)
            {
                addDiagnostic("Project trash reconciliation entry guard could not be acquired",
                              std::move(*guard.try_error()));
                return false;
            }
            record.try_value()->state = a_state;
            Result<std::vector<std::byte>> updated =
                project_files_private::serialize_trash_record(*record.try_value(), m_assertContext);
            if (!updated)
            {
                addDiagnostic("Project trash reconciliation record serialization failed",
                              std::move(*updated.try_error()));
                return false;
            }
            WorkspaceMutationResult written =
                m_workspace->replace_file_atomic(*boundRecord.try_value(), *updated.try_value(), container.displayName);
            if (written.primaryError.has_value())
            {
                addDiagnostic("Project trash reconciliation record write was not durable",
                              std::move(*written.primaryError));
            }
            for (Error &diagnostic : written.secondaryDiagnostics)
            {
                addDiagnostic("Project trash reconciliation record write reported a secondary failure",
                              std::move(diagnostic));
            }
            if (written.outcome == WorkspaceMutationOutcome::NotCommitted ||
                written.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
            {
                addDiagnostic("Project trash reconciliation record write failed");
                return false;
            }
            Result<void> finished = m_workspace->finish_entry_mutation_guard(std::move(*guard.try_value()));
            if (!finished)
            {
                addDiagnostic("Project trash reconciliation entry changed during record update",
                              std::move(*finished.try_error()));
                return false;
            }
            return true;
        };
        /// @brief Payloadを含まない終端Recordと空Operation ContainerをCleanupする
        const auto cleanupRecord = [&]() noexcept -> bool
        {
            WorkspaceMutationResult removedRecord =
                m_workspace->remove_file_or_empty_directory(*boundRecord.try_value());
            if (removedRecord.primaryError.has_value())
            {
                addDiagnostic("Project trash completed record cleanup was not durable",
                              std::move(*removedRecord.primaryError));
            }
            for (Error &diagnostic : removedRecord.secondaryDiagnostics)
            {
                addDiagnostic("Project trash completed record cleanup reported a secondary failure",
                              std::move(diagnostic));
            }
            if (removedRecord.outcome == WorkspaceMutationOutcome::NotCommitted ||
                removedRecord.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
            {
                addDiagnostic("Project trash completed record cleanup failed");
                return false;
            }
            WorkspaceMutationResult removedOperation =
                m_workspace->remove_file_or_empty_directory(*operation.try_value());
            if (removedOperation.primaryError.has_value())
            {
                addDiagnostic("Project trash completed container cleanup was not durable",
                              std::move(*removedOperation.primaryError));
            }
            for (Error &diagnostic : removedOperation.secondaryDiagnostics)
            {
                addDiagnostic("Project trash completed container cleanup reported a secondary failure",
                              std::move(diagnostic));
            }
            if (removedOperation.outcome == WorkspaceMutationOutcome::NotCommitted ||
                removedOperation.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
            {
                addDiagnostic("Project trash completed container cleanup failed");
                return false;
            }
            return true;
        };
        /// @brief 未Commit DeleteをGuard付き中間状態から終端状態へ確定してCleanupする
        const auto abortAndCleanupRecord = [&](const BoundWorkspacePath &a_guardedEntry) noexcept -> bool
        {
            if (!updateRecord(project_files_private::TrashRecordState::Aborting, a_guardedEntry))
            {
                return false;
            }

            record.try_value()->state = project_files_private::TrashRecordState::Aborted;
            Result<std::vector<std::byte>> aborted =
                project_files_private::serialize_trash_record(*record.try_value(), m_assertContext);
            if (!aborted)
            {
                addDiagnostic("Project trash aborted record serialization failed", std::move(*aborted.try_error()));
                return false;
            }
            WorkspaceMutationResult written =
                m_workspace->replace_file_atomic(*boundRecord.try_value(), *aborted.try_value(), container.displayName);
            if (written.primaryError.has_value())
            {
                addDiagnostic("Project trash aborted record write was not durable", std::move(*written.primaryError));
            }
            for (Error &diagnostic : written.secondaryDiagnostics)
            {
                addDiagnostic("Project trash aborted record write reported a secondary failure", std::move(diagnostic));
            }
            if (written.outcome == WorkspaceMutationOutcome::NotCommitted ||
                written.outcome == WorkspaceMutationOutcome::ReconciliationRequired)
            {
                addDiagnostic("Project trash aborted record write failed");
                return false;
            }
            return cleanupRecord();
        };
        /// @brief Recovery Catalog公開直前に存在側EntryをGuard付きで再検証する
        const auto verifyCatalogEntry = [&](const BoundWorkspacePath &a_guardedEntry) noexcept -> bool
        {
            Result<std::unique_ptr<WorkspaceEntryMutationGuard>> guard = guardMatchingEntry(a_guardedEntry);
            if (!guard)
            {
                addDiagnostic("Project trash catalog entry guard could not be acquired", std::move(*guard.try_error()));
                return false;
            }
            Result<void> finished = m_workspace->finish_entry_mutation_guard(std::move(*guard.try_value()));
            if (!finished)
            {
                addDiagnostic("Project trash catalog entry changed during verification",
                              std::move(*finished.try_error()));
                return false;
            }
            return true;
        };
        /// @brief 検証済みRecordを指定状態のRecovery Catalog Entryへ変換する
        const auto addEntry = [&](RecoveryEntryState a_state) noexcept
        {
            RecoveryEntry entry;
            try
            {
                entry.operationId = container.displayName;
                entry.originalPath = record.try_value()->originalPath;
                entry.originalArea = ProjectFileArea::SourceAssets;
                entry.entryType = record.try_value()->fingerprint.type;
                entry.byteSize = fingerprint_byte_size(record.try_value()->fingerprint);
                entry.descendantCount = record.try_value()->fingerprint.manifest.size();
                entry.state = a_state;
                entries.push_back(std::move(entry));
            }
            catch (...)
            {
                terminate_allocation(m_assertContext);
            }
        };

        using project_files_private::TrashRecordState;
        switch (record.try_value()->state)
        {
        case TrashRecordState::Allocating:
            if (payloadEntry == nullptr && payloadMissing && originalMatches)
            {
                if (!abortAndCleanupRecord(*boundOriginal.try_value()))
                {
                    addEntry(RecoveryEntryState::ReconciliationRequired);
                }
            }
            else
            {
                addEntry(RecoveryEntryState::ReconciliationRequired);
                addDiagnostic("Allocating trash operation does not match its original entry or contains a payload");
            }
            break;
        case TrashRecordState::Prepared:
            if (originalMissing && payloadMatches && updateRecord(TrashRecordState::Trashed, *payload.try_value()))
            {
                addEntry(RecoveryEntryState::Recoverable);
            }
            else if (originalMatches && payloadMissing)
            {
                if (!abortAndCleanupRecord(*boundOriginal.try_value()))
                {
                    addEntry(RecoveryEntryState::ReconciliationRequired);
                }
            }
            else
            {
                addEntry(RecoveryEntryState::ReconciliationRequired);
                addDiagnostic("Prepared trash operation could not be reconciled automatically");
            }
            break;
        case TrashRecordState::Trashed:
            if (originalMissing && payloadMatches && payloadEntry != nullptr &&
                verifyCatalogEntry(*payload.try_value()))
            {
                addEntry(RecoveryEntryState::Recoverable);
            }
            else if (originalMatches && payloadMissing)
            {
                if (!abortAndCleanupRecord(*boundOriginal.try_value()))
                {
                    addEntry(RecoveryEntryState::ReconciliationRequired);
                }
            }
            else
            {
                addEntry(RecoveryEntryState::ReconciliationRequired);
                addDiagnostic("Trashed operation does not match its expected data location");
            }
            break;
        case TrashRecordState::Restoring:
            if (originalMatches && payloadMissing &&
                updateRecord(TrashRecordState::Restored, *boundOriginal.try_value()))
            {
                if (!cleanupRecord())
                {
                    addEntry(RecoveryEntryState::ReconciliationRequired);
                }
            }
            else if (originalMissing && payloadMatches && updateRecord(TrashRecordState::Trashed, *payload.try_value()))
            {
                addEntry(RecoveryEntryState::Recoverable);
            }
            else
            {
                addEntry(RecoveryEntryState::ReconciliationRequired);
                addDiagnostic("Restoring operation could not be reconciled automatically");
            }
            break;
        case TrashRecordState::Restored:
            if (originalMatches && payloadMissing)
            {
                if (!cleanupRecord())
                {
                    addEntry(RecoveryEntryState::ReconciliationRequired);
                }
            }
            else if (originalMissing && payloadMatches && updateRecord(TrashRecordState::Trashed, *payload.try_value()))
            {
                addEntry(RecoveryEntryState::Recoverable);
            }
            else
            {
                addEntry(RecoveryEntryState::ReconciliationRequired);
                addDiagnostic("Restored operation does not match its expected data location");
            }
            break;
        case TrashRecordState::Aborting:
            if (payloadEntry == nullptr && payloadMissing && originalMatches)
            {
                if (!abortAndCleanupRecord(*boundOriginal.try_value()))
                {
                    addEntry(RecoveryEntryState::ReconciliationRequired);
                }
            }
            else
            {
                addEntry(RecoveryEntryState::ReconciliationRequired);
                addDiagnostic("Aborting trash operation could not be reconciled automatically");
            }
            break;
        case TrashRecordState::Aborted:
            if (payloadEntry == nullptr && payloadMissing)
            {
                if (!cleanupRecord())
                {
                    addEntry(RecoveryEntryState::ReconciliationRequired);
                }
            }
            else
            {
                addEntry(RecoveryEntryState::ReconciliationRequired);
                addDiagnostic("Aborted trash operation unexpectedly contains a payload");
            }
            break;
        }
    }
    try
    {
        std::sort(entries.begin(), entries.end(),
                  /// @brief Recovery EntryをOperation IDの決定順へ並べる
                  [](const RecoveryEntry &a_left, const RecoveryEntry &a_right) noexcept
                  { return a_left.operationId < a_right.operationId; });
        m_recoveryEntries = std::move(entries);
        m_recoveryDiagnostics = std::move(diagnostics);
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    return Result<void>::success();
}

Result<ProjectFileOperationResult> ProjectFileService::execute_create(ProjectFileOperationKind a_kind,
                                                                      ProjectFileArea a_area,
                                                                      RelativePath a_destination,
                                                                      std::span<const std::byte> a_bytes) noexcept
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        return Result<ProjectFileOperationResult>::failure(make_project_file_error(
            m_assertContext, ProjectFileError::InvalidRequest, "Project file service was called from another thread"));
    }
    if (m_isBusy)
    {
        return Result<ProjectFileOperationResult>::failure(make_project_file_error(
            m_assertContext, ProjectFileError::Busy, "Project file mutation is already active"));
    }
    BusyReset busy(m_isBusy);

    Result<std::string> generatedId = m_operationIdSource->next_operation_id();
    if (!generatedId)
    {
        return Result<ProjectFileOperationResult>::failure(reclassify_project_file_error(
            m_assertContext, ProjectFileError::InvalidRequest, "Project file operation id generation failed",
            std::move(*generatedId.try_error())));
    }
    Result<ProjectId> validatedId = ProjectId::parse(*generatedId.try_value(), m_assertContext);
    if (!validatedId)
    {
        return Result<ProjectFileOperationResult>::failure(
            reclassify_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                          "Project file operation id is invalid", std::move(*validatedId.try_error())));
    }

    std::string operationId = std::move(*generatedId.try_value());
    std::string destination;
    try
    {
        destination.assign(a_destination.text());
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }

    if (!m_policy.can_mutate(a_area))
    {
        std::optional<Error> primary(make_project_file_error(m_assertContext, ProjectFileError::ProtectedEntry,
                                                             "Project file area is protected from mutation"));
        ProjectFileOperationResult result(std::move(operationId), a_kind, a_area, std::nullopt, std::move(destination),
                                          ProjectFileOperationStage::ValidateRequest,
                                          ProjectFileOperationOutcome::NotCommitted, std::move(primary), {}, {});
        return Result<ProjectFileOperationResult>::success(std::move(result));
    }

    Result<BoundWorkspacePath> bound = m_workspace->bind_path(area_root(a_area), a_destination, m_assertContext);
    if (!bound)
    {
        const ProjectFileError classification =
            classify_project_file_error(*bound.try_error(), WorkspaceMutationOutcome::NotCommitted);
        std::optional<Error> primary(reclassify_project_file_error(
            m_assertContext, classification, "Project file destination binding failed", std::move(*bound.try_error())));
        ProjectFileOperationResult result(std::move(operationId), a_kind, a_area, std::nullopt, std::move(destination),
                                          ProjectFileOperationStage::BindDestination,
                                          ProjectFileOperationOutcome::NotCommitted, std::move(primary), {}, {});
        return Result<ProjectFileOperationResult>::success(std::move(result));
    }

    WorkspaceMutationResult mutation =
        a_kind == ProjectFileOperationKind::DirectoryCreation
            ? m_workspace->create_directory_new(*bound.try_value(), operationId)
            : m_workspace->create_file_new_atomic(*bound.try_value(), a_bytes, operationId);

    std::optional<Error> primary;
    if (mutation.primaryError.has_value())
    {
        const ProjectFileError classification = classify_project_file_error(*mutation.primaryError, mutation.outcome);
        primary.emplace(reclassify_project_file_error(m_assertContext, classification,
                                                      "Project file create operation failed",
                                                      std::move(*mutation.primaryError)));
    }
    else if (mutation.outcome != WorkspaceMutationOutcome::Committed)
    {
        primary.emplace(make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                "Project file create result requires reconciliation"));
    }

    std::vector<std::string> rescanDirectories;
    try
    {
        const std::size_t separator = destination.rfind('/');
        rescanDirectories.emplace_back(separator == std::string::npos ? std::string{}
                                                                      : destination.substr(0U, separator));
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }

    const ProjectFileOperationStage stage =
        mutation.outcome == WorkspaceMutationOutcome::Committed
            ? ProjectFileOperationStage::Complete
            : (mutation.outcome == WorkspaceMutationOutcome::NotCommitted ? ProjectFileOperationStage::NativePublish
                                                                          : ProjectFileOperationStage::Verify);
    ProjectFileOperationResult result(std::move(operationId), a_kind, a_area, std::nullopt, std::move(destination),
                                      stage, convert_outcome(mutation.outcome), std::move(primary),
                                      std::move(mutation.secondaryDiagnostics), std::move(rescanDirectories));
    return Result<ProjectFileOperationResult>::success(std::move(result));
}

Result<ProjectFileOperationResult> ProjectFileService::execute_transfer(
    ProjectFileOperationKind a_kind, ProjectFileArea a_area, RelativePath a_source, RelativePath a_destination,
    TraversalLimits a_traversalLimits, ContentVerificationLimits a_contentLimits) noexcept
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        return Result<ProjectFileOperationResult>::failure(make_project_file_error(
            m_assertContext, ProjectFileError::InvalidRequest, "Project file service was called from another thread"));
    }
    if (m_isBusy)
    {
        return Result<ProjectFileOperationResult>::failure(make_project_file_error(
            m_assertContext, ProjectFileError::Busy, "Project file mutation is already active"));
    }
    BusyReset busy(m_isBusy);

    Result<std::string> generatedId = m_operationIdSource->next_operation_id();
    if (!generatedId)
    {
        return Result<ProjectFileOperationResult>::failure(reclassify_project_file_error(
            m_assertContext, ProjectFileError::InvalidRequest, "Project file operation id generation failed",
            std::move(*generatedId.try_error())));
    }
    Result<ProjectId> validatedId = ProjectId::parse(*generatedId.try_value(), m_assertContext);
    if (!validatedId)
    {
        return Result<ProjectFileOperationResult>::failure(
            reclassify_project_file_error(m_assertContext, ProjectFileError::InvalidRequest,
                                          "Project file operation id is invalid", std::move(*validatedId.try_error())));
    }

    std::string operationId = std::move(*generatedId.try_value());
    std::string source;
    std::string destination;
    try
    {
        source.assign(a_source.text());
        destination.assign(a_destination.text());
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }

    const std::string sourceKey = a_source.comparison_key(m_assertContext);
    const std::string destinationKey = a_destination.comparison_key(m_assertContext);
    const std::size_t sourceSeparator = source.rfind('/');
    const std::size_t destinationSeparator = destination.rfind('/');
    const std::string_view sourceParent = sourceSeparator == std::string::npos
                                              ? std::string_view{}
                                              : std::string_view(source).substr(0U, sourceSeparator);
    const std::string_view destinationParent = destinationSeparator == std::string::npos
                                                   ? std::string_view{}
                                                   : std::string_view(destination).substr(0U, destinationSeparator);
    const std::size_t sourceKeySeparator = sourceKey.rfind('/');
    const std::size_t destinationKeySeparator = destinationKey.rfind('/');
    const std::string_view sourceParentKey = sourceKeySeparator == std::string::npos
                                                 ? std::string_view{}
                                                 : std::string_view(sourceKey).substr(0U, sourceKeySeparator);
    const std::string_view destinationParentKey =
        destinationKeySeparator == std::string::npos
            ? std::string_view{}
            : std::string_view(destinationKey).substr(0U, destinationKeySeparator);
    const bool samePath = source == destination;
    const bool samePortablePath = sourceKey == destinationKey;
    const bool destinationIsDescendant = destinationKey.size() > sourceKey.size() &&
                                         destinationKey.starts_with(sourceKey) &&
                                         destinationKey[sourceKey.size()] == '/';
    const bool invalidKind = a_kind != ProjectFileOperationKind::Rename && a_kind != ProjectFileOperationKind::Move &&
                             a_kind != ProjectFileOperationKind::Copy;
    const bool wrongRenameParent = a_kind == ProjectFileOperationKind::Rename && sourceParent != destinationParent;
    const bool wrongMoveParent = a_kind == ProjectFileOperationKind::Move && sourceParentKey == destinationParentKey;
    const bool invalidCopyLimits = a_kind == ProjectFileOperationKind::Copy && !a_contentLimits.is_valid();
    if (!m_policy.can_mutate(a_area) || invalidKind || !a_traversalLimits.is_valid() || invalidCopyLimits || samePath ||
        (samePortablePath && a_kind != ProjectFileOperationKind::Rename) || wrongRenameParent || wrongMoveParent ||
        destinationIsDescendant)
    {
        const ProjectFileError errorCode =
            !m_policy.can_mutate(a_area) ? ProjectFileError::ProtectedEntry : ProjectFileError::InvalidRequest;
        std::optional<Error> primary(make_project_file_error(
            m_assertContext, errorCode,
            !m_policy.can_mutate(a_area) ? "Project file area is protected from mutation"
                                         : "Project file transfer request violates semantic preconditions"));
        ProjectFileOperationResult result(std::move(operationId), a_kind, a_area, std::move(source),
                                          std::move(destination), ProjectFileOperationStage::ValidateRequest,
                                          ProjectFileOperationOutcome::NotCommitted, std::move(primary), {}, {});
        return Result<ProjectFileOperationResult>::success(std::move(result));
    }

    Result<BoundWorkspacePath> boundSource = m_workspace->bind_path(area_root(a_area), a_source, m_assertContext);
    if (!boundSource)
    {
        const ProjectFileError classification =
            classify_project_file_error(*boundSource.try_error(), WorkspaceMutationOutcome::NotCommitted);
        std::optional<Error> primary(reclassify_project_file_error(m_assertContext, classification,
                                                                   "Project file source binding failed",
                                                                   std::move(*boundSource.try_error())));
        ProjectFileOperationResult result(std::move(operationId), a_kind, a_area, std::move(source),
                                          std::move(destination), ProjectFileOperationStage::BindSource,
                                          ProjectFileOperationOutcome::NotCommitted, std::move(primary), {}, {});
        return Result<ProjectFileOperationResult>::success(std::move(result));
    }
    Result<BoundWorkspacePath> boundDestination =
        m_workspace->bind_path(area_root(a_area), a_destination, m_assertContext);
    if (!boundDestination)
    {
        const ProjectFileError classification =
            classify_project_file_error(*boundDestination.try_error(), WorkspaceMutationOutcome::NotCommitted);
        std::optional<Error> primary(reclassify_project_file_error(m_assertContext, classification,
                                                                   "Project file destination binding failed",
                                                                   std::move(*boundDestination.try_error())));
        ProjectFileOperationResult result(std::move(operationId), a_kind, a_area, std::move(source),
                                          std::move(destination), ProjectFileOperationStage::BindDestination,
                                          ProjectFileOperationOutcome::NotCommitted, std::move(primary), {}, {});
        return Result<ProjectFileOperationResult>::success(std::move(result));
    }

    WorkspaceMutationResult mutation =
        a_kind == ProjectFileOperationKind::Copy
            ? m_workspace->copy_entry_new(*boundSource.try_value(), *boundDestination.try_value(), a_traversalLimits,
                                          a_contentLimits, operationId)
            : m_workspace->rename_entry(*boundSource.try_value(), *boundDestination.try_value(), a_traversalLimits);
    std::optional<Error> primary;
    if (mutation.primaryError.has_value())
    {
        const ProjectFileError classification = classify_project_file_error(*mutation.primaryError, mutation.outcome);
        primary.emplace(reclassify_project_file_error(m_assertContext, classification,
                                                      "Project file transfer operation failed",
                                                      std::move(*mutation.primaryError)));
    }
    else if (mutation.outcome != WorkspaceMutationOutcome::Committed)
    {
        primary.emplace(make_project_file_error(m_assertContext, ProjectFileError::RecoveryRequired,
                                                "Project file transfer result requires reconciliation"));
    }

    std::vector<std::string> rescanDirectories;
    try
    {
        rescanDirectories.emplace_back(sourceParent);
        if (sourceParentKey != destinationParentKey)
        {
            rescanDirectories.emplace_back(destinationParent);
        }
    }
    catch (...)
    {
        terminate_allocation(m_assertContext);
    }
    const ProjectFileOperationStage stage =
        mutation.outcome == WorkspaceMutationOutcome::Committed
            ? ProjectFileOperationStage::Complete
            : (mutation.outcome == WorkspaceMutationOutcome::NotCommitted ? ProjectFileOperationStage::NativePublish
                                                                          : ProjectFileOperationStage::Verify);
    ProjectFileOperationResult result(std::move(operationId), a_kind, a_area, std::move(source), std::move(destination),
                                      stage, convert_outcome(mutation.outcome), std::move(primary),
                                      std::move(mutation.secondaryDiagnostics), std::move(rescanDirectories));
    return Result<ProjectFileOperationResult>::success(std::move(result));
}

const RelativePath &ProjectFileService::area_root(ProjectFileArea a_area) const noexcept
{
    switch (a_area)
    {
    case ProjectFileArea::SourceAssets:
        return m_roots.source_assets();
    case ProjectFileArea::RuntimeAssets:
        return m_roots.runtime_assets();
    case ProjectFileArea::Generated:
        return m_roots.generated();
    case ProjectFileArea::Saved:
        return m_roots.saved();
    }
    return m_roots.source_assets();
}
} // namespace cue::project_files
