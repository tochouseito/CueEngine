#include <Cue/EditorCore/EditorController.h>

#include <Cue/EditorCore/Error.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Scene/Serialization.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cue::editor_core
{
namespace
{
constexpr std::uint32_t k_recoveryFormatVersion = 1U;
constexpr std::uint32_t k_recoveryRegistryVersion = 1U;
constexpr std::size_t k_maximumRecoveryHeaderBytes = 4096U;
constexpr std::size_t k_maximumRecoveryRegistryBytes = 4U * 1024U * 1024U;
constexpr std::size_t k_maximumRecoveryRegistryEntries = 65536U;
constexpr std::string_view k_recoveryMagic = "CueRecovery";
constexpr std::string_view k_recoveryRegistryMagic = "CueRecoveryRegistry";

struct ParsedRecovery final
{
    RecoveryMetadata metadata;
    std::string sceneJson;
};

/// @brief Saved Root内のProject単位Recovery Registry Locatorを返す
[[nodiscard]] Result<RelativePath> make_recovery_registry_path(const AssertContext &a_assertContext) noexcept
{
    return RelativePath::parse("Editor/Recovery/Registry.cueindex", a_assertContext);
}

/// @brief UTF-8文字列の決定的Digestを返す
[[nodiscard]] std::uint64_t digest_text(std::string_view a_text) noexcept
{
    return file_content_digest(std::as_bytes(std::span(a_text.data(), a_text.size())));
}

/// @brief Scene Stable IDからSaved Root内のRecovery Locatorを構築する
[[nodiscard]] Result<RelativePath> make_recovery_path(const scene::SceneAssetId &a_sceneId,
                                                      const AssertContext &a_assertContext) noexcept
{
    const scene::IdentityText sceneText = a_sceneId.canonical_text();
    try
    {
        std::string path("Editor/Recovery/");
        path.append(sceneText.data(), sceneText.size());
        path.append(".cuerecovery");
        return RelativePath::parse(path, a_assertContext);
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore recovery path allocation failed");
    }
    std::terminate();
}

/// @brief Fileの存在状態、Size、Content DigestをRoot境界内で取得する
[[nodiscard]] Result<SceneFileFingerprint> fingerprint_scene_file(FilesystemRoot &a_filesystem,
                                                                  const RelativePath &a_path,
                                                                  const AssertContext &a_assertContext) noexcept
{
    return fingerprint_file(a_filesystem, a_path, scene::k_maximumSceneBytes, a_assertContext);
}

/// @brief Recovery Headerから一行を取り出してCursorを進める
[[nodiscard]] bool take_line(std::string_view a_text, std::size_t &a_cursor, std::string_view &a_line) noexcept
{
    if (a_cursor > a_text.size())
    {
        return false;
    }
    const std::size_t end = a_text.find('\n', a_cursor);
    if (end == std::string_view::npos || end - a_cursor > k_maximumRecoveryHeaderBytes)
    {
        return false;
    }
    a_line = a_text.substr(a_cursor, end - a_cursor);
    a_cursor = end + 1U;
    return true;
}

/// @brief 10進数の全Input消費を要求して符号なし値を解析する
template <typename Value> [[nodiscard]] bool parse_unsigned(std::string_view a_text, Value &a_value) noexcept
{
    if (a_text.empty())
    {
        return false;
    }
    const auto parsed = std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_value);
    return parsed.ec == std::errc{} && parsed.ptr == a_text.data() + a_text.size();
}

/// @brief Append-only Recovery Registryを完全検証してScene Identity列へ変換する
[[nodiscard]] Result<std::vector<scene::SceneAssetId>> parse_recovery_registry(
    std::string_view a_text, const ProjectDescriptor &a_project, const AssertContext &a_assertContext) noexcept
{
    std::size_t cursor = 0U;
    std::string_view magic;
    std::string_view versionText;
    std::string_view projectId;
    if (!take_line(a_text, cursor, magic) || !take_line(a_text, cursor, versionText) ||
        !take_line(a_text, cursor, projectId) || magic != k_recoveryRegistryMagic)
    {
        return Result<std::vector<scene::SceneAssetId>>::failure(make_editor_core_error(
            a_assertContext, EditorCoreError::InvalidRecovery, "Recovery registry header is malformed"));
    }

    std::uint32_t version = 0U;
    if (!parse_unsigned(versionText, version) || version == 0U)
    {
        return Result<std::vector<scene::SceneAssetId>>::failure(make_editor_core_error(
            a_assertContext, EditorCoreError::InvalidRecovery, "Recovery registry version is invalid"));
    }
    if (version != k_recoveryRegistryVersion)
    {
        return Result<std::vector<scene::SceneAssetId>>::failure(make_editor_core_error(
            a_assertContext, EditorCoreError::UnsupportedRecovery, "Recovery registry version is unsupported"));
    }
    if (projectId != a_project.project_id().text())
    {
        return Result<std::vector<scene::SceneAssetId>>::failure(make_editor_core_error(
            a_assertContext, EditorCoreError::InvalidRecovery, "Recovery registry project identity does not match"));
    }

    try
    {
        std::vector<scene::SceneAssetId> sceneIds;
        while (cursor < a_text.size())
        {
            if (sceneIds.size() == k_maximumRecoveryRegistryEntries)
            {
                return Result<std::vector<scene::SceneAssetId>>::failure(make_editor_core_error(
                    a_assertContext, EditorCoreError::InvalidRecovery, "Recovery registry entry limit was exceeded"));
            }
            std::string_view sceneIdText;
            if (!take_line(a_text, cursor, sceneIdText) || sceneIdText.empty())
            {
                return Result<std::vector<scene::SceneAssetId>>::failure(make_editor_core_error(
                    a_assertContext, EditorCoreError::InvalidRecovery, "Recovery registry entry is malformed"));
            }
            auto sceneId = scene::SceneAssetId::parse(sceneIdText, a_assertContext);
            if (!sceneId)
            {
                return Result<std::vector<scene::SceneAssetId>>::failure(std::move(*sceneId.try_error()));
            }
            if (std::find(sceneIds.begin(), sceneIds.end(), *sceneId.try_value()) != sceneIds.end())
            {
                return Result<std::vector<scene::SceneAssetId>>::failure(make_editor_core_error(
                    a_assertContext, EditorCoreError::InvalidRecovery, "Recovery registry contains a duplicate entry"));
            }
            sceneIds.push_back(std::move(*sceneId.try_value()));
        }
        return Result<std::vector<scene::SceneAssetId>>::success(std::move(sceneIds));
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore recovery registry parsing allocation failed");
    }
    std::terminate();
}

/// @brief Registryが指すRecovery MetadataをProjectと要求Scene Identityへ照合する
[[nodiscard]] Result<void> validate_recovery_candidate(const RecoveryMetadata &a_metadata,
                                                       const ProjectDescriptor &a_project,
                                                       const scene::SceneAssetId &a_sceneId,
                                                       const AssertContext &a_assertContext) noexcept
{
    const scene::IdentityText sceneText = a_sceneId.canonical_text();
    if (a_metadata.project_id() != a_project.project_id().text() ||
        a_metadata.scene_id() != std::string_view(sceneText.data(), sceneText.size()))
    {
        return Result<void>::failure(
            make_editor_core_error(a_assertContext, EditorCoreError::InvalidRecovery,
                                   "Recovery metadata does not match the current project and registry entry"));
    }
    return Result<void>::success();
}

/// @brief Recovery RegistryへScene Identityを先行登録し、Envelope公開後の孤立を防ぐ
[[nodiscard]] Result<void> register_recovery_candidate(FilesystemRoot &a_savedRoot, const ProjectDescriptor &a_project,
                                                       const scene::SceneAssetId &a_sceneId,
                                                       const AssertContext &a_assertContext) noexcept
{
    auto registryPath = make_recovery_registry_path(a_assertContext);
    if (!registryPath)
    {
        return Result<void>::failure(std::move(*registryPath.try_error()));
    }
    auto expected =
        fingerprint_file(a_savedRoot, *registryPath.try_value(), k_maximumRecoveryRegistryBytes, a_assertContext);
    if (!expected)
    {
        return Result<void>::failure(std::move(*expected.try_error()));
    }
    auto lease = a_savedRoot.acquire_file_write_lease(*registryPath.try_value());
    if (!lease)
    {
        return Result<void>::failure(std::move(*lease.try_error()));
    }
    auto leased =
        fingerprint_file(a_savedRoot, *registryPath.try_value(), k_maximumRecoveryRegistryBytes, a_assertContext);
    if (!leased)
    {
        return Result<void>::failure(std::move(*leased.try_error()));
    }
    if (*leased.try_value() != *expected.try_value())
    {
        return Result<void>::failure(
            make_editor_core_error(a_assertContext, EditorCoreError::ExternalConflict,
                                   "Recovery registry changed before the write lease was acquired"));
    }

    std::vector<scene::SceneAssetId> sceneIds;
    if (expected.try_value()->exists)
    {
        auto bytes = a_savedRoot.read_file(*registryPath.try_value(), k_maximumRecoveryRegistryBytes);
        if (!bytes)
        {
            return Result<void>::failure(std::move(*bytes.try_error()));
        }
        const auto &storage = *bytes.try_value();
        auto parsed =
            parse_recovery_registry(std::string_view(reinterpret_cast<const char *>(storage.data()), storage.size()),
                                    a_project, a_assertContext);
        if (!parsed)
        {
            return Result<void>::failure(std::move(*parsed.try_error()));
        }
        sceneIds = std::move(*parsed.try_value());
    }
    const bool containsScene = std::find(sceneIds.begin(), sceneIds.end(), a_sceneId) != sceneIds.end();
    if (!containsScene && sceneIds.size() == k_maximumRecoveryRegistryEntries)
    {
        return Result<void>::failure(make_editor_core_error(a_assertContext, EditorCoreError::InvalidRecovery,
                                                            "Recovery registry entry limit was exceeded"));
    }

    try
    {
        if (!containsScene)
        {
            sceneIds.push_back(a_sceneId);
            std::sort(sceneIds.begin(), sceneIds.end());
        }
        std::string registry;
        registry.reserve(k_recoveryRegistryMagic.size() + a_project.project_id().text().size() + 16U +
                         sceneIds.size() * 37U);
        registry.append(k_recoveryRegistryMagic);
        registry.push_back('\n');
        registry.append(std::to_string(k_recoveryRegistryVersion));
        registry.push_back('\n');
        registry.append(a_project.project_id().text());
        registry.push_back('\n');
        for (const scene::SceneAssetId &sceneId : sceneIds)
        {
            const scene::IdentityText sceneText = sceneId.canonical_text();
            registry.append(sceneText.data(), sceneText.size());
            registry.push_back('\n');
        }
        if (registry.size() > k_maximumRecoveryRegistryBytes)
        {
            return Result<void>::failure(make_editor_core_error(a_assertContext, EditorCoreError::InvalidRecovery,
                                                                "Recovery registry byte limit was exceeded"));
        }
        return a_savedRoot.write_file_atomic_if_unchanged(*lease.try_value(), *registryPath.try_value(),
                                                          *expected.try_value(), k_maximumRecoveryRegistryBytes,
                                                          std::as_bytes(std::span(registry.data(), registry.size())));
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore recovery registry allocation failed");
    }
    std::terminate();
}

/// @brief Version付きRecovery Envelopeを完全検証して所有値へ変換する
[[nodiscard]] Result<ParsedRecovery> parse_recovery_envelope(std::string_view a_text,
                                                             const AssertContext &a_assertContext) noexcept
{
    std::size_t cursor = 0U;
    std::string_view magic;
    std::string_view versionText;
    std::string_view projectId;
    std::string_view sceneId;
    std::string_view locatorText;
    std::string_view baseExistsText;
    std::string_view baseSizeText;
    std::string_view baseDigestText;
    std::string_view stateText;
    std::string_view sceneDigestText;
    std::string_view sceneSizeText;
    std::string_view separator;
    if (!take_line(a_text, cursor, magic) || !take_line(a_text, cursor, versionText) ||
        !take_line(a_text, cursor, projectId) || !take_line(a_text, cursor, sceneId) ||
        !take_line(a_text, cursor, locatorText) || !take_line(a_text, cursor, baseExistsText) ||
        !take_line(a_text, cursor, baseSizeText) || !take_line(a_text, cursor, baseDigestText) ||
        !take_line(a_text, cursor, stateText) || !take_line(a_text, cursor, sceneDigestText) ||
        !take_line(a_text, cursor, sceneSizeText) || !take_line(a_text, cursor, separator) ||
        magic != k_recoveryMagic || !separator.empty())
    {
        return Result<ParsedRecovery>::failure(make_editor_core_error(a_assertContext, EditorCoreError::InvalidRecovery,
                                                                      "Recovery envelope header is malformed"));
    }

    std::uint32_t version = 0U;
    if (!parse_unsigned(versionText, version) || version == 0U)
    {
        return Result<ParsedRecovery>::failure(make_editor_core_error(a_assertContext, EditorCoreError::InvalidRecovery,
                                                                      "Recovery format version is invalid"));
    }
    if (version != k_recoveryFormatVersion)
    {
        return Result<ParsedRecovery>::failure(make_editor_core_error(
            a_assertContext, EditorCoreError::UnsupportedRecovery, "Recovery format version is unsupported"));
    }

    auto parsedProjectId = ProjectId::parse(projectId, a_assertContext);
    auto parsedSceneId = scene::SceneAssetId::parse(sceneId, a_assertContext);
    auto locator = RelativePath::parse(locatorText, a_assertContext);
    std::uint64_t baseSize = 0U;
    std::uint64_t baseDigest = 0U;
    std::uint64_t stateValue = 0U;
    std::uint64_t sceneDigest = 0U;
    std::uint64_t sceneSize = 0U;
    const bool baseExists = baseExistsText == "1";
    if (!parsedProjectId || !parsedSceneId || !locator || (baseExistsText != "0" && !baseExists) ||
        !parse_unsigned(baseSizeText, baseSize) || !parse_unsigned(baseDigestText, baseDigest) ||
        !parse_unsigned(stateText, stateValue) || stateValue == 0U || !parse_unsigned(sceneDigestText, sceneDigest) ||
        !parse_unsigned(sceneSizeText, sceneSize) || sceneSize > scene::k_maximumSceneBytes ||
        sceneSize != a_text.size() - cursor || (!baseExists && (baseSize != 0U || baseDigest != 0U)))
    {
        return Result<ParsedRecovery>::failure(make_editor_core_error(a_assertContext, EditorCoreError::InvalidRecovery,
                                                                      "Recovery envelope metadata is invalid"));
    }

    const std::string_view sceneJson = a_text.substr(cursor, static_cast<std::size_t>(sceneSize));
    if (digest_text(sceneJson) != sceneDigest)
    {
        return Result<ParsedRecovery>::failure(make_editor_core_error(
            a_assertContext, EditorCoreError::InvalidRecovery, "Recovery scene digest does not match its payload"));
    }

    try
    {
        RecoveryMetadata metadata(version, std::string(projectId), std::string(sceneId),
                                  std::move(*locator.try_value()),
                                  SceneFileFingerprint{baseExists, baseSize, baseDigest}, stateValue, sceneDigest);
        return Result<ParsedRecovery>::success(ParsedRecovery{std::move(metadata), std::string(sceneJson)});
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore recovery parsing allocation failed");
    }
    std::terminate();
}

/// @brief Recovery Metadataが現在SessionとDocumentに対応することを検証する
[[nodiscard]] Result<void> validate_recovery_identity(const RecoveryMetadata &a_metadata,
                                                      const ProjectDescriptor &a_project,
                                                      const EditorDocument &a_document,
                                                      const AssertContext &a_assertContext) noexcept
{
    const scene::IdentityText sceneText = a_document.scene_document().scene_asset_id().canonical_text();
    const std::string_view currentSceneId(sceneText.data(), sceneText.size());
    if (a_metadata.project_id() != a_project.project_id().text() || a_metadata.scene_id() != currentSceneId ||
        a_metadata.source_locator().comparison_key(a_assertContext) !=
            a_document.scene_locator().comparison_key(a_assertContext))
    {
        return Result<void>::failure(make_editor_document_error(
            a_assertContext, EditorCoreError::InvalidRecovery,
            "Recovery metadata does not match the current project and scene", a_document.id().value()));
    }
    return Result<void>::success();
}
} // namespace

ScenePersistenceServices::ScenePersistenceServices(
    FilesystemRoot &a_sourceAssetsRoot, FilesystemRoot &a_savedRoot, const schema::SchemaRegistry &a_schemaRegistry,
    const scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry,
    const scene::SceneMigrationRegistry &a_sceneMigrations,
    const scene::ComponentMigrationRegistry &a_componentMigrations) noexcept
    : m_sourceAssetsRoot(&a_sourceAssetsRoot), m_savedRoot(&a_savedRoot), m_schemaRegistry(&a_schemaRegistry),
      m_valueSchemaRegistry(&a_valueSchemaRegistry), m_sceneMigrations(&a_sceneMigrations),
      m_componentMigrations(&a_componentMigrations)
{
}

RecoveryMetadata::RecoveryMetadata(std::uint32_t a_formatVersion, std::string a_projectId, std::string a_sceneId,
                                   RelativePath a_sourceLocator, SceneFileFingerprint a_baseFingerprint,
                                   std::uint64_t a_sourceStateValue, std::uint64_t a_sceneDigest) noexcept
    : m_formatVersion(a_formatVersion), m_projectId(std::move(a_projectId)), m_sceneId(std::move(a_sceneId)),
      m_sourceLocator(std::move(a_sourceLocator)), m_baseFingerprint(a_baseFingerprint),
      m_sourceStateValue(a_sourceStateValue), m_sceneDigest(a_sceneDigest)
{
}

std::uint32_t RecoveryMetadata::format_version() const noexcept
{
    return m_formatVersion;
}

const std::string &RecoveryMetadata::project_id() const noexcept
{
    return m_projectId;
}

const std::string &RecoveryMetadata::scene_id() const noexcept
{
    return m_sceneId;
}

const RelativePath &RecoveryMetadata::source_locator() const noexcept
{
    return m_sourceLocator;
}

SceneFileFingerprint RecoveryMetadata::base_fingerprint() const noexcept
{
    return m_baseFingerprint;
}

std::uint64_t RecoveryMetadata::source_state_value() const noexcept
{
    return m_sourceStateValue;
}

std::uint64_t RecoveryMetadata::scene_digest() const noexcept
{
    return m_sceneDigest;
}

RecoveryCandidateInspection::RecoveryCandidateInspection(std::string a_sceneId, RecoveryMetadata a_metadata) noexcept
    : m_sceneId(std::move(a_sceneId)), m_metadata(std::move(a_metadata))
{
}

RecoveryCandidateInspection::RecoveryCandidateInspection(std::string a_sceneId, Error a_error) noexcept
    : m_sceneId(std::move(a_sceneId)), m_error(std::move(a_error))
{
}

const std::string &RecoveryCandidateInspection::scene_id() const noexcept
{
    return m_sceneId;
}

const RecoveryMetadata *RecoveryCandidateInspection::try_metadata() const noexcept
{
    return m_metadata.has_value() ? &*m_metadata : nullptr;
}

const Error *RecoveryCandidateInspection::try_error() const noexcept
{
    return m_error.has_value() ? &*m_error : nullptr;
}

Result<void> EditorController::require_persistence_services() const noexcept
{
    if (m_sourceAssetsRoot == nullptr || m_savedRoot == nullptr || m_schemaRegistry == nullptr ||
        m_valueSchemaRegistry == nullptr || m_sceneMigrations == nullptr || m_componentMigrations == nullptr)
    {
        return Result<void>::failure(
            make_editor_core_error(*m_assertContext, EditorCoreError::PersistenceUnavailable,
                                   "Editor controller was created without scene persistence services"));
    }
    return Result<void>::success();
}

Result<EditorDocumentId> EditorController::open_document_from_storage(RelativePath a_locator) noexcept
{
    assert_owner_thread();
    auto services = require_persistence_services();
    if (!services)
    {
        return Result<EditorDocumentId>::failure(std::move(*services.try_error()));
    }

    auto beforeFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, a_locator, *m_assertContext);
    if (!beforeFingerprint)
    {
        return Result<EditorDocumentId>::failure(std::move(*beforeFingerprint.try_error()));
    }
    auto loaded = scene::load_scene_document(*m_sourceAssetsRoot, a_locator, *m_schemaRegistry, *m_valueSchemaRegistry,
                                             *m_sceneMigrations, *m_componentMigrations, *m_assertContext);
    if (!loaded)
    {
        return Result<EditorDocumentId>::failure(std::move(*loaded.try_error()));
    }
    auto afterFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, a_locator, *m_assertContext);
    if (!afterFingerprint)
    {
        return Result<EditorDocumentId>::failure(std::move(*afterFingerprint.try_error()));
    }
    if (*beforeFingerprint.try_value() != *afterFingerprint.try_value())
    {
        return Result<EditorDocumentId>::failure(make_editor_core_error(
            *m_assertContext, EditorCoreError::ExternalConflict, "Scene changed while it was being opened"));
    }

    const scene::SceneAssetId sceneId = loaded.try_value()->document().scene_asset_id();
    auto recoveryPath = make_recovery_path(sceneId, *m_assertContext);
    if (!recoveryPath)
    {
        return Result<EditorDocumentId>::failure(std::move(*recoveryPath.try_error()));
    }
    auto recoveryEntry = m_savedRoot->query_entry(*recoveryPath.try_value());
    if (!recoveryEntry)
    {
        return Result<EditorDocumentId>::failure(std::move(*recoveryEntry.try_error()));
    }
    if (*recoveryEntry.try_value() != EntryType::Missing && *recoveryEntry.try_value() != EntryType::RegularFile)
    {
        return Result<EditorDocumentId>::failure(make_editor_core_error(
            *m_assertContext, EditorCoreError::InvalidRecovery, "Recovery destination is not a regular file"));
    }

    auto opened = open_document(std::move(loaded.try_value()->document()), std::move(a_locator), true);
    if (!opened)
    {
        return opened;
    }
    EditorDocument *document = find_document(*opened.try_value());
    document->m_baseFingerprint = *afterFingerprint.try_value();
    document->m_hasRecoveryCandidate = *recoveryEntry.try_value() == EntryType::RegularFile;
    return opened;
}

Result<scene::SceneSnapshot> EditorController::load_saved_startup_scene_snapshot() noexcept
{
    assert_owner_thread();
    auto services = require_persistence_services();
    if (!services)
    {
        return Result<scene::SceneSnapshot>::failure(std::move(*services.try_error()));
    }

    const std::optional<StartupSceneReference> &startupScene = m_session.project_descriptor().default_scene();
    if (!startupScene.has_value())
    {
        return Result<scene::SceneSnapshot>::failure(make_editor_core_error(
            *m_assertContext, EditorCoreError::SceneMismatch, "Project Descriptor has no Startup Scene"));
    }

    const RelativePath &locator = startupScene->source_locator();
    auto beforeFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, locator, *m_assertContext);
    if (!beforeFingerprint)
    {
        return Result<scene::SceneSnapshot>::failure(std::move(*beforeFingerprint.try_error()));
    }
    auto loaded = scene::load_scene_document(*m_sourceAssetsRoot, locator, *m_schemaRegistry, *m_valueSchemaRegistry,
                                             *m_sceneMigrations, *m_componentMigrations, *m_assertContext);
    if (!loaded)
    {
        return Result<scene::SceneSnapshot>::failure(std::move(*loaded.try_error()));
    }
    auto afterFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, locator, *m_assertContext);
    if (!afterFingerprint)
    {
        return Result<scene::SceneSnapshot>::failure(std::move(*afterFingerprint.try_error()));
    }
    if (*beforeFingerprint.try_value() != *afterFingerprint.try_value())
    {
        return Result<scene::SceneSnapshot>::failure(make_editor_core_error(
            *m_assertContext, EditorCoreError::ExternalConflict,
            "Startup Scene changed while the Package input was being loaded"));
    }

    auto expectedSceneId = scene::SceneAssetId::parse(startupScene->scene_asset_id(), *m_assertContext);
    if (!expectedSceneId)
    {
        return Result<scene::SceneSnapshot>::failure(std::move(*expectedSceneId.try_error()));
    }
    if (loaded.try_value()->document().scene_asset_id() != *expectedSceneId.try_value())
    {
        return Result<scene::SceneSnapshot>::failure(make_editor_core_error(
            *m_assertContext, EditorCoreError::SceneMismatch,
            "Startup Scene identity differs from the Project Descriptor"));
    }
    return scene::create_scene_snapshot(loaded.try_value()->document(), *m_assertContext);
}

Result<ExternalChangeState> EditorController::poll_external_change(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<ExternalChangeState>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }
    if (!document->m_hasSavedDestination || !document->m_baseFingerprint.has_value())
    {
        document->m_externalChangeState = ExternalChangeState::None;
        return Result<ExternalChangeState>::success(ExternalChangeState::None);
    }
    auto services = require_persistence_services();
    if (!services)
    {
        return Result<ExternalChangeState>::failure(std::move(*services.try_error()));
    }

    auto current = fingerprint_scene_file(*m_sourceAssetsRoot, document->m_locator, *m_assertContext);
    if (!current)
    {
        return Result<ExternalChangeState>::failure(std::move(*current.try_error()));
    }
    if (*current.try_value() == *document->m_baseFingerprint)
    {
        document->m_externalChangeState = ExternalChangeState::None;
    }
    else
    {
        document->m_externalChangeState =
            current.try_value()->exists ? ExternalChangeState::Modified : ExternalChangeState::Removed;
    }
    return Result<ExternalChangeState>::success(ExternalChangeState(document->m_externalChangeState));
}

Result<scene::SceneSaveOutcome> EditorController::save_document(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<scene::SceneSaveOutcome>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }
    return save_document_to(a_documentId, document->m_locator, false, false);
}

Result<scene::SceneSaveOutcome> EditorController::save_document_as(EditorDocumentId a_documentId,
                                                                   RelativePath a_locator) noexcept
{
    return save_document_to(a_documentId, std::move(a_locator), true, false);
}

Result<scene::SceneSaveOutcome> EditorController::save_document_as_new(EditorDocumentId a_documentId,
                                                                       RelativePath a_locator) noexcept
{
    return save_document_to(a_documentId, std::move(a_locator), true, true);
}

Result<scene::SceneSaveOutcome> EditorController::save_document_to(EditorDocumentId a_documentId,
                                                                   RelativePath a_locator,
                                                                   bool a_switchDestination,
                                                                   bool a_requireMissingDestination) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<scene::SceneSaveOutcome>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }
    if (document->m_closeState == DocumentCloseState::Closed)
    {
        return Result<scene::SceneSaveOutcome>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::InvalidDocumentState,
                                       "Closed document cannot be saved", a_documentId.value()));
    }
    const auto failSave = [document](Error &&a_error) noexcept
    {
        if (document->m_closeState == DocumentCloseState::SaveRequested)
        {
            document->m_closeState = DocumentCloseState::AwaitingDecision;
        }
        return Result<scene::SceneSaveOutcome>::failure(std::move(a_error));
    };
    auto services = require_persistence_services();
    if (!services)
    {
        return failSave(std::move(*services.try_error()));
    }
    if (document->m_pendingSave.has_value())
    {
        return failSave(make_editor_document_error(*m_assertContext, EditorCoreError::InvalidDocumentState,
                                                   "Save Uncertain must be retried or discarded before another save",
                                                   a_documentId.value()));
    }

    if (a_switchDestination)
    {
        const std::string destinationKey = a_locator.comparison_key(*m_assertContext);
        for (const EditorDocument &other : m_session.m_documents)
        {
            if (other.id() != a_documentId && other.scene_locator().comparison_key(*m_assertContext) == destinationKey)
            {
                return failSave(make_editor_document_error(*m_assertContext, EditorCoreError::DuplicateLocator,
                                                           "Save As destination is already open in another document",
                                                           a_documentId.value()));
            }
        }
    }
    else if (!document->m_hasSavedDestination || !document->m_baseFingerprint.has_value())
    {
        return failSave(make_editor_document_error(
            *m_assertContext, EditorCoreError::PersistenceUnavailable,
            "Normal save requires a destination with a captured base fingerprint", a_documentId.value()));
    }

    auto destinationFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, a_locator, *m_assertContext);
    if (!destinationFingerprint)
    {
        return failSave(std::move(*destinationFingerprint.try_error()));
    }
    if (a_requireMissingDestination && destinationFingerprint.try_value()->exists)
    {
        return failSave(make_editor_document_error(
            *m_assertContext, EditorCoreError::ExternalConflict,
            "New scene destination already exists", a_documentId.value()));
    }
    if (!a_switchDestination && (document->m_externalChangeState != ExternalChangeState::None ||
                                 *destinationFingerprint.try_value() != *document->m_baseFingerprint))
    {
        document->m_externalChangeState =
            destinationFingerprint.try_value()->exists ? ExternalChangeState::Modified : ExternalChangeState::Removed;
        if (document->m_closeState == DocumentCloseState::SaveRequested)
        {
            document->m_closeState = DocumentCloseState::AwaitingDecision;
        }
        return Result<scene::SceneSaveOutcome>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::ExternalConflict,
            "Scene destination changed since the document was loaded", a_documentId.value()));
    }

    const SceneFileFingerprint expectedFingerprint =
        a_switchDestination ? *destinationFingerprint.try_value() : *document->m_baseFingerprint;
    auto lease = m_sourceAssetsRoot->acquire_file_write_lease(a_locator);
    if (!lease)
    {
        return failSave(std::move(*lease.try_error()));
    }
    auto leasedFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, a_locator, *m_assertContext);
    if (!leasedFingerprint)
    {
        return failSave(std::move(*leasedFingerprint.try_error()));
    }
    if (*leasedFingerprint.try_value() != expectedFingerprint)
    {
        if (!a_switchDestination)
        {
            document->m_externalChangeState =
                leasedFingerprint.try_value()->exists ? ExternalChangeState::Modified : ExternalChangeState::Removed;
        }
        if (document->m_closeState == DocumentCloseState::SaveRequested)
        {
            document->m_closeState = DocumentCloseState::AwaitingDecision;
        }
        return Result<scene::SceneSaveOutcome>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::ExternalConflict,
            "Scene destination changed before the write lease was acquired", a_documentId.value()));
    }

    auto candidateText = scene::serialize_scene_document(document->m_document, *m_assertContext);
    if (!candidateText)
    {
        return failSave(std::move(*candidateText.try_error()));
    }
    const DocumentStateId savedState = document->m_currentStateId;
    scene::SceneDocumentCheckpoint candidateCheckpoint = document->m_document.create_checkpoint();
    const std::uint64_t candidateByteSize = static_cast<std::uint64_t>(candidateText.try_value()->size());
    const std::uint64_t candidateDigest = digest_text(*candidateText.try_value());
    scene::SceneSaveOutcome outcome = scene::save_scene_document_if_unchanged(
        *m_sourceAssetsRoot, *lease.try_value(), a_locator, expectedFingerprint, document->m_document,
        *m_schemaRegistry, *m_valueSchemaRegistry, *m_sceneMigrations, *m_componentMigrations, *m_assertContext);
    if (outcome.status() == scene::SceneSaveStatus::NotPublished && outcome.try_error() != nullptr &&
        outcome.try_error()->root_code().domain() == "Cue.IO" &&
        outcome.try_error()->root_code().value() == static_cast<std::int64_t>(IoError::PreconditionFailed))
    {
        if (!a_switchDestination)
        {
            document->m_externalChangeState = ExternalChangeState::Modified;
        }
        if (document->m_closeState == DocumentCloseState::SaveRequested)
        {
            document->m_closeState = DocumentCloseState::AwaitingDecision;
        }
        return Result<scene::SceneSaveOutcome>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::ExternalConflict,
            "Scene destination changed immediately before atomic publish", a_documentId.value()));
    }
    if (outcome.status() == scene::SceneSaveStatus::Committed)
    {
        auto committedFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, a_locator, *m_assertContext);
        if (!committedFingerprint)
        {
            document->m_persistenceState = DocumentPersistenceState::SaveUncertain;
            document->m_pendingSave.emplace(EditorDocument::PendingSaveRecord{
                savedState, std::move(a_locator), expectedFingerprint, std::move(candidateCheckpoint),
                candidateByteSize, candidateDigest, {}, EditorDocument::PendingSaveReason::VerificationFailed,
                a_switchDestination});
            if (document->m_closeState == DocumentCloseState::SaveRequested)
            {
                document->m_closeState = DocumentCloseState::AwaitingDecision;
            }
            return Result<scene::SceneSaveOutcome>::failure(std::move(*committedFingerprint.try_error()));
        }
        if (!committedFingerprint.try_value()->exists ||
            committedFingerprint.try_value()->byteSize != candidateByteSize ||
            committedFingerprint.try_value()->contentDigest != candidateDigest)
        {
            if (!a_switchDestination)
            {
                document->m_externalChangeState = committedFingerprint.try_value()->exists
                                                      ? ExternalChangeState::Modified
                                                      : ExternalChangeState::Removed;
            }
            if (document->m_closeState == DocumentCloseState::SaveRequested)
            {
                document->m_closeState = DocumentCloseState::AwaitingDecision;
            }
            return Result<scene::SceneSaveOutcome>::failure(make_editor_document_error(
                *m_assertContext, EditorCoreError::ExternalConflict,
                "Committed scene destination differs from the saved candidate", a_documentId.value()));
        }
        if (a_switchDestination)
        {
            document->m_locator = std::move(a_locator);
        }
        document->m_baseFingerprint = *committedFingerprint.try_value();
        document->m_externalChangeState = ExternalChangeState::None;
        document->m_persistenceState = DocumentPersistenceState::Idle;
        document->m_pendingSave.reset();
        auto marked = mark_saved(a_documentId, savedState);
        if (!marked)
        {
            return failSave(std::move(*marked.try_error()));
        }
    }
    else
    {
        if (outcome.status() == scene::SceneSaveStatus::PublishedButDurabilityUnknown ||
            outcome.status() == scene::SceneSaveStatus::PublishedButBackupDurabilityUnknown ||
            outcome.status() == scene::SceneSaveStatus::PublishedButVerificationFailed)
        {
            document->m_persistenceState = DocumentPersistenceState::SaveUncertain;
            EditorDocument::PendingSaveReason reason = EditorDocument::PendingSaveReason::VerificationFailed;
            if (outcome.status() == scene::SceneSaveStatus::PublishedButDurabilityUnknown)
            {
                reason = EditorDocument::PendingSaveReason::DurabilityUnknown;
            }
            else if (outcome.status() == scene::SceneSaveStatus::PublishedButBackupDurabilityUnknown)
            {
                reason = EditorDocument::PendingSaveReason::BackupDurabilityUnknown;
            }
            auto recoveryBackupBytes = outcome.take_recovery_backup_bytes();
            document->m_pendingSave.emplace(EditorDocument::PendingSaveRecord{
                savedState, std::move(a_locator), expectedFingerprint, std::move(candidateCheckpoint),
                candidateByteSize, candidateDigest, std::move(recoveryBackupBytes), reason, a_switchDestination});
        }
        if (document->m_closeState == DocumentCloseState::SaveRequested)
        {
            document->m_closeState = DocumentCloseState::AwaitingDecision;
        }
    }
    return Result<scene::SceneSaveOutcome>::success(std::move(outcome));
}

Result<std::vector<scene::SceneSaveStatus>> EditorController::save_all_documents() noexcept
{
    assert_owner_thread();
    try
    {
        std::vector<EditorDocumentId> targets;
        for (const EditorDocument &document : m_session.m_documents)
        {
            if (document.is_dirty())
            {
                targets.push_back(document.id());
            }
        }
        std::vector<scene::SceneSaveStatus> statuses;
        statuses.reserve(targets.size());
        for (const EditorDocumentId id : targets)
        {
            auto saved = save_document(id);
            if (!saved)
            {
                return Result<std::vector<scene::SceneSaveStatus>>::failure(std::move(*saved.try_error()));
            }
            statuses.push_back(saved.try_value()->status());
        }
        return Result<std::vector<scene::SceneSaveStatus>>::success(std::move(statuses));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation();
    }
    catch (...)
    {
        terminate_exception();
    }
}

Result<scene::SceneSaveStatus> EditorController::retry_uncertain_save(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<scene::SceneSaveStatus>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }

    struct RetryCloseGuard final
    {
        /// @brief 未確定または失敗したRetry後にClose判断待ちへ戻す
        ~RetryCloseGuard()
        {
            if (!dismissed && *state == DocumentCloseState::SaveRequested)
            {
                *state = DocumentCloseState::AwaitingDecision;
            }
        }

        DocumentCloseState *state;
        bool dismissed = false;
    } closeGuard{&document->m_closeState};

    auto services = require_persistence_services();
    if (!services)
    {
        return Result<scene::SceneSaveStatus>::failure(std::move(*services.try_error()));
    }
    if (!document->m_pendingSave.has_value() || document->m_persistenceState != DocumentPersistenceState::SaveUncertain)
    {
        return Result<scene::SceneSaveStatus>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::InvalidDocumentState,
                                       "Retry requires a Save Uncertain record", a_documentId.value()));
    }

    EditorDocument::PendingSaveRecord &record = *document->m_pendingSave;
    auto lease = m_sourceAssetsRoot->acquire_file_write_lease(record.destination);
    if (!lease)
    {
        return Result<scene::SceneSaveStatus>::failure(std::move(*lease.try_error()));
    }
    auto currentFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, record.destination, *m_assertContext);
    if (!currentFingerprint)
    {
        return Result<scene::SceneSaveStatus>::failure(std::move(*currentFingerprint.try_error()));
    }
    auto currentScene =
        scene::load_scene_document(*m_sourceAssetsRoot, record.destination, *m_schemaRegistry, *m_valueSchemaRegistry,
                                   *m_sceneMigrations, *m_componentMigrations, *m_assertContext);
    if (!currentScene)
    {
        if (!record.switchDestination)
        {
            document->m_externalChangeState =
                currentFingerprint.try_value()->exists ? ExternalChangeState::Modified : ExternalChangeState::Removed;
        }
        return Result<scene::SceneSaveStatus>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::ExternalConflict,
                                       "Save Uncertain destination cannot be validated", a_documentId.value()));
    }
    auto currentText = scene::serialize_scene_document(currentScene.try_value()->document(), *m_assertContext);
    if (!currentText)
    {
        return Result<scene::SceneSaveStatus>::failure(std::move(*currentText.try_error()));
    }
    if (currentScene.try_value()->document().scene_asset_id() != document->m_document.scene_asset_id() ||
        static_cast<std::uint64_t>(currentText.try_value()->size()) != record.candidateByteSize ||
        digest_text(*currentText.try_value()) != record.candidateDigest)
    {
        if (!record.switchDestination)
        {
            document->m_externalChangeState = ExternalChangeState::Modified;
        }
        return Result<scene::SceneSaveStatus>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::ExternalConflict,
            "Save Uncertain destination differs from the recorded candidate", a_documentId.value()));
    }
    if (record.reason == EditorDocument::PendingSaveReason::VerificationFailed)
    {
        auto verifiedFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, record.destination, *m_assertContext);
        if (!verifiedFingerprint)
        {
            return Result<scene::SceneSaveStatus>::failure(std::move(*verifiedFingerprint.try_error()));
        }
        if (*verifiedFingerprint.try_value() != *currentFingerprint.try_value())
        {
            if (!record.switchDestination)
            {
                document->m_externalChangeState = ExternalChangeState::Modified;
            }
            return Result<scene::SceneSaveStatus>::failure(make_editor_document_error(
                *m_assertContext, EditorCoreError::ExternalConflict,
                "Save Uncertain destination changed during verification", a_documentId.value()));
        }
        currentFingerprint = std::move(verifiedFingerprint);
    }

    scene::SceneSaveStatus status = scene::SceneSaveStatus::Committed;
    if (record.reason == EditorDocument::PendingSaveReason::BackupDurabilityUnknown)
    {
        if (!record.recoveryBackupBytes.has_value())
        {
            return Result<scene::SceneSaveStatus>::failure(make_editor_document_error(
                *m_assertContext, EditorCoreError::InvalidDocumentState,
                "Backup retry requires the original destination bytes", a_documentId.value()));
        }
        auto backupWritten = m_sourceAssetsRoot->write_recovery_backup_atomic(
            record.destination, *record.recoveryBackupBytes, *m_assertContext);
        if (!backupWritten)
        {
            const bool durabilityUnknown =
                backupWritten.try_error()->root_code().domain() == "Cue.IO" &&
                backupWritten.try_error()->root_code().value() == static_cast<std::int64_t>(IoError::DurabilityUnknown);
            status = durabilityUnknown ? scene::SceneSaveStatus::PublishedButBackupDurabilityUnknown
                                       : scene::SceneSaveStatus::PublishedButVerificationFailed;
            return Result<scene::SceneSaveStatus>::success(std::move(status));
        }
        currentFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, record.destination, *m_assertContext);
        if (!currentFingerprint)
        {
            return Result<scene::SceneSaveStatus>::failure(std::move(*currentFingerprint.try_error()));
        }
        if (!currentFingerprint.try_value()->exists ||
            currentFingerprint.try_value()->byteSize != record.candidateByteSize ||
            currentFingerprint.try_value()->contentDigest != record.candidateDigest)
        {
            if (!record.switchDestination)
            {
                document->m_externalChangeState = currentFingerprint.try_value()->exists
                                                      ? ExternalChangeState::Modified
                                                      : ExternalChangeState::Removed;
            }
            return Result<scene::SceneSaveStatus>::failure(make_editor_document_error(
                *m_assertContext, EditorCoreError::ExternalConflict,
                "Backup retry destination differs from the recorded candidate", a_documentId.value()));
        }
    }
    else if (record.reason == EditorDocument::PendingSaveReason::DurabilityUnknown)
    {
        scene::SceneDocument candidate =
            scene::SceneDocument::create(document->m_document.scene_asset_id(), *m_assertContext);
        auto restored = candidate.restore_checkpoint(record.candidateCheckpoint);
        if (!restored)
        {
            return Result<scene::SceneSaveStatus>::failure(std::move(*restored.try_error()));
        }
        scene::SceneSaveOutcome retry = record.recoveryBackupBytes.has_value()
                                            ? scene::save_scene_document_if_unchanged_with_backup(
                                                  *m_sourceAssetsRoot, *lease.try_value(), record.destination,
                                                  *currentFingerprint.try_value(), *record.recoveryBackupBytes,
                                                  candidate, *m_schemaRegistry, *m_valueSchemaRegistry,
                                                  *m_sceneMigrations, *m_componentMigrations, *m_assertContext)
                                            : scene::save_scene_document_if_unchanged(
                                                  *m_sourceAssetsRoot, *lease.try_value(), record.destination,
                                                  *currentFingerprint.try_value(), candidate, *m_schemaRegistry,
                                                  *m_valueSchemaRegistry, *m_sceneMigrations, *m_componentMigrations,
                                                  *m_assertContext);
        status = retry.status();
        if (status == scene::SceneSaveStatus::NotPublished && retry.try_error() != nullptr &&
            retry.try_error()->root_code().domain() == "Cue.IO" &&
            retry.try_error()->root_code().value() == static_cast<std::int64_t>(IoError::PreconditionFailed))
        {
            if (!record.switchDestination)
            {
                document->m_externalChangeState = ExternalChangeState::Modified;
            }
            return Result<scene::SceneSaveStatus>::failure(make_editor_document_error(
                *m_assertContext, EditorCoreError::ExternalConflict,
                "Save Uncertain destination changed immediately before retry publish", a_documentId.value()));
        }
        if (status != scene::SceneSaveStatus::Committed)
        {
            if (status == scene::SceneSaveStatus::PublishedButVerificationFailed)
            {
                record.reason = EditorDocument::PendingSaveReason::VerificationFailed;
            }
            else if (status == scene::SceneSaveStatus::PublishedButBackupDurabilityUnknown)
            {
                auto recoveryBackupBytes = retry.take_recovery_backup_bytes();
                if (!record.recoveryBackupBytes.has_value() && recoveryBackupBytes.has_value())
                {
                    record.recoveryBackupBytes = std::move(recoveryBackupBytes);
                }
                record.reason = EditorDocument::PendingSaveReason::BackupDurabilityUnknown;
            }
            return Result<scene::SceneSaveStatus>::success(std::move(status));
        }
        currentFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, record.destination, *m_assertContext);
        if (!currentFingerprint)
        {
            return Result<scene::SceneSaveStatus>::failure(std::move(*currentFingerprint.try_error()));
        }
        if (!currentFingerprint.try_value()->exists ||
            currentFingerprint.try_value()->byteSize != record.candidateByteSize ||
            currentFingerprint.try_value()->contentDigest != record.candidateDigest)
        {
            if (!record.switchDestination)
            {
                document->m_externalChangeState = currentFingerprint.try_value()->exists ? ExternalChangeState::Modified
                                                                                         : ExternalChangeState::Removed;
            }
            return Result<scene::SceneSaveStatus>::failure(make_editor_document_error(
                *m_assertContext, EditorCoreError::ExternalConflict,
                "Committed retry destination differs from the recorded candidate", a_documentId.value()));
        }
    }

    const DocumentStateId savedState = record.sourceStateId;
    const bool switchDestination = record.switchDestination;
    RelativePath destination = std::move(record.destination);
    document->m_pendingSave.reset();
    document->m_persistenceState = DocumentPersistenceState::Idle;
    document->m_externalChangeState = ExternalChangeState::None;
    document->m_baseFingerprint = *currentFingerprint.try_value();
    if (switchDestination)
    {
        document->m_locator = std::move(destination);
    }
    closeGuard.dismissed = true;
    auto marked = mark_saved(a_documentId, savedState);
    if (!marked)
    {
        if (document->m_closeState == DocumentCloseState::SaveRequested)
        {
            document->m_closeState = DocumentCloseState::AwaitingDecision;
        }
        return Result<scene::SceneSaveStatus>::failure(std::move(*marked.try_error()));
    }
    return Result<scene::SceneSaveStatus>::success(std::move(status));
}

Result<void> EditorController::discard_uncertain_save(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                                                "Editor document was not found", a_documentId.value()));
    }
    if (!document->m_pendingSave.has_value() || document->m_persistenceState != DocumentPersistenceState::SaveUncertain)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::InvalidDocumentState,
                                                                "Discard requires a Save Uncertain record",
                                                                a_documentId.value()));
    }
    document->m_pendingSave.reset();
    document->m_persistenceState = DocumentPersistenceState::Idle;
    if (document->m_closeState == DocumentCloseState::SaveRequested)
    {
        document->m_closeState = DocumentCloseState::AwaitingDecision;
    }
    return Result<void>::success();
}

Result<DocumentStateId> EditorController::reload_document(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    auto services = require_persistence_services();
    if (!services)
    {
        return Result<DocumentStateId>::failure(std::move(*services.try_error()));
    }
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<DocumentStateId>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }
    if (!document->m_hasSavedDestination || document->m_nextStateId == std::numeric_limits<std::uint64_t>::max())
    {
        return Result<DocumentStateId>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::InvalidDocumentState,
            "Reload requires a saved destination and available state identity", a_documentId.value()));
    }
    if (document->m_pendingSave.has_value())
    {
        return Result<DocumentStateId>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::InvalidDocumentState,
            "Save Uncertain must be retried or discarded before reload", a_documentId.value()));
    }
    if (document->m_closeState != DocumentCloseState::Open)
    {
        return Result<DocumentStateId>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::InvalidDocumentState,
                                       "Reload requires an open document", a_documentId.value()));
    }

    auto beforeFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, document->m_locator, *m_assertContext);
    if (!beforeFingerprint)
    {
        return Result<DocumentStateId>::failure(std::move(*beforeFingerprint.try_error()));
    }
    auto loaded =
        scene::load_scene_document(*m_sourceAssetsRoot, document->m_locator, *m_schemaRegistry, *m_valueSchemaRegistry,
                                   *m_sceneMigrations, *m_componentMigrations, *m_assertContext);
    if (!loaded)
    {
        return Result<DocumentStateId>::failure(std::move(*loaded.try_error()));
    }
    if (loaded.try_value()->document().scene_asset_id() != document->m_document.scene_asset_id())
    {
        return Result<DocumentStateId>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::SceneMismatch,
            "Reloaded scene identity does not match the open document", a_documentId.value()));
    }
    auto afterFingerprint = fingerprint_scene_file(*m_sourceAssetsRoot, document->m_locator, *m_assertContext);
    if (!afterFingerprint)
    {
        return Result<DocumentStateId>::failure(std::move(*afterFingerprint.try_error()));
    }
    if (*beforeFingerprint.try_value() != *afterFingerprint.try_value())
    {
        return Result<DocumentStateId>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::ExternalConflict,
                                       "Scene changed while reload was in progress", a_documentId.value()));
    }

    document->m_document = std::move(loaded.try_value()->document());
    auto state = issue_persistent_state(a_documentId, true);
    if (!state)
    {
        return state;
    }
    document->m_savedStateId = *state.try_value();
    document->m_baseFingerprint = *afterFingerprint.try_value();
    document->m_externalChangeState = ExternalChangeState::None;
    document->m_persistenceState = DocumentPersistenceState::Idle;
    document->m_closeState = DocumentCloseState::Open;
    return state;
}

Result<void> EditorController::autosave_recovery(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    auto services = require_persistence_services();
    if (!services)
    {
        return services;
    }
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                                                "Editor document was not found", a_documentId.value()));
    }
    if (!document->is_dirty() && document->m_hasSavedDestination)
    {
        return Result<void>::success();
    }

    auto serialized = scene::serialize_scene_document(document->m_document, *m_assertContext);
    if (!serialized)
    {
        return Result<void>::failure(std::move(*serialized.try_error()));
    }
    auto recoveryDirectory = RelativePath::parse("Editor/Recovery", *m_assertContext);
    auto recoveryPath = make_recovery_path(document->m_document.scene_asset_id(), *m_assertContext);
    if (!recoveryDirectory || !recoveryPath)
    {
        return Result<void>::failure(
            std::move(*(recoveryDirectory ? recoveryPath.try_error() : recoveryDirectory.try_error())));
    }
    auto created = m_savedRoot->create_directories(*recoveryDirectory.try_value());
    if (!created)
    {
        return created;
    }
    auto registered = register_recovery_candidate(*m_savedRoot, m_session.m_descriptor,
                                                  document->m_document.scene_asset_id(), *m_assertContext);
    if (!registered)
    {
        return registered;
    }
    auto expectedRecovery =
        fingerprint_file(*m_savedRoot, *recoveryPath.try_value(),
                         scene::k_maximumSceneBytes + k_maximumRecoveryHeaderBytes, *m_assertContext);
    if (!expectedRecovery)
    {
        return Result<void>::failure(std::move(*expectedRecovery.try_error()));
    }
    auto recoveryLease = m_savedRoot->acquire_file_write_lease(*recoveryPath.try_value());
    if (!recoveryLease)
    {
        return Result<void>::failure(std::move(*recoveryLease.try_error()));
    }
    auto leasedRecovery = fingerprint_file(*m_savedRoot, *recoveryPath.try_value(),
                                           scene::k_maximumSceneBytes + k_maximumRecoveryHeaderBytes, *m_assertContext);
    if (!leasedRecovery)
    {
        return Result<void>::failure(std::move(*leasedRecovery.try_error()));
    }
    if (*leasedRecovery.try_value() != *expectedRecovery.try_value())
    {
        return Result<void>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::ExternalConflict,
            "Recovery destination changed before the write lease was acquired", a_documentId.value()));
    }

    const scene::IdentityText sceneText = document->m_document.scene_asset_id().canonical_text();
    const SceneFileFingerprint base = document->m_baseFingerprint.value_or(SceneFileFingerprint{});
    const std::uint64_t sceneDigest = digest_text(*serialized.try_value());
    try
    {
        std::string envelope;
        envelope.reserve(k_maximumRecoveryHeaderBytes + serialized.try_value()->size());
        envelope.append(k_recoveryMagic);
        envelope.push_back('\n');
        envelope.append(std::to_string(k_recoveryFormatVersion));
        envelope.push_back('\n');
        envelope.append(m_session.m_descriptor.project_id().text());
        envelope.push_back('\n');
        envelope.append(sceneText.data(), sceneText.size());
        envelope.push_back('\n');
        envelope.append(document->m_locator.text());
        envelope.push_back('\n');
        envelope.append(base.exists ? "1\n" : "0\n");
        envelope.append(std::to_string(base.byteSize));
        envelope.push_back('\n');
        envelope.append(std::to_string(base.contentDigest));
        envelope.push_back('\n');
        envelope.append(std::to_string(document->m_currentStateId.value()));
        envelope.push_back('\n');
        envelope.append(std::to_string(sceneDigest));
        envelope.push_back('\n');
        envelope.append(std::to_string(serialized.try_value()->size()));
        envelope.append("\n\n");
        envelope.append(*serialized.try_value());
        const auto bytes = std::as_bytes(std::span(envelope.data(), envelope.size()));
        auto written = m_savedRoot->write_file_atomic_if_unchanged(
            *recoveryLease.try_value(), *recoveryPath.try_value(), *expectedRecovery.try_value(),
            scene::k_maximumSceneBytes + k_maximumRecoveryHeaderBytes, bytes);
        if (!written)
        {
            return written;
        }
        document->m_hasRecoveryCandidate = true;
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation();
    }
    catch (...)
    {
        terminate_exception();
    }
}

Result<std::vector<RecoveryCandidateInspection>> EditorController::list_recovery_candidates() noexcept
{
    assert_owner_thread();
    auto services = require_persistence_services();
    if (!services)
    {
        return Result<std::vector<RecoveryCandidateInspection>>::failure(std::move(*services.try_error()));
    }
    auto registryPath = make_recovery_registry_path(*m_assertContext);
    if (!registryPath)
    {
        return Result<std::vector<RecoveryCandidateInspection>>::failure(std::move(*registryPath.try_error()));
    }
    auto registryEntry = m_savedRoot->query_entry(*registryPath.try_value());
    if (!registryEntry)
    {
        return Result<std::vector<RecoveryCandidateInspection>>::failure(std::move(*registryEntry.try_error()));
    }
    if (*registryEntry.try_value() == EntryType::Missing)
    {
        return Result<std::vector<RecoveryCandidateInspection>>::success({});
    }
    if (*registryEntry.try_value() != EntryType::RegularFile)
    {
        return Result<std::vector<RecoveryCandidateInspection>>::failure(make_editor_core_error(
            *m_assertContext, EditorCoreError::InvalidRecovery, "Recovery registry is not a regular file"));
    }

    auto registryBytes = m_savedRoot->read_file(*registryPath.try_value(), k_maximumRecoveryRegistryBytes);
    if (!registryBytes)
    {
        return Result<std::vector<RecoveryCandidateInspection>>::failure(std::move(*registryBytes.try_error()));
    }
    const auto &registryStorage = *registryBytes.try_value();
    auto sceneIds = parse_recovery_registry(
        std::string_view(reinterpret_cast<const char *>(registryStorage.data()), registryStorage.size()),
        m_session.m_descriptor, *m_assertContext);
    if (!sceneIds)
    {
        return Result<std::vector<RecoveryCandidateInspection>>::failure(std::move(*sceneIds.try_error()));
    }

    try
    {
        std::vector<RecoveryCandidateInspection> inspections;
        inspections.reserve(sceneIds.try_value()->size());
        for (const scene::SceneAssetId &sceneId : *sceneIds.try_value())
        {
            const scene::IdentityText sceneText = sceneId.canonical_text();
            std::string sceneIdText(sceneText.data(), sceneText.size());
            auto recoveryPath = make_recovery_path(sceneId, *m_assertContext);
            if (!recoveryPath)
            {
                inspections.emplace_back(std::move(sceneIdText), std::move(*recoveryPath.try_error()));
                continue;
            }
            auto recoveryEntry = m_savedRoot->query_entry(*recoveryPath.try_value());
            if (!recoveryEntry)
            {
                inspections.emplace_back(std::move(sceneIdText), std::move(*recoveryEntry.try_error()));
                continue;
            }
            if (*recoveryEntry.try_value() == EntryType::Missing)
            {
                continue;
            }
            if (*recoveryEntry.try_value() != EntryType::RegularFile)
            {
                inspections.emplace_back(
                    std::move(sceneIdText),
                    make_editor_core_error(*m_assertContext, EditorCoreError::InvalidRecovery,
                                           "Recovery registry entry does not refer to a regular file"));
                continue;
            }
            auto bytes = m_savedRoot->read_file(*recoveryPath.try_value(),
                                                scene::k_maximumSceneBytes + k_maximumRecoveryHeaderBytes);
            if (!bytes)
            {
                inspections.emplace_back(std::move(sceneIdText), std::move(*bytes.try_error()));
                continue;
            }
            const auto &storage = *bytes.try_value();
            auto parsed = parse_recovery_envelope(
                std::string_view(reinterpret_cast<const char *>(storage.data()), storage.size()), *m_assertContext);
            if (!parsed)
            {
                inspections.emplace_back(std::move(sceneIdText), std::move(*parsed.try_error()));
                continue;
            }
            auto identity = validate_recovery_candidate(parsed.try_value()->metadata, m_session.m_descriptor, sceneId,
                                                        *m_assertContext);
            if (!identity)
            {
                inspections.emplace_back(std::move(sceneIdText), std::move(*identity.try_error()));
                continue;
            }
            auto recovered =
                scene::parse_scene_document(parsed.try_value()->sceneJson, *m_schemaRegistry, *m_valueSchemaRegistry,
                                            *m_sceneMigrations, *m_componentMigrations, *m_assertContext);
            if (!recovered)
            {
                inspections.emplace_back(std::move(sceneIdText), std::move(*recovered.try_error()));
                continue;
            }
            if (recovered.try_value()->document().scene_asset_id() != sceneId)
            {
                inspections.emplace_back(
                    std::move(sceneIdText),
                    make_editor_core_error(*m_assertContext, EditorCoreError::SceneMismatch,
                                           "Recovery scene identity does not match its registry entry"));
                continue;
            }
            inspections.emplace_back(std::move(sceneIdText), std::move(parsed.try_value()->metadata));
        }
        return Result<std::vector<RecoveryCandidateInspection>>::success(std::move(inspections));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation();
    }
    catch (...)
    {
        terminate_exception();
    }
}

Result<EditorDocumentId> EditorController::open_document_from_recovery(std::string_view a_sceneId) noexcept
{
    assert_owner_thread();
    auto services = require_persistence_services();
    if (!services)
    {
        return Result<EditorDocumentId>::failure(std::move(*services.try_error()));
    }
    auto sceneId = scene::SceneAssetId::parse(a_sceneId, *m_assertContext);
    if (!sceneId)
    {
        return Result<EditorDocumentId>::failure(std::move(*sceneId.try_error()));
    }
    auto recoveryPath = make_recovery_path(*sceneId.try_value(), *m_assertContext);
    if (!recoveryPath)
    {
        return Result<EditorDocumentId>::failure(std::move(*recoveryPath.try_error()));
    }
    auto bytes =
        m_savedRoot->read_file(*recoveryPath.try_value(), scene::k_maximumSceneBytes + k_maximumRecoveryHeaderBytes);
    if (!bytes)
    {
        return Result<EditorDocumentId>::failure(std::move(*bytes.try_error()));
    }
    const auto &storage = *bytes.try_value();
    auto parsed = parse_recovery_envelope(
        std::string_view(reinterpret_cast<const char *>(storage.data()), storage.size()), *m_assertContext);
    if (!parsed)
    {
        return Result<EditorDocumentId>::failure(std::move(*parsed.try_error()));
    }
    auto identity = validate_recovery_candidate(parsed.try_value()->metadata, m_session.m_descriptor,
                                                *sceneId.try_value(), *m_assertContext);
    if (!identity)
    {
        return Result<EditorDocumentId>::failure(std::move(*identity.try_error()));
    }
    auto recovered =
        scene::parse_scene_document(parsed.try_value()->sceneJson, *m_schemaRegistry, *m_valueSchemaRegistry,
                                    *m_sceneMigrations, *m_componentMigrations, *m_assertContext);
    if (!recovered)
    {
        return Result<EditorDocumentId>::failure(std::move(*recovered.try_error()));
    }
    if (recovered.try_value()->document().scene_asset_id() != *sceneId.try_value())
    {
        return Result<EditorDocumentId>::failure(
            make_editor_core_error(*m_assertContext, EditorCoreError::SceneMismatch,
                                   "Recovery scene identity does not match the requested registry entry"));
    }

    const SceneFileFingerprint recoveryBase = parsed.try_value()->metadata.base_fingerprint();
    RelativePath locator = parsed.try_value()->metadata.source_locator();
    auto currentBase = fingerprint_scene_file(*m_sourceAssetsRoot, locator, *m_assertContext);
    const ExternalChangeState observedState =
        !currentBase
            ? ExternalChangeState::Unknown
            : (recoveryBase == *currentBase.try_value()
                   ? ExternalChangeState::None
                   : (currentBase.try_value()->exists ? ExternalChangeState::Modified : ExternalChangeState::Removed));
    auto opened = open_document(std::move(recovered.try_value()->document()), std::move(locator), recoveryBase.exists);
    if (!opened)
    {
        return opened;
    }
    EditorDocument *document = find_document(*opened.try_value());
    auto state = issue_persistent_state(*opened.try_value(), true);
    if (!state)
    {
        return Result<EditorDocumentId>::failure(std::move(*state.try_error()));
    }
    document->m_baseFingerprint = recoveryBase;
    document->m_externalChangeState = observedState;
    document->m_persistenceState = DocumentPersistenceState::Idle;
    document->m_closeState = DocumentCloseState::Open;
    document->m_hasRecoveryCandidate = true;
    return opened;
}

Result<RecoveryMetadata> EditorController::inspect_recovery(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    auto services = require_persistence_services();
    if (!services)
    {
        return Result<RecoveryMetadata>::failure(std::move(*services.try_error()));
    }
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<RecoveryMetadata>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }
    auto recoveryPath = make_recovery_path(document->m_document.scene_asset_id(), *m_assertContext);
    if (!recoveryPath)
    {
        return Result<RecoveryMetadata>::failure(std::move(*recoveryPath.try_error()));
    }
    auto bytes =
        m_savedRoot->read_file(*recoveryPath.try_value(), scene::k_maximumSceneBytes + k_maximumRecoveryHeaderBytes);
    if (!bytes)
    {
        return Result<RecoveryMetadata>::failure(std::move(*bytes.try_error()));
    }
    const auto &storage = *bytes.try_value();
    auto parsed = parse_recovery_envelope(
        std::string_view(reinterpret_cast<const char *>(storage.data()), storage.size()), *m_assertContext);
    if (!parsed)
    {
        return Result<RecoveryMetadata>::failure(std::move(*parsed.try_error()));
    }
    auto identity =
        validate_recovery_identity(parsed.try_value()->metadata, m_session.m_descriptor, *document, *m_assertContext);
    if (!identity)
    {
        return Result<RecoveryMetadata>::failure(std::move(*identity.try_error()));
    }
    auto sceneDocument =
        scene::parse_scene_document(parsed.try_value()->sceneJson, *m_schemaRegistry, *m_valueSchemaRegistry,
                                    *m_sceneMigrations, *m_componentMigrations, *m_assertContext);
    if (!sceneDocument)
    {
        return Result<RecoveryMetadata>::failure(std::move(*sceneDocument.try_error()));
    }
    if (sceneDocument.try_value()->document().scene_asset_id() != document->m_document.scene_asset_id())
    {
        return Result<RecoveryMetadata>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::SceneMismatch,
            "Recovery scene identity does not match the open document", a_documentId.value()));
    }
    document->m_hasRecoveryCandidate = true;
    return Result<RecoveryMetadata>::success(std::move(parsed.try_value()->metadata));
}

Result<DocumentStateId> EditorController::recover_document(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    auto services = require_persistence_services();
    if (!services)
    {
        return Result<DocumentStateId>::failure(std::move(*services.try_error()));
    }
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<DocumentStateId>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }
    if (document->m_nextStateId == std::numeric_limits<std::uint64_t>::max())
    {
        return Result<DocumentStateId>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::RevisionExhausted,
                                       "Recovery requires an available document state identity", a_documentId.value()));
    }
    if (document->m_pendingSave.has_value())
    {
        return Result<DocumentStateId>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::InvalidDocumentState,
            "Save Uncertain must be retried or discarded before recovery", a_documentId.value()));
    }
    if (document->m_closeState != DocumentCloseState::Open)
    {
        return Result<DocumentStateId>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::InvalidDocumentState,
                                       "Recovery requires an open document", a_documentId.value()));
    }
    auto recoveryPath = make_recovery_path(document->m_document.scene_asset_id(), *m_assertContext);
    if (!recoveryPath)
    {
        return Result<DocumentStateId>::failure(std::move(*recoveryPath.try_error()));
    }
    auto bytes =
        m_savedRoot->read_file(*recoveryPath.try_value(), scene::k_maximumSceneBytes + k_maximumRecoveryHeaderBytes);
    if (!bytes)
    {
        return Result<DocumentStateId>::failure(std::move(*bytes.try_error()));
    }
    const auto &storage = *bytes.try_value();
    auto parsed = parse_recovery_envelope(
        std::string_view(reinterpret_cast<const char *>(storage.data()), storage.size()), *m_assertContext);
    if (!parsed)
    {
        return Result<DocumentStateId>::failure(std::move(*parsed.try_error()));
    }
    auto identity =
        validate_recovery_identity(parsed.try_value()->metadata, m_session.m_descriptor, *document, *m_assertContext);
    if (!identity)
    {
        return Result<DocumentStateId>::failure(std::move(*identity.try_error()));
    }
    auto recovered =
        scene::parse_scene_document(parsed.try_value()->sceneJson, *m_schemaRegistry, *m_valueSchemaRegistry,
                                    *m_sceneMigrations, *m_componentMigrations, *m_assertContext);
    if (!recovered)
    {
        return Result<DocumentStateId>::failure(std::move(*recovered.try_error()));
    }
    if (recovered.try_value()->document().scene_asset_id() != document->m_document.scene_asset_id())
    {
        return Result<DocumentStateId>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::SceneMismatch,
            "Recovery scene identity does not match the open document", a_documentId.value()));
    }

    const SceneFileFingerprint recoveryBase = parsed.try_value()->metadata.base_fingerprint();
    auto currentBase = fingerprint_scene_file(*m_sourceAssetsRoot, document->m_locator, *m_assertContext);
    if (!currentBase)
    {
        return Result<DocumentStateId>::failure(std::move(*currentBase.try_error()));
    }
    document->m_document = std::move(recovered.try_value()->document());
    auto state = issue_persistent_state(a_documentId, true);
    if (!state)
    {
        return state;
    }
    document->m_baseFingerprint = recoveryBase;
    document->m_externalChangeState =
        recoveryBase == *currentBase.try_value()
            ? ExternalChangeState::None
            : (currentBase.try_value()->exists ? ExternalChangeState::Modified : ExternalChangeState::Removed);
    document->m_persistenceState = DocumentPersistenceState::Idle;
    document->m_closeState = DocumentCloseState::Open;
    document->m_hasRecoveryCandidate = true;
    return state;
}

Result<void> EditorController::ignore_recovery(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                                                "Editor document was not found", a_documentId.value()));
    }
    document->m_hasRecoveryCandidate = false;
    return Result<void>::success();
}

Result<void> EditorController::discard_recovery(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    auto services = require_persistence_services();
    if (!services)
    {
        return services;
    }
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                                                "Editor document was not found", a_documentId.value()));
    }
    auto recoveryPath = make_recovery_path(document->m_document.scene_asset_id(), *m_assertContext);
    if (!recoveryPath)
    {
        return Result<void>::failure(std::move(*recoveryPath.try_error()));
    }
    auto lease = m_savedRoot->acquire_file_write_lease(*recoveryPath.try_value());
    if (!lease)
    {
        return Result<void>::failure(std::move(*lease.try_error()));
    }
    auto removed = m_savedRoot->remove_file(*recoveryPath.try_value());
    if (!removed)
    {
        return removed;
    }
    document->m_hasRecoveryCandidate = false;
    return Result<void>::success();
}
} // namespace cue::editor_core
