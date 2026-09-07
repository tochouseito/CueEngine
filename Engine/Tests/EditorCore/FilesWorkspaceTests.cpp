#include <Cue/EditorCore/FilesWorkspace.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/IO/Windows/WindowsWorkspaceFilesystem.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/ProjectFiles/Error.h>
#include <Cue/ProjectFiles/Service.h>
#include <Cue/Scene/ComponentData.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Scene/Serialization.h>
#include <Cue/Schema/Registry.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
/// @brief Failure StageをCTest出力へ記録して失敗を返す
[[nodiscard]] bool fail_stage(std::string_view a_stage) noexcept
{
    std::fwrite(a_stage.data(), 1U, a_stage.size(), stderr);
    std::fwrite("\n", 1U, 1U, stderr);
    return false;
}

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の予期しないFatalをProcess失敗へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }

    /// @brief Test中の予期しない詳細付きFatalをProcess失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

class SequenceOperationIdSource final : public cue::project_files::ProjectFileOperationIdSource
{
  public:
    /// @brief 決定的なUUID Version 4候補を順番に返すSourceを構築する
    explicit SequenceOperationIdSource(const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext)
    {
    }

    /// @brief 次の決定的Operation IDを返す
    [[nodiscard]] cue::Result<std::string> next_operation_id() noexcept override
    {
        static constexpr std::array<std::string_view, 12U> k_ids{
            "00000000-0000-4000-8000-000000000301", "00000000-0000-4000-8000-000000000302",
            "00000000-0000-4000-8000-000000000303", "00000000-0000-4000-8000-000000000304",
            "00000000-0000-4000-8000-000000000305", "00000000-0000-4000-8000-000000000306",
            "00000000-0000-4000-8000-000000000307", "00000000-0000-4000-8000-000000000308",
            "00000000-0000-4000-8000-000000000309", "00000000-0000-4000-8000-00000000030a",
            "00000000-0000-4000-8000-00000000030b", "00000000-0000-4000-8000-00000000030c"};
        if (m_next >= k_ids.size())
        {
            return cue::Result<std::string>::failure(cue::project_files::make_project_file_error(
                *m_assertContext, cue::project_files::ProjectFileError::InvalidRequest,
                "Files workspace test operation id sequence is exhausted"));
        }
        return cue::Result<std::string>::success(std::string(k_ids[m_next++]));
    }

  private:
    const cue::AssertContext *m_assertContext;
    std::size_t m_next = 0U;
};

/// @brief Native Test FileへUTF-8本文を書き込む
[[nodiscard]] bool write_file(std::wstring_view a_path, std::string_view a_text) noexcept
{
    const std::wstring path(a_path);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0U;
    const BOOL succeeded = WriteFile(file, a_text.data(), static_cast<DWORD>(a_text.size()), &written, nullptr);
    const BOOL flushed = FlushFileBuffers(file);
    CloseHandle(file);
    return succeeded != FALSE && flushed != FALSE && written == static_cast<DWORD>(a_text.size());
}

class TestProject final
{
  public:
    /// @brief Files Workspace統合Test用の一意なProject Treeを作成する
    explicit TestProject(const cue::AssertContext &a_assertContext)
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0U || length >= temporary.size())
        {
            return;
        }
        static std::atomic_uint64_t sequence{0U};
        m_root = temporary.data();
        m_root += L"CueFilesWorkspaceTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64()) + L"-" +
                  std::to_wstring(sequence.fetch_add(1U, std::memory_order_relaxed));
        if (CreateDirectoryW(m_root.c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Assets").c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Assets\\Source").c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Assets\\Source\\Folder").c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Assets\\Runtime").c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Generated").c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Saved").c_str(), nullptr) == FALSE ||
            !write_file(child(L"Assets\\Source\\Root.txt"), "root") ||
            !write_file(child(L"Assets\\Source\\Folder\\Nested.txt"), "nested") ||
            !write_file(child(L"Assets\\Source\\Folder\\Open.cuescene"), "scene"))
        {
            return;
        }

        cue::Result<cue::ProjectId> id = cue::ProjectId::parse("12345678-1234-4234-8234-123456789abc", a_assertContext);
        if (!id)
        {
            return;
        }
        cue::Result<cue::ProjectDescriptor> descriptor = cue::create_blank_project_descriptor(
            *id.try_value(), "Files Workspace Test",
            cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}}, a_assertContext);
        if (!descriptor)
        {
            return;
        }
        cue::Result<std::string> serialized =
            cue::serialize_project_descriptor(*descriptor.try_value(), a_assertContext);
        if (!serialized || !write_file(child(L"CueProject.json"), *serialized.try_value()))
        {
            return;
        }
        m_descriptor.emplace(std::move(*descriptor.try_value()));
        m_created = true;
    }

    /// @brief Test Projectの複製を禁止する
    TestProject(const TestProject &) = delete;
    /// @brief Test ProjectのCopy代入を禁止する
    TestProject &operator=(const TestProject &) = delete;
    /// @brief Test終了時にProject Treeを削除する
    ~TestProject()
    {
        std::error_code error;
        std::filesystem::remove_all(m_root, error);
    }

    /// @brief Project Tree作成が成功したか返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_created;
    }

    /// @brief Project Descriptorを返す
    [[nodiscard]] const cue::ProjectDescriptor &descriptor() const noexcept
    {
        return *m_descriptor;
    }

    /// @brief EditorController用にDescriptorを所有複製する
    [[nodiscard]] cue::Result<cue::ProjectDescriptor> clone_descriptor(
        const cue::AssertContext &a_assertContext) const noexcept
    {
        cue::Result<std::string> serialized = cue::serialize_project_descriptor(*m_descriptor, a_assertContext);
        return serialized ? cue::parse_project_descriptor(*serialized.try_value(), a_assertContext)
                          : cue::Result<cue::ProjectDescriptor>::failure(std::move(*serialized.try_error()));
    }

    /// @brief Project RootのUTF-8 Pathを返す
    [[nodiscard]] std::string root_utf8() const
    {
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_root.data(),
                                              static_cast<int>(m_root.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_root.data(), static_cast<int>(m_root.size()),
                                result.data(), count, nullptr, nullptr) != count)
        {
            return {};
        }
        return result;
    }

    /// @brief Project Root配下のNative Child PathをUTF-8で返す
    [[nodiscard]] std::string child_utf8(std::wstring_view a_relative) const
    {
        const std::wstring path = child(a_relative);
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
                                              nullptr, 0, nullptr, nullptr);
        if (count <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
                                result.data(), count, nullptr, nullptr) != count)
        {
            return {};
        }
        return result;
    }

    /// @brief Project Root配下のNative Child Pathを返す
    [[nodiscard]] std::wstring child(std::wstring_view a_relative) const
    {
        return m_root + L"\\" + std::wstring(a_relative);
    }

  private:
    std::wstring m_root;
    std::optional<cue::ProjectDescriptor> m_descriptor;
    bool m_created = false;
};

/// @brief Test Project Rootへ拘束された新しいProjectFileServiceを生成する
[[nodiscard]] cue::Result<cue::project_files::ProjectFileService> create_project_file_service(
    const TestProject &a_project, const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<std::unique_ptr<cue::WorkspaceFilesystem>> workspace =
        cue::create_windows_workspace_filesystem(a_project.root_utf8(), a_assertContext);
    if (!workspace)
    {
        return cue::Result<cue::project_files::ProjectFileService>::failure(std::move(*workspace.try_error()));
    }
    return cue::project_files::ProjectFileService::create(a_project.descriptor(), std::move(*workspace.try_value()),
                                                          std::make_unique<SequenceOperationIdSource>(a_assertContext),
                                                          a_assertContext);
}

/// @brief Snapshot群に指定Area相対Locatorが存在するか判定する
[[nodiscard]] bool contains_entry(const cue::editor_core::FilesViewModel &a_view, std::string_view a_locator) noexcept
{
    return std::ranges::any_of(a_view.directories(),
                               /// @brief 一つのDirectory Snapshotに対象Locatorがあるか判定する
                               [a_locator](const cue::project_files::ProjectFileDirectorySnapshot &a_snapshot)
                               {
                                   return std::ranges::any_of(
                                       a_snapshot.entries,
                                       /// @brief 一つのEntry Locatorが対象と一致するか判定する
                                       [a_locator](const cue::project_files::ProjectFileEntry &a_entry)
                                       { return a_entry.locator == a_locator; });
                               });
}

/// @brief CommittedまたはDurability不明の公開済み結果か判定する
[[nodiscard]] bool is_published(cue::project_files::ProjectFileOperationOutcome a_outcome) noexcept
{
    return a_outcome == cue::project_files::ProjectFileOperationOutcome::Committed ||
           a_outcome == cue::project_files::ProjectFileOperationOutcome::CommittedButDurabilityUnknown;
}

/// @brief 外部削除がAuthoritative SnapshotとSelectionへ反映されるまで上限付きでPollする
[[nodiscard]] bool wait_for_external_refresh(cue::editor_core::FilesWorkspaceService &a_service,
                                             std::string_view a_removedLocator) noexcept
{
    for (std::size_t attempt = 0U; attempt < 300U; ++attempt)
    {
        cue::Result<bool> polled = a_service.poll_external_changes();
        if (!polled)
        {
            return false;
        }
        if (*polled.try_value() && !a_service.view_model().selection().has_value() &&
            !contains_entry(a_service.view_model(), a_removedLocator))
        {
            return true;
        }
        Sleep(10U);
    }
    return false;
}

/// @brief Files ViewModel、Operation、Open Document Guard、外部変更の一貫性を検証する
[[nodiscard]] bool test_files_workspace(const cue::AssertContext &a_assertContext)
{
    TestProject project(a_assertContext);
    if (!project.is_created())
    {
        return fail_stage("project");
    }
    cue::Result<cue::project_files::ProjectFileService> projectFiles =
        create_project_file_service(project, a_assertContext);
    cue::Result<cue::ProjectDescriptor> editorDescriptor = project.clone_descriptor(a_assertContext);
    cue::Result<std::unique_ptr<cue::FilesystemRoot>> sourceAssets =
        cue::create_windows_filesystem_root(project.child_utf8(L"Assets\\Source"), a_assertContext);
    cue::Result<std::unique_ptr<cue::FilesystemRoot>> savedRoot =
        cue::create_windows_filesystem_root(project.child_utf8(L"Saved"), a_assertContext);
    cue::schema::SchemaRegistryIdentitySource schemaIdentitySource;
    cue::schema::SchemaRegistryBuilder schemaBuilder(schemaIdentitySource, a_assertContext);
    cue::Result<std::unique_ptr<cue::schema::SchemaRegistry>> schemaRegistry = schemaBuilder.seal();
    if (!projectFiles || !editorDescriptor || !sourceAssets || !savedRoot || !schemaRegistry)
    {
        return fail_stage("services");
    }
    cue::Result<cue::scene::ComponentValueSchemaRegistry> valueRegistry =
        cue::scene::ComponentValueSchemaRegistry::create({}, **schemaRegistry.try_value(), a_assertContext);
    cue::scene::SceneMigrationRegistry sceneMigrations;
    cue::scene::ComponentMigrationRegistry componentMigrations;
    if (!valueRegistry)
    {
        return fail_stage("value-registry");
    }
    cue::editor_core::ScenePersistenceServices persistence(**sourceAssets.try_value(), **savedRoot.try_value(),
                                                           **schemaRegistry.try_value(), *valueRegistry.try_value(),
                                                           sceneMigrations, componentMigrations);
    std::unique_ptr<cue::editor_core::EditorController> editor = cue::editor_core::EditorController::create(
        std::move(*editorDescriptor.try_value()), persistence, a_assertContext);
    constexpr cue::editor_core::FilesWorkspaceLimits k_limits{
        cue::TraversalLimits{32U, 4096U, 1024U, 1024U * 1024U},
        cue::ContentVerificationLimits{1024U * 1024U, 16U * 1024U * 1024U},
        cue::WorkspaceWatchLimits{256U, 64U * 1024U, 128U, 25U, 250U}};

    cue::Result<cue::ProjectDescriptor> noPersistenceDescriptor = project.clone_descriptor(a_assertContext);
    cue::Result<cue::project_files::ProjectFileService> noPersistenceFiles =
        create_project_file_service(project, a_assertContext);
    if (!noPersistenceDescriptor || !noPersistenceFiles)
    {
        return fail_stage("factory-persistence-input");
    }
    std::unique_ptr<cue::editor_core::EditorController> noPersistenceController =
        cue::editor_core::EditorController::create(std::move(*noPersistenceDescriptor.try_value()), a_assertContext);
    cue::Result<std::unique_ptr<cue::editor_core::FilesWorkspaceService>> noPersistenceService =
        cue::editor_core::FilesWorkspaceService::create(std::move(*noPersistenceFiles.try_value()),
                                                        *noPersistenceController, k_limits, a_assertContext);
    if (noPersistenceService || noPersistenceService.try_error() == nullptr ||
        noPersistenceService.try_error()->code().value() !=
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::PersistenceUnavailable))
    {
        return fail_stage("factory-persistence");
    }

    cue::Result<cue::ProjectId> mismatchedId =
        cue::ProjectId::parse("12345678-1234-4234-8234-123456789abd", a_assertContext);
    cue::Result<cue::ProjectDescriptor> mismatchedDescriptor =
        mismatchedId ? cue::create_blank_project_descriptor(
                           *mismatchedId.try_value(), "Mismatched Files Workspace Test",
                           cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}},
                           a_assertContext)
                     : cue::Result<cue::ProjectDescriptor>::failure(cue::editor_core::make_editor_core_error(
                           a_assertContext, cue::editor_core::EditorCoreError::InvalidWorkspaceRequest,
                           "Files workspace test mismatched ProjectId is unavailable"));
    cue::Result<cue::project_files::ProjectFileService> mismatchedFiles =
        create_project_file_service(project, a_assertContext);
    if (!mismatchedDescriptor || !mismatchedFiles)
    {
        return fail_stage("factory-project-input");
    }
    std::unique_ptr<cue::editor_core::EditorController> mismatchedController =
        cue::editor_core::EditorController::create(std::move(*mismatchedDescriptor.try_value()), persistence,
                                                   a_assertContext);
    cue::Result<std::unique_ptr<cue::editor_core::FilesWorkspaceService>> mismatchedService =
        cue::editor_core::FilesWorkspaceService::create(std::move(*mismatchedFiles.try_value()), *mismatchedController,
                                                        k_limits, a_assertContext);
    if (mismatchedService || mismatchedService.try_error() == nullptr ||
        mismatchedService.try_error()->code().value() !=
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidWorkspaceRequest))
    {
        return fail_stage("factory-project");
    }

    TestProject foreignProject(a_assertContext);
    cue::Result<std::unique_ptr<cue::FilesystemRoot>> foreignSourceAssets =
        foreignProject.is_created()
            ? cue::create_windows_filesystem_root(foreignProject.child_utf8(L"Assets\\Source"), a_assertContext)
            : cue::Result<std::unique_ptr<cue::FilesystemRoot>>::failure(cue::editor_core::make_editor_core_error(
                  a_assertContext, cue::editor_core::EditorCoreError::InvalidWorkspaceRequest,
                  "Files workspace foreign project is unavailable"));
    cue::Result<std::unique_ptr<cue::FilesystemRoot>> foreignSavedRoot =
        foreignProject.is_created()
            ? cue::create_windows_filesystem_root(foreignProject.child_utf8(L"Saved"), a_assertContext)
            : cue::Result<std::unique_ptr<cue::FilesystemRoot>>::failure(cue::editor_core::make_editor_core_error(
                  a_assertContext, cue::editor_core::EditorCoreError::InvalidWorkspaceRequest,
                  "Files workspace foreign project is unavailable"));
    cue::Result<cue::ProjectDescriptor> foreignSourceDescriptor = project.clone_descriptor(a_assertContext);
    cue::Result<cue::ProjectDescriptor> foreignSavedDescriptor = project.clone_descriptor(a_assertContext);
    cue::Result<cue::project_files::ProjectFileService> foreignSourceFiles =
        create_project_file_service(project, a_assertContext);
    cue::Result<cue::project_files::ProjectFileService> foreignSavedFiles =
        create_project_file_service(project, a_assertContext);
    if (!foreignSourceAssets || !foreignSavedRoot || !foreignSourceDescriptor || !foreignSavedDescriptor ||
        !foreignSourceFiles || !foreignSavedFiles)
    {
        return fail_stage("factory-root-identity-input");
    }
    cue::editor_core::ScenePersistenceServices foreignSourcePersistence(
        **foreignSourceAssets.try_value(), **savedRoot.try_value(), **schemaRegistry.try_value(),
        *valueRegistry.try_value(), sceneMigrations, componentMigrations);
    std::unique_ptr<cue::editor_core::EditorController> foreignSourceController =
        cue::editor_core::EditorController::create(std::move(*foreignSourceDescriptor.try_value()),
                                                   foreignSourcePersistence, a_assertContext);
    cue::Result<std::unique_ptr<cue::editor_core::FilesWorkspaceService>> foreignSourceService =
        cue::editor_core::FilesWorkspaceService::create(std::move(*foreignSourceFiles.try_value()),
                                                        *foreignSourceController, k_limits, a_assertContext);
    if (foreignSourceService || foreignSourceService.try_error() == nullptr ||
        foreignSourceService.try_error()->code().value() !=
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidWorkspaceRequest))
    {
        return fail_stage("factory-source-root-identity");
    }

    cue::editor_core::ScenePersistenceServices foreignSavedPersistence(
        **sourceAssets.try_value(), **foreignSavedRoot.try_value(), **schemaRegistry.try_value(),
        *valueRegistry.try_value(), sceneMigrations, componentMigrations);
    std::unique_ptr<cue::editor_core::EditorController> foreignSavedController =
        cue::editor_core::EditorController::create(std::move(*foreignSavedDescriptor.try_value()),
                                                   foreignSavedPersistence, a_assertContext);
    cue::Result<std::unique_ptr<cue::editor_core::FilesWorkspaceService>> foreignSavedService =
        cue::editor_core::FilesWorkspaceService::create(std::move(*foreignSavedFiles.try_value()),
                                                        *foreignSavedController, k_limits, a_assertContext);
    if (foreignSavedService || foreignSavedService.try_error() == nullptr ||
        foreignSavedService.try_error()->code().value() !=
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidWorkspaceRequest))
    {
        return fail_stage("factory-saved-root-identity");
    }

    cue::Result<std::unique_ptr<cue::editor_core::FilesWorkspaceService>> files =
        cue::editor_core::FilesWorkspaceService::create(std::move(*projectFiles.try_value()), *editor, k_limits,
                                                        a_assertContext);
    if (!files)
    {
        return fail_stage("files-create");
    }

    cue::editor_core::FilesWorkspaceService &service = **files.try_value();
    if (service.view_model().generation() != 1U || service.view_model().is_stale() ||
        service.view_model().directories().size() != 1U || !contains_entry(service.view_model(), "Folder") ||
        !contains_entry(service.view_model(), "Root.txt") || !service.set_expanded("Folder", true) ||
        service.view_model().directories().size() != 2U || !contains_entry(service.view_model(), "Folder/Nested.txt") ||
        !service.select("Folder/Nested.txt") || service.view_model().selection() != "Folder/Nested.txt" ||
        !service.set_search_filter("nested") || service.view_model().try_search_result() == nullptr ||
        service.view_model().try_search_result()->entries.size() != 1U ||
        service.view_model().try_search_result()->entries.front().locator != "Folder/Nested.txt")
    {
        return fail_stage("initial-view");
    }
    if (!service.set_expanded("Folder", false) || !service.set_search_filter({}) ||
        service.view_model().try_search_result() != nullptr || service.view_model().selection().has_value() ||
        !service.set_expanded("Folder", true) || !service.select("Folder/Nested.txt") ||
        !service.set_search_filter("nested") || !service.set_expanded("Folder", false) ||
        !service.set_search_filter("root") || service.view_model().selection().has_value() ||
        !service.set_search_filter("nested") || !service.set_expanded("Folder", true) ||
        !service.select("Folder/Nested.txt"))
    {
        return fail_stage("search-selection");
    }

    constexpr std::string_view content = "created";
    cue::Result<cue::project_files::ProjectFileOperationOutcome> created =
        service.create_file("Created.txt", std::as_bytes(std::span(content.data(), content.size())));
    if (!created || !is_published(*created.try_value()) || !contains_entry(service.view_model(), "Created.txt"))
    {
        return fail_stage("create");
    }
    cue::Result<cue::project_files::ProjectFileOperationOutcome> renamed = service.rename("Created.txt", "Renamed.txt");
    if (!renamed || !is_published(*renamed.try_value()) || !contains_entry(service.view_model(), "Renamed.txt"))
    {
        return fail_stage("rename");
    }
    cue::Result<cue::project_files::ProjectFileOperationOutcome> moved =
        service.move("Renamed.txt", "Folder/Moved.txt");
    if (!moved || !is_published(*moved.try_value()) || !contains_entry(service.view_model(), "Folder/Moved.txt"))
    {
        return fail_stage("move");
    }
    cue::Result<cue::project_files::ProjectFileOperationOutcome> copied = service.copy("Folder/Moved.txt", "Copy.txt");
    if (!copied || !is_published(*copied.try_value()) || !contains_entry(service.view_model(), "Copy.txt") ||
        service.view_model().selection() != "Folder/Nested.txt")
    {
        return fail_stage("copy");
    }

    const std::uint64_t beforeConflictGeneration = service.view_model().generation();
    cue::Result<cue::project_files::ProjectFileOperationOutcome> conflict =
        service.create_file("Copy.txt", std::as_bytes(std::span(content.data(), content.size())));
    if (!conflict || *conflict.try_value() != cue::project_files::ProjectFileOperationOutcome::NotCommitted ||
        service.view_model().operation_state() != cue::editor_core::FilesOperationState::Failed ||
        service.view_model().try_error() == nullptr || service.view_model().generation() <= beforeConflictGeneration ||
        !contains_entry(service.view_model(), "Copy.txt"))
    {
        return fail_stage("conflict");
    }

    cue::Result<cue::scene::SceneAssetId> sceneId =
        cue::scene::SceneAssetId::parse("00000000-0000-4000-8000-000000000401", a_assertContext);
    std::optional<cue::scene::SceneDocument> scene;
    if (sceneId)
    {
        scene.emplace(cue::scene::SceneDocument::create(std::move(*sceneId.try_value()), a_assertContext));
    }
    cue::Result<cue::RelativePath> openLocator = cue::RelativePath::parse("Folder/Open.cuescene", a_assertContext);
    if (!scene.has_value() || !openLocator ||
        !editor->open_document(std::move(*scene), std::move(*openLocator.try_value()), true))
    {
        return fail_stage("open-document");
    }
    cue::Result<cue::project_files::ProjectFileOperationOutcome> blocked = service.delete_entry("Folder");
    if (blocked || service.view_model().try_error() == nullptr ||
        service.view_model().operation_state() != cue::editor_core::FilesOperationState::Failed ||
        service.view_model().try_last_operation() != nullptr ||
        service.view_model().try_error()->code().value() !=
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::WorkspaceEntryInUse) ||
        !contains_entry(service.view_model(), "Folder"))
    {
        return fail_stage("open-guard");
    }
    if (!service.refresh() || service.view_model().operation_state() != cue::editor_core::FilesOperationState::Idle ||
        service.view_model().try_error() != nullptr || service.view_model().selection() != "Folder/Nested.txt")
    {
        return fail_stage("failure-dismiss");
    }

    cue::Result<cue::project_files::ProjectFileOperationOutcome> deleted = service.delete_entry("Copy.txt");
    if (!deleted || !is_published(*deleted.try_value()) || contains_entry(service.view_model(), "Copy.txt") ||
        service.view_model().recovery_entries().empty())
    {
        return fail_stage("delete");
    }
    const std::string operationId(service.view_model().recovery_entries().front().operationId);
    cue::Result<cue::project_files::ProjectFileOperationOutcome> restored = service.restore(operationId);
    if (!restored || !is_published(*restored.try_value()) || !contains_entry(service.view_model(), "Copy.txt"))
    {
        return fail_stage("restore");
    }

    if (!service.select("Root.txt") || DeleteFileW(project.child(L"Assets\\Source\\Root.txt").c_str()) == FALSE ||
        !wait_for_external_refresh(service, "Root.txt") || service.view_model().selection().has_value() ||
        contains_entry(service.view_model(), "Root.txt"))
    {
        return fail_stage("external");
    }

    bool wrongThreadRefreshRejected = false;
    bool wrongThreadClearRejected = false;
    std::thread wrongThread(
        /// @brief Owner Thread外からのRefreshとSelection変更が副作用なく拒否されるか確認する
        [&service, &wrongThreadRefreshRejected, &wrongThreadClearRejected]() noexcept
        {
            wrongThreadRefreshRejected = !service.refresh();
            wrongThreadClearRejected = !service.clear_selection();
        });
    wrongThread.join();
    if (!wrongThreadRefreshRejected || !wrongThreadClearRejected || !service.stop() || !service.stop())
    {
        return fail_stage("stop");
    }
    cue::Result<bool> pollAfterStop = service.poll_external_changes();
    return !pollAfterStop && service.view_model().is_stale() && pollAfterStop.try_error() != nullptr &&
           pollAfterStop.try_error()->code().value() ==
               static_cast<std::int64_t>(cue::editor_core::EditorCoreError::WorkspaceUnavailable);
}
} // namespace

/// @brief Files Workspace ViewModelとProjectFileService接続のHeadless Contractを実行する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    const cue::AssertContext assertContext(logger, fatalHandler);
    return test_files_workspace(assertContext) ? 0 : 1;
}
