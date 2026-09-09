#include <Cue/Editor/Windows/EditorSession.h>

#include <Cue/EditorCore/FilesWorkspace.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/IO/Windows/WindowsWorkspaceFilesystem.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/ProjectFiles/Service.h>
#include <Cue/Scene/ComponentData.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Schema/Registry.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>
#include <bcrypt.h>

namespace
{
constexpr cue::editor_core::FilesWorkspaceLimits k_filesLimits{
    cue::TraversalLimits{32U, 4096U, 1024U, 1024U * 1024U},
    cue::ContentVerificationLimits{1024U * 1024U, 16U * 1024U * 1024U},
    cue::WorkspaceWatchLimits{256U, 64U * 1024U, 128U, 25U, 250U}};

/// @brief Editor Session失敗を安定Domainへ分類する
[[nodiscard]] cue::Error make_session_error(const cue::AssertContext &a_context,
                                            cue::editor::WindowsEditorSessionError a_code,
                                            std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_context.fatal_handler(), "Cue.Editor.Windows", static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_context.fatal_handler(), std::move(code), a_summary);
}

/// @brief 下位Moduleの原因を保持してEditor Session失敗へ再分類する
[[nodiscard]] cue::Error reclassify_session_error(const cue::AssertContext &a_context,
                                                  cue::editor::WindowsEditorSessionError a_code,
                                                  std::string_view a_summary, cue::Error a_cause) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_context.fatal_handler(), "Cue.Editor.Windows", static_cast<std::int64_t>(a_code));
    return cue::Error::reclassify(a_context.fatal_handler(), std::move(code), a_summary, std::move(a_cause));
}

/// @brief Editor Session構築中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_session_exception(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("Windows Editor session operation failed unexpectedly");
    std::abort();
}

/// @brief UTF-8 PathをWindows PathへStrict変換する
[[nodiscard]] cue::Result<std::wstring> to_utf16_path(std::string_view a_path,
                                                      const cue::AssertContext &a_context) noexcept
{
    std::wstring converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_utf8_to_windows_utf16(a_path, converted, a_context.fatal_handler());
    if (result.status != cue::WindowsUtfConversionStatus::Success || converted.empty() ||
        converted.find(L'\0') != std::wstring::npos)
    {
        return cue::Result<std::wstring>::failure(
            make_session_error(a_context, cue::editor::WindowsEditorSessionError::InvalidLaunchParameters,
                               "Editor launch path is not valid UTF-8"));
    }
    return cue::Result<std::wstring>::success(std::move(converted));
}

/// @brief Windows PathをProcess間で保持するUTF-8へStrict変換する
[[nodiscard]] cue::Result<std::string> to_utf8_path(std::wstring_view a_path,
                                                    const cue::AssertContext &a_context) noexcept
{
    std::string converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_windows_utf16_to_utf8(a_path, converted, a_context.fatal_handler());
    if (result.status != cue::WindowsUtfConversionStatus::Success || converted.empty())
    {
        return cue::Result<std::string>::failure(
            make_session_error(a_context, cue::editor::WindowsEditorSessionError::InvalidLaunchParameters,
                               "Editor launch path could not be converted to UTF-8"));
    }
    return cue::Result<std::string>::success(std::move(converted));
}

/// @brief Descriptor LocatorをProject Rootと固定Descriptor名へ分解する
[[nodiscard]] cue::Result<std::string> project_locator_from_descriptor(std::string_view a_descriptorLocator,
                                                                       const cue::AssertContext &a_context) noexcept
{
    cue::Result<std::wstring> converted = to_utf16_path(a_descriptorLocator, a_context);
    if (!converted)
    {
        return cue::Result<std::string>::failure(std::move(*converted.try_error()));
    }
    try
    {
        const std::filesystem::path descriptor(*converted.try_value());
        const std::wstring filename = descriptor.filename().native();
        if (!descriptor.is_absolute() || descriptor.root_path() == descriptor ||
            CompareStringOrdinal(filename.c_str(), static_cast<int>(filename.size()), L"CueProject.json", 15, TRUE) !=
                CSTR_EQUAL)
        {
            return cue::Result<std::string>::failure(
                make_session_error(a_context, cue::editor::WindowsEditorSessionError::InvalidLaunchParameters,
                                   "Project descriptor locator must be an absolute CueProject.json path"));
        }
        return to_utf8_path(descriptor.parent_path().native(), a_context);
    }
    catch (...)
    {
        terminate_session_exception(a_context);
    }
}

/// @brief Project Rootと検証済み相対RootをWindows Absolute Locatorへ合成する
[[nodiscard]] cue::Result<std::string> compose_root_locator(std::string_view a_projectLocator,
                                                            const cue::RelativePath &a_relativeRoot,
                                                            const cue::AssertContext &a_context) noexcept
{
    cue::Result<std::wstring> project = to_utf16_path(a_projectLocator, a_context);
    cue::Result<std::wstring> relative = to_utf16_path(a_relativeRoot.text(), a_context);
    if (!project || !relative)
    {
        return cue::Result<std::string>::failure(!project ? std::move(*project.try_error())
                                                          : std::move(*relative.try_error()));
    }
    try
    {
        const std::filesystem::path combined =
            (std::filesystem::path(*project.try_value()) / std::filesystem::path(*relative.try_value()))
                .lexically_normal();
        return to_utf8_path(combined.native(), a_context);
    }
    catch (...)
    {
        terminate_session_exception(a_context);
    }
}

/// @brief BCryptのSystem RNGからRFC 4122 Version 4候補を供給する
class WindowsSceneIdentitySource final : public cue::scene::SceneIdentitySource
{
  public:
    /// @brief RNG失敗時の終端先をIdentity Source全寿命へ保持する
    explicit WindowsSceneIdentitySource(const cue::AssertContext &a_context) noexcept : m_assertContext(&a_context)
    {
    }

    /// @brief 非所有診断Contextだけを破棄する
    ~WindowsSceneIdentitySource() override = default;

    /// @brief System RNGからUUID Version 4のVariantとVersionを設定して返す
    [[nodiscard]] cue::scene::IdentityBytes next_identity() noexcept override
    {
        cue::scene::IdentityBytes bytes{};
        const NTSTATUS status =
            BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0)
        {
            m_assertContext->fatal_handler().terminate("Windows Scene identity generation failed");
            std::abort();
        }
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
        return bytes;
    }

  private:
    const cue::AssertContext *m_assertContext;
};

/// @brief BCryptのSystem RNGからProject File操作用UUID Version 4を発行する
class WindowsProjectFileOperationIdSource final : public cue::project_files::ProjectFileOperationIdSource
{
  public:
    /// @brief RNG失敗時の終端先をOperation ID Source全寿命へ保持する
    explicit WindowsProjectFileOperationIdSource(const cue::AssertContext &a_context) noexcept
        : m_assertContext(&a_context)
    {
    }

    /// @brief 非所有診断Contextだけを破棄する
    ~WindowsProjectFileOperationIdSource() override = default;

    /// @brief System RNGからlowercase UUID Version 4を生成して返す
    [[nodiscard]] cue::Result<std::string> next_operation_id() noexcept override
    {
        std::array<std::uint8_t, 16U> bytes{};
        const NTSTATUS status =
            BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0)
        {
            m_assertContext->fatal_handler().terminate("Windows project file operation identity generation failed");
            std::abort();
        }
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

        constexpr std::string_view k_digits = "0123456789abcdef";
        std::array<char, 36U> text{};
        std::size_t output = 0U;
        for (std::size_t index = 0U; index < bytes.size(); ++index)
        {
            if (index == 4U || index == 6U || index == 8U || index == 10U)
            {
                text[output++] = '-';
            }
            text[output++] = k_digits[bytes[index] >> 4U];
            text[output++] = k_digits[bytes[index] & 0x0fU];
        }
        try
        {
            return cue::Result<std::string>::success(std::string(text.data(), text.size()));
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate("Windows project file operation identity allocation failed");
            std::abort();
        }
    }

  private:
    const cue::AssertContext *m_assertContext;
};
} // namespace

namespace cue::editor
{
WindowsEditorSession::WindowsEditorSession(std::string a_projectLocator, std::unique_ptr<FilesystemRoot> a_projectRoot,
                                           std::unique_ptr<FilesystemRoot> a_sourceAssetsRoot,
                                           std::unique_ptr<FilesystemRoot> a_savedRoot,
                                           const AssertContext &a_assertContext) noexcept
    : m_projectLocator(std::move(a_projectLocator)), m_projectRoot(std::move(a_projectRoot)),
      m_sourceAssetsRoot(std::move(a_sourceAssetsRoot)), m_savedRoot(std::move(a_savedRoot)),
      m_assertContext(&a_assertContext)
{
}

WindowsEditorSession::~WindowsEditorSession() noexcept
{
    if (m_filesWorkspace != nullptr)
    {
        Result<void> stopped = m_filesWorkspace->stop();
        if (!stopped)
        {
            static_cast<void>(m_assertContext->logger().log(
                LogLevel::Error, "Files workspace watcher could not be stopped", std::move(*stopped.try_error())));
        }
    }
}

Result<std::unique_ptr<WindowsEditorSession>> WindowsEditorSession::create(
    WindowsEditorLaunchParameters a_parameters, WindowsEditorEngineConfiguration a_configuration,
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_parameters.protocolVersion != k_editorLaunchProtocolVersion ||
            a_parameters.projectDescriptorLocator.empty() || a_parameters.expectedProjectId.empty() ||
            a_parameters.engineCompatibilityId.empty() || a_configuration.supportedProjectFormatVersion == 0U)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(
                make_session_error(a_assertContext, WindowsEditorSessionError::InvalidLaunchParameters,
                                   "Editor launch parameters are incomplete or unsupported"));
        }

        Result<std::string> projectLocator =
            project_locator_from_descriptor(a_parameters.projectDescriptorLocator, a_assertContext);
        if (!projectLocator)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(std::move(*projectLocator.try_error()));
        }
        Result<std::unique_ptr<FilesystemRoot>> projectRoot =
            create_windows_filesystem_root(*projectLocator.try_value(), a_assertContext);
        if (!projectRoot)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(
                reclassify_session_error(a_assertContext, WindowsEditorSessionError::ProjectOpenFailed,
                                         "Project root could not be opened", std::move(*projectRoot.try_error())));
        }
        Result<ProjectDescriptor> descriptor = load_project_descriptor(**projectRoot.try_value(), a_assertContext);
        if (!descriptor)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(reclassify_session_error(
                a_assertContext, WindowsEditorSessionError::ProjectOpenFailed,
                "Project descriptor could not be loaded by the shared parser", std::move(*descriptor.try_error())));
        }
        Result<ProjectId> expectedProjectId = ProjectId::parse(a_parameters.expectedProjectId, a_assertContext);
        if (!expectedProjectId || descriptor.try_value()->project_id() != *expectedProjectId.try_value())
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(
                make_session_error(a_assertContext, WindowsEditorSessionError::ProjectIdentityMismatch,
                                   "Project descriptor identity differs from the launch request"));
        }
        const std::string compatibilityId =
            make_engine_compatibility_id(descriptor.try_value()->engine_compatibility(), a_assertContext);
        if (compatibilityId != a_parameters.engineCompatibilityId)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(
                make_session_error(a_assertContext, WindowsEditorSessionError::ProjectCompatibilityMismatch,
                                   "Project compatibility differs from the launch request"));
        }
        Result<ProjectCompatibilityReport> compatibility = evaluate_project_compatibility(
            descriptor.try_value()->schema_version(), a_configuration.supportedProjectFormatVersion,
            descriptor.try_value()->engine_compatibility(), a_configuration.currentEngineVersion,
            a_configuration.capabilityProfile, a_configuration.capabilitySnapshot, a_assertContext);
        if (!compatibility)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(reclassify_session_error(
                a_assertContext, WindowsEditorSessionError::ProjectUnsupported,
                "Project compatibility could not be evaluated", std::move(*compatibility.try_error())));
        }
        if (!compatibility.try_value()->can_open())
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(
                make_session_error(a_assertContext, WindowsEditorSessionError::ProjectUnsupported,
                                   "Project cannot be opened by the current Editor"));
        }

        Result<std::string> sourceLocator = compose_root_locator(
            *projectLocator.try_value(), descriptor.try_value()->roots().source_assets(), a_assertContext);
        Result<std::string> savedLocator =
            compose_root_locator(*projectLocator.try_value(), descriptor.try_value()->roots().saved(), a_assertContext);
        if (!sourceLocator || !savedLocator)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(
                !sourceLocator ? std::move(*sourceLocator.try_error()) : std::move(*savedLocator.try_error()));
        }
        Result<std::unique_ptr<FilesystemRoot>> sourceRoot =
            create_windows_filesystem_root(*sourceLocator.try_value(), a_assertContext);
        if (!sourceRoot)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(reclassify_session_error(
                a_assertContext, WindowsEditorSessionError::SourceAssetsOpenFailed,
                "Project Source Assets root could not be opened", std::move(*sourceRoot.try_error())));
        }

        Result<EntryType> savedEntry = (*projectRoot.try_value())->query_entry(descriptor.try_value()->roots().saved());
        if (!savedEntry)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(reclassify_session_error(
                a_assertContext, WindowsEditorSessionError::SavedRootOpenFailed,
                "Project Saved root could not be inspected", std::move(*savedEntry.try_error())));
        }
        if (*savedEntry.try_value() == EntryType::Missing)
        {
            Result<void> created =
                (*projectRoot.try_value())->create_directories(descriptor.try_value()->roots().saved());
            if (!created)
            {
                return Result<std::unique_ptr<WindowsEditorSession>>::failure(reclassify_session_error(
                    a_assertContext, WindowsEditorSessionError::SavedRootOpenFailed,
                    "Project Saved root could not be created", std::move(*created.try_error())));
            }
        }
        else if (*savedEntry.try_value() != EntryType::Directory)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(
                make_session_error(a_assertContext, WindowsEditorSessionError::SavedRootOpenFailed,
                                   "Project Saved root is not a supported directory"));
        }
        Result<std::unique_ptr<FilesystemRoot>> savedRoot =
            create_windows_filesystem_root(*savedLocator.try_value(), a_assertContext);
        if (!savedRoot)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(
                reclassify_session_error(a_assertContext, WindowsEditorSessionError::SavedRootOpenFailed,
                                         "Project Saved root could not be opened", std::move(*savedRoot.try_error())));
        }

        std::unique_ptr<WindowsEditorSession> session(new WindowsEditorSession(
            std::move(*projectLocator.try_value()), std::move(*projectRoot.try_value()),
            std::move(*sourceRoot.try_value()), std::move(*savedRoot.try_value()), a_assertContext));
        Result<void> initialized =
            session->initialize(std::move(*descriptor.try_value()), a_parameters.initialSceneLocator,
                                a_parameters.expectedInitialSceneAssetId);
        if (!initialized)
        {
            return Result<std::unique_ptr<WindowsEditorSession>>::failure(std::move(*initialized.try_error()));
        }
        return Result<std::unique_ptr<WindowsEditorSession>>::success(std::move(session));
    }
    catch (...)
    {
        terminate_session_exception(a_assertContext);
    }
}

Result<void> WindowsEditorSession::initialize(ProjectDescriptor a_descriptor,
                                              const std::optional<std::string> &a_initialSceneLocator,
                                              const std::optional<std::string> &a_expectedInitialSceneAssetId) noexcept
{
    try
    {
        m_schemaIdentitySource = std::make_unique<schema::SchemaRegistryIdentitySource>();
        schema::SchemaRegistryBuilder schemaBuilder(*m_schemaIdentitySource, *m_assertContext);
        Result<std::unique_ptr<schema::SchemaRegistry>> schemaRegistry = schemaBuilder.seal();
        if (!schemaRegistry)
        {
            return Result<void>::failure(reclassify_session_error(
                *m_assertContext, WindowsEditorSessionError::SchemaInitializationFailed,
                "Editor Schema Registry could not be initialized", std::move(*schemaRegistry.try_error())));
        }
        m_schemaRegistry = std::move(*schemaRegistry.try_value());
        Result<scene::ComponentValueSchemaRegistry> valueRegistry =
            scene::ComponentValueSchemaRegistry::create({}, *m_schemaRegistry, *m_assertContext);
        if (!valueRegistry)
        {
            return Result<void>::failure(reclassify_session_error(
                *m_assertContext, WindowsEditorSessionError::SchemaInitializationFailed,
                "Editor Component Value Registry could not be initialized", std::move(*valueRegistry.try_error())));
        }
        m_valueSchemaRegistry =
            std::make_unique<scene::ComponentValueSchemaRegistry>(std::move(*valueRegistry.try_value()));
        m_sceneMigrations = std::make_unique<scene::SceneMigrationRegistry>();
        m_componentMigrations = std::make_unique<scene::ComponentMigrationRegistry>();
        m_sceneIdentitySource = std::make_unique<WindowsSceneIdentitySource>(*m_assertContext);
        editor_core::ScenePersistenceServices persistence(*m_sourceAssetsRoot, *m_savedRoot, *m_schemaRegistry,
                                                          *m_valueSchemaRegistry, *m_sceneMigrations,
                                                          *m_componentMigrations);
        m_controller = editor_core::EditorController::create(std::move(a_descriptor), persistence, *m_assertContext);

        Result<std::unique_ptr<WorkspaceFilesystem>> workspace =
            create_windows_workspace_filesystem(m_projectLocator, *m_assertContext);
        if (!workspace)
        {
            return Result<void>::failure(reclassify_session_error(
                *m_assertContext, WindowsEditorSessionError::ProjectFilesInitializationFailed,
                "Project files workspace could not be opened", std::move(*workspace.try_error())));
        }
        std::unique_ptr<project_files::ProjectFileOperationIdSource> operationIds =
            std::make_unique<WindowsProjectFileOperationIdSource>(*m_assertContext);
        Result<project_files::ProjectFileService> projectFiles = project_files::ProjectFileService::create(
            m_controller->session().project_descriptor(), std::move(*workspace.try_value()), std::move(operationIds),
            *m_assertContext);
        if (!projectFiles)
        {
            return Result<void>::failure(reclassify_session_error(
                *m_assertContext, WindowsEditorSessionError::ProjectFilesInitializationFailed,
                "Project file service could not be initialized", std::move(*projectFiles.try_error())));
        }
        Result<std::unique_ptr<editor_core::FilesWorkspaceService>> filesWorkspace =
            editor_core::FilesWorkspaceService::create(std::move(*projectFiles.try_value()), *m_controller,
                                                       k_filesLimits, *m_assertContext);
        if (!filesWorkspace)
        {
            return Result<void>::failure(reclassify_session_error(
                *m_assertContext, WindowsEditorSessionError::ProjectFilesInitializationFailed,
                "Files workspace service could not be initialized", std::move(*filesWorkspace.try_error())));
        }
        m_filesWorkspace = std::move(*filesWorkspace.try_value());

        if (a_initialSceneLocator.has_value())
        {
            Result<RelativePath> locator = RelativePath::parse(*a_initialSceneLocator, *m_assertContext);
            if (!locator)
            {
                return Result<void>::failure(
                    reclassify_session_error(*m_assertContext, WindowsEditorSessionError::SceneOpenFailed,
                                             "Initial scene locator is invalid", std::move(*locator.try_error())));
            }
            Result<editor_core::EditorDocumentId> opened = open_scene(std::move(*locator.try_value()));
            if (!opened)
            {
                return Result<void>::failure(std::move(*opened.try_error()));
            }
            if (a_expectedInitialSceneAssetId.has_value())
            {
                Result<scene::SceneAssetId> expectedSceneAssetId =
                    scene::SceneAssetId::parse(*a_expectedInitialSceneAssetId, *m_assertContext);
                const editor_core::EditorDocument *document =
                    expectedSceneAssetId ? m_controller->session().find_document(*opened.try_value()) : nullptr;
                if (!expectedSceneAssetId || document == nullptr ||
                    document->scene_document().scene_asset_id() != *expectedSceneAssetId.try_value())
                {
                    return Result<void>::failure(
                        make_session_error(*m_assertContext, WindowsEditorSessionError::SceneOpenFailed,
                                           "Initial scene identity differs from the Project Descriptor"));
                }
            }
        }
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_session_exception(*m_assertContext);
    }
}

Result<void> WindowsEditorSession::require_project_only_state() const noexcept
{
    if (m_activeDocumentId.has_value() || m_preparedDocumentId.has_value())
    {
        return Result<void>::failure(
            make_session_error(*m_assertContext, WindowsEditorSessionError::InvalidSessionState,
                               "Close or discard every session scene before opening another scene"));
    }
    return Result<void>::success();
}

Result<editor_core::EditorDocumentId> WindowsEditorSession::create_scene(RelativePath a_locator) noexcept
{
    reconcile_active_document();
    Result<void> projectOnly = require_project_only_state();
    if (!projectOnly)
    {
        return Result<editor_core::EditorDocumentId>::failure(std::move(*projectOnly.try_error()));
    }
    Result<editor_core::EditorDocumentId> prepared = prepare_new_scene(std::move(a_locator));
    if (!prepared)
    {
        return prepared;
    }
    m_activeDocumentId = *m_preparedDocumentId;
    m_preparedDocumentId.reset();
    return prepared;
}

Result<editor_core::EditorDocumentId> WindowsEditorSession::prepare_new_scene(RelativePath a_locator) noexcept
{
    reconcile_active_document();
    if (m_preparedDocumentId.has_value())
    {
        return Result<editor_core::EditorDocumentId>::failure(
            make_session_error(*m_assertContext, WindowsEditorSessionError::InvalidSessionState,
                               "A prepared scene must be activated or discarded before preparing another scene"));
    }
    Result<EntryType> entry = m_sourceAssetsRoot->query_entry(a_locator);
    if (!entry)
    {
        return Result<editor_core::EditorDocumentId>::failure(
            reclassify_session_error(*m_assertContext, WindowsEditorSessionError::SceneOpenFailed,
                                     "New scene destination could not be inspected", std::move(*entry.try_error())));
    }
    if (*entry.try_value() != EntryType::Missing)
    {
        return Result<editor_core::EditorDocumentId>::failure(make_session_error(
            *m_assertContext, WindowsEditorSessionError::SceneOpenFailed, "New scene destination already exists"));
    }
    Result<scene::SceneAssetId> sceneId = scene::SceneAssetId::generate(*m_sceneIdentitySource, *m_assertContext);
    if (!sceneId)
    {
        return Result<editor_core::EditorDocumentId>::failure(reclassify_session_error(
            *m_assertContext, WindowsEditorSessionError::SceneOpenFailed,
            "A stable scene identity could not be generated", std::move(*sceneId.try_error())));
    }
    scene::SceneDocument scene = scene::SceneDocument::create(std::move(*sceneId.try_value()), *m_assertContext);
    Result<editor_core::EditorDocumentId> opened =
        m_controller->open_document(std::move(scene), std::move(a_locator), false);
    if (!opened)
    {
        return Result<editor_core::EditorDocumentId>::failure(
            reclassify_session_error(*m_assertContext, WindowsEditorSessionError::SceneOpenFailed,
                                     "New scene could not be opened", std::move(*opened.try_error())));
    }
    m_preparedDocumentId = *opened.try_value();
    return opened;
}

Result<editor_core::EditorDocumentId> WindowsEditorSession::open_scene(RelativePath a_locator) noexcept
{
    reconcile_active_document();
    Result<void> projectOnly = require_project_only_state();
    if (!projectOnly)
    {
        return Result<editor_core::EditorDocumentId>::failure(std::move(*projectOnly.try_error()));
    }
    Result<editor_core::EditorDocumentId> prepared = prepare_open_scene(std::move(a_locator));
    if (!prepared)
    {
        return prepared;
    }
    m_activeDocumentId = *m_preparedDocumentId;
    m_preparedDocumentId.reset();
    return prepared;
}

Result<editor_core::EditorDocumentId> WindowsEditorSession::prepare_open_scene(RelativePath a_locator) noexcept
{
    reconcile_active_document();
    if (m_preparedDocumentId.has_value())
    {
        return Result<editor_core::EditorDocumentId>::failure(
            make_session_error(*m_assertContext, WindowsEditorSessionError::InvalidSessionState,
                               "A prepared scene must be activated or discarded before preparing another scene"));
    }
    Result<editor_core::EditorDocumentId> opened = m_controller->open_document_from_storage(std::move(a_locator));
    if (!opened)
    {
        return Result<editor_core::EditorDocumentId>::failure(
            reclassify_session_error(*m_assertContext, WindowsEditorSessionError::SceneOpenFailed,
                                     "Scene could not be opened from Source Assets", std::move(*opened.try_error())));
    }
    m_preparedDocumentId = *opened.try_value();
    return opened;
}

Result<editor_core::DocumentCloseState> WindowsEditorSession::request_activate_prepared_scene() noexcept
{
    reconcile_active_document();
    if (!m_preparedDocumentId.has_value())
    {
        return Result<editor_core::DocumentCloseState>::failure(
            make_session_error(*m_assertContext, WindowsEditorSessionError::InvalidSessionState,
                               "No prepared scene is available to activate"));
    }
    if (!m_activeDocumentId.has_value())
    {
        m_activeDocumentId = *m_preparedDocumentId;
        m_preparedDocumentId.reset();
        return Result<editor_core::DocumentCloseState>::success(editor_core::DocumentCloseState::Closed);
    }
    Result<editor_core::DocumentCloseState> state = m_controller->request_close(*m_activeDocumentId);
    reconcile_active_document();
    return state;
}

Result<void> WindowsEditorSession::discard_prepared_scene() noexcept
{
    if (!m_preparedDocumentId.has_value())
    {
        return Result<void>::failure(make_session_error(*m_assertContext,
                                                        WindowsEditorSessionError::InvalidSessionState,
                                                        "No prepared scene is available to discard"));
    }
    const editor_core::EditorDocumentId preparedId = *m_preparedDocumentId;
    Result<editor_core::DocumentCloseState> state = m_controller->request_close(preparedId);
    if (!state)
    {
        return Result<void>::failure(std::move(*state.try_error()));
    }
    if (*state.try_value() == editor_core::DocumentCloseState::AwaitingDecision)
    {
        state = m_controller->respond_to_close(preparedId, editor_core::CloseDecision::Discard);
        if (!state)
        {
            return Result<void>::failure(std::move(*state.try_error()));
        }
    }
    if (*state.try_value() != editor_core::DocumentCloseState::Closed)
    {
        return Result<void>::failure(make_session_error(*m_assertContext,
                                                        WindowsEditorSessionError::InvalidSessionState,
                                                        "Prepared scene did not reach the closed state"));
    }
    m_preparedDocumentId.reset();
    return Result<void>::success();
}

Result<editor_core::EditorDocumentId> WindowsEditorSession::open_recovery_scene(std::string_view a_sceneId) noexcept
{
    reconcile_active_document();
    Result<void> projectOnly = require_project_only_state();
    if (!projectOnly)
    {
        return Result<editor_core::EditorDocumentId>::failure(std::move(*projectOnly.try_error()));
    }
    Result<editor_core::EditorDocumentId> opened = m_controller->open_document_from_recovery(a_sceneId);
    if (!opened)
    {
        return Result<editor_core::EditorDocumentId>::failure(
            reclassify_session_error(*m_assertContext, WindowsEditorSessionError::SceneOpenFailed,
                                     "Recovery scene could not be opened", std::move(*opened.try_error())));
    }
    m_activeDocumentId = *opened.try_value();
    return opened;
}

Result<std::vector<editor_core::RecoveryCandidateInspection>> WindowsEditorSession::list_recovery_candidates() noexcept
{
    return m_controller->list_recovery_candidates();
}

Result<scene::SceneSaveOutcome> WindowsEditorSession::save_active_scene() noexcept
{
    return save_active_scene_impl(false);
}

Result<scene::SceneSaveOutcome> WindowsEditorSession::save_active_scene_overwriting_existing_destination() noexcept
{
    return save_active_scene_impl(true);
}

Result<scene::SceneSaveOutcome> WindowsEditorSession::save_active_scene_as_new(RelativePath a_locator) noexcept
{
    return save_active_scene_as_impl(std::move(a_locator), false);
}

Result<scene::SceneSaveOutcome> WindowsEditorSession::save_active_scene_as_overwriting(RelativePath a_locator) noexcept
{
    return save_active_scene_as_impl(std::move(a_locator), true);
}

Result<void> WindowsEditorSession::autosave_active_scene_recovery() noexcept
{
    reconcile_active_document();
    if (!m_activeDocumentId.has_value())
    {
        return Result<void>::failure(make_session_error(*m_assertContext,
                                                        WindowsEditorSessionError::InvalidSessionState,
                                                        "No active scene is available for recovery autosave"));
    }
    return m_controller->autosave_recovery(*m_activeDocumentId);
}

Result<scene::SceneSaveOutcome> WindowsEditorSession::save_active_scene_impl(bool a_allowExistingDestination) noexcept
{
    reconcile_active_document();
    if (!m_activeDocumentId.has_value())
    {
        return Result<scene::SceneSaveOutcome>::failure(make_session_error(
            *m_assertContext, WindowsEditorSessionError::InvalidSessionState, "No active scene is available to save"));
    }
    const editor_core::EditorDocument *document = m_controller->session().find_document(*m_activeDocumentId);
    if (!document->has_saved_destination())
    {
        Result<void> created = ensure_scene_parent_directory(document->scene_locator());
        if (!created)
        {
            return Result<scene::SceneSaveOutcome>::failure(std::move(*created.try_error()));
        }
    }
    Result<scene::SceneSaveOutcome> saved =
        document->has_saved_destination()
            ? m_controller->save_document(*m_activeDocumentId)
            : (a_allowExistingDestination
                   ? m_controller->save_document_as(*m_activeDocumentId, RelativePath(document->scene_locator()))
                   : m_controller->save_document_as_new(*m_activeDocumentId, RelativePath(document->scene_locator())));
    reconcile_active_document();
    return saved;
}

Result<scene::SceneSaveOutcome> WindowsEditorSession::save_active_scene_as_impl(
    RelativePath a_locator, bool a_allowExistingDestination) noexcept
{
    reconcile_active_document();
    if (!m_activeDocumentId.has_value())
    {
        return Result<scene::SceneSaveOutcome>::failure(
            make_session_error(*m_assertContext, WindowsEditorSessionError::InvalidSessionState,
                               "No active scene is available to save as another destination"));
    }
    Result<void> created = ensure_scene_parent_directory(a_locator);
    if (!created)
    {
        return Result<scene::SceneSaveOutcome>::failure(std::move(*created.try_error()));
    }
    Result<scene::SceneSaveOutcome> saved =
        a_allowExistingDestination ? m_controller->save_document_as(*m_activeDocumentId, std::move(a_locator))
                                   : m_controller->save_document_as_new(*m_activeDocumentId, std::move(a_locator));
    reconcile_active_document();
    return saved;
}

Result<void> WindowsEditorSession::ensure_scene_parent_directory(const RelativePath &a_locator) noexcept
{
    const std::string_view locator = a_locator.text();
    const std::size_t separator = locator.rfind('/');
    if (separator == std::string_view::npos)
    {
        return Result<void>::success();
    }
    Result<RelativePath> parent = RelativePath::parse(locator.substr(0U, separator), *m_assertContext);
    if (!parent)
    {
        return Result<void>::failure(
            reclassify_session_error(*m_assertContext, WindowsEditorSessionError::SceneSaveFailed,
                                     "Scene destination directory is invalid", std::move(*parent.try_error())));
    }
    Result<void> created = m_sourceAssetsRoot->create_directories(*parent.try_value());
    if (!created)
    {
        return Result<void>::failure(reclassify_session_error(
            *m_assertContext, WindowsEditorSessionError::SceneSaveFailed,
            "Scene destination directory could not be created", std::move(*created.try_error())));
    }
    return Result<void>::success();
}

Result<editor_core::DocumentStateId> WindowsEditorSession::reload_active_scene() noexcept
{
    reconcile_active_document();
    if (!m_activeDocumentId.has_value())
    {
        return Result<editor_core::DocumentStateId>::failure(
            make_session_error(*m_assertContext, WindowsEditorSessionError::InvalidSessionState,
                               "No active scene is available to reload"));
    }
    return m_controller->reload_document(*m_activeDocumentId);
}

Result<scene::SceneSaveStatus> WindowsEditorSession::retry_uncertain_save_active_scene() noexcept
{
    reconcile_active_document();
    if (!m_activeDocumentId.has_value())
    {
        return Result<scene::SceneSaveStatus>::failure(
            make_session_error(*m_assertContext, WindowsEditorSessionError::InvalidSessionState,
                               "No active scene has an uncertain save to retry"));
    }
    Result<scene::SceneSaveStatus> status = m_controller->retry_uncertain_save(*m_activeDocumentId);
    reconcile_active_document();
    return status;
}

Result<void> WindowsEditorSession::discard_uncertain_save_active_scene() noexcept
{
    reconcile_active_document();
    if (!m_activeDocumentId.has_value())
    {
        return Result<void>::failure(make_session_error(*m_assertContext,
                                                        WindowsEditorSessionError::InvalidSessionState,
                                                        "No active scene has an uncertain save to discard"));
    }
    Result<void> discarded = m_controller->discard_uncertain_save(*m_activeDocumentId);
    reconcile_active_document();
    return discarded;
}

Result<editor_core::DocumentCloseState> WindowsEditorSession::request_close() noexcept
{
    reconcile_active_document();
    if (!m_activeDocumentId.has_value())
    {
        return Result<editor_core::DocumentCloseState>::success(editor_core::DocumentCloseState::Closed);
    }
    Result<editor_core::DocumentCloseState> state = m_controller->request_close(*m_activeDocumentId);
    reconcile_active_document();
    return state;
}

Result<editor_core::DocumentCloseState> WindowsEditorSession::respond_to_close(
    editor_core::CloseDecision a_decision) noexcept
{
    reconcile_active_document();
    if (!m_activeDocumentId.has_value())
    {
        return Result<editor_core::DocumentCloseState>::failure(
            make_session_error(*m_assertContext, WindowsEditorSessionError::InvalidSessionState,
                               "No active scene is awaiting a close decision"));
    }
    Result<editor_core::DocumentCloseState> state = m_controller->respond_to_close(*m_activeDocumentId, a_decision);
    reconcile_active_document();
    return state;
}

editor_core::EditorController &WindowsEditorSession::controller() noexcept
{
    return *m_controller;
}

editor_core::FilesWorkspaceService &WindowsEditorSession::files_workspace() noexcept
{
    return *m_filesWorkspace;
}

const schema::SchemaRegistry &WindowsEditorSession::schema_registry() const noexcept
{
    return *m_schemaRegistry;
}

scene::SceneIdentitySource &WindowsEditorSession::identity_source() noexcept
{
    return *m_sceneIdentitySource;
}

const std::optional<editor_core::EditorDocumentId> &WindowsEditorSession::active_document_id() const noexcept
{
    return m_activeDocumentId;
}

bool WindowsEditorSession::has_prepared_scene() const noexcept
{
    return m_preparedDocumentId.has_value();
}

std::string_view WindowsEditorSession::project_locator() const noexcept
{
    return m_projectLocator;
}

void WindowsEditorSession::reconcile_active_document() noexcept
{
    if (m_activeDocumentId.has_value() && m_controller->session().find_document(*m_activeDocumentId) == nullptr)
    {
        m_activeDocumentId.reset();
        if (m_preparedDocumentId.has_value())
        {
            m_activeDocumentId = *m_preparedDocumentId;
            m_preparedDocumentId.reset();
        }
    }
}
} // namespace cue::editor
