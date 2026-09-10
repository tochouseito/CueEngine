#pragma once

#include <Cue/IO/Filesystem.h>

#include <cstdint>
#include <optional>
#include <string>

namespace cue
{
namespace schema
{
class SchemaRegistry;
}

namespace scene
{
class ComponentMigrationRegistry;
class ComponentValueSchemaRegistry;
class SceneMigrationRegistry;
} // namespace scene
} // namespace cue

namespace cue::editor_core
{
/// @brief Project Root、Scene正本Root、Recovery Root、Schema群をEditor Sessionへ明示注入する非所有Service集合
class ScenePersistenceServices final
{
  public:
    /// @brief Controllerより長く生存するProject DescriptorとScene Persistence依存を束ねる
    ScenePersistenceServices(FilesystemRoot &a_projectRoot, FilesystemRoot &a_sourceAssetsRoot,
                             FilesystemRoot &a_savedRoot, const schema::SchemaRegistry &a_schemaRegistry,
                             const scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry,
                             const scene::SceneMigrationRegistry &a_sceneMigrations,
                             const scene::ComponentMigrationRegistry &a_componentMigrations) noexcept;
    /// @brief Controllerより長く生存するPersistence依存を束ねる
    /// @details Project Rootを所有しない単体利用では、Session生成時のDescriptorをStartup Scene読込に使用する
    ScenePersistenceServices(FilesystemRoot &a_sourceAssetsRoot, FilesystemRoot &a_savedRoot,
                             const schema::SchemaRegistry &a_schemaRegistry,
                             const scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry,
                             const scene::SceneMigrationRegistry &a_sceneMigrations,
                             const scene::ComponentMigrationRegistry &a_componentMigrations) noexcept;

  private:
    friend class EditorController;

    FilesystemRoot *m_projectRoot;
    FilesystemRoot *m_sourceAssetsRoot;
    FilesystemRoot *m_savedRoot;
    const schema::SchemaRegistry *m_schemaRegistry;
    const scene::ComponentValueSchemaRegistry *m_valueSchemaRegistry;
    const scene::SceneMigrationRegistry *m_sceneMigrations;
    const scene::ComponentMigrationRegistry *m_componentMigrations;
};

/// @brief Scene Fileの存在状態、Byte数、Content Digestを一つの比較値として保持する
using SceneFileFingerprint = FileFingerprint;

/// @brief EditorDocumentのSave公開結果が確定済みか表す
enum class DocumentPersistenceState : std::uint8_t
{
    Idle,
    SaveUncertain
};

/// @brief 検証済みRecovery EnvelopeのUI非依存Metadata
class RecoveryMetadata final
{
  public:
    /// @brief Recovery Metadataを所有値として構築する
    RecoveryMetadata(std::uint32_t a_formatVersion, std::string a_projectId, std::string a_sceneId,
                     RelativePath a_sourceLocator, SceneFileFingerprint a_baseFingerprint,
                     std::uint64_t a_sourceStateValue, std::uint64_t a_sceneDigest) noexcept;

    /// @brief Recovery Format Versionを返す
    [[nodiscard]] std::uint32_t format_version() const noexcept;
    /// @brief Recoveryを書き込んだProject Identityを返す
    [[nodiscard]] const std::string &project_id() const noexcept;
    /// @brief Recoveryが保持するScene Identityを返す
    [[nodiscard]] const std::string &scene_id() const noexcept;
    /// @brief Recoveryが対応するScene正本Locatorを返す
    [[nodiscard]] const RelativePath &source_locator() const noexcept;
    /// @brief Recovery作成時の正本Fingerprintを返す
    [[nodiscard]] SceneFileFingerprint base_fingerprint() const noexcept;
    /// @brief Recovery作成時のDocument State値を返す
    [[nodiscard]] std::uint64_t source_state_value() const noexcept;
    /// @brief Recovery本文のContent Digestを返す
    [[nodiscard]] std::uint64_t scene_digest() const noexcept;

  private:
    std::uint32_t m_formatVersion;
    std::string m_projectId;
    std::string m_sceneId;
    RelativePath m_sourceLocator;
    SceneFileFingerprint m_baseFingerprint;
    std::uint64_t m_sourceStateValue;
    std::uint64_t m_sceneDigest;
};

/// @brief 一つのRegistry Entryに対する検証済み候補または隔離診断
class RecoveryCandidateInspection final
{
  public:
    /// @brief 完全検証済みRecovery候補を所有する
    RecoveryCandidateInspection(std::string a_sceneId, RecoveryMetadata a_metadata) noexcept;
    /// @brief 他候補の列挙を妨げないEntry単位Errorを所有する
    RecoveryCandidateInspection(std::string a_sceneId, Error a_error) noexcept;

    /// @brief Registryが保持するScene Identityを返す
    [[nodiscard]] const std::string &scene_id() const noexcept;
    /// @brief 検証済みMetadataがあればPointerを返す
    [[nodiscard]] const RecoveryMetadata *try_metadata() const noexcept;
    /// @brief 隔離されたEntry単位ErrorがあればPointerを返す
    [[nodiscard]] const Error *try_error() const noexcept;

  private:
    std::string m_sceneId;
    std::optional<RecoveryMetadata> m_metadata;
    std::optional<Error> m_error;
};
} // namespace cue::editor_core
