#pragma once

#include <Cue/EditorCore/EditorController.h>
#include <Cue/Foundation/Result.h>
#include <Cue/Project/Compatibility.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
class FilesystemRoot;
} // namespace cue

namespace cue::schema
{
class SchemaRegistry;
class SchemaRegistryIdentitySource;
} // namespace cue::schema

namespace cue::scene
{
class ComponentMigrationRegistry;
class ComponentValueSchemaRegistry;
class SceneIdentitySource;
class SceneMigrationRegistry;
} // namespace cue::scene

namespace cue::editor
{
/// @brief Editor Process起動とProject Session構築の回復可能な失敗分類
enum class WindowsEditorSessionError : std::int64_t
{
    InvalidLaunchParameters = 1,
    ProjectOpenFailed = 2,
    ProjectIdentityMismatch = 3,
    ProjectCompatibilityMismatch = 4,
    ProjectUnsupported = 5,
    SourceAssetsOpenFailed = 6,
    SavedRootOpenFailed = 7,
    SchemaInitializationFailed = 8,
    SceneOpenFailed = 9,
    InvalidSessionState = 10,
    SceneSaveFailed = 11,
    ProjectFilesInitializationFailed = 12,
};

/// @brief Project Hubから値だけで受け取るEditor Process起動入力
struct WindowsEditorLaunchParameters final
{
    std::uint32_t protocolVersion;
    std::string projectDescriptorLocator;
    std::string expectedProjectId;
    std::string engineCompatibilityId;
    std::optional<std::string> initialSceneLocator;
};

/// @brief Editor側でProject互換性を再検証する現在Engine入力
struct WindowsEditorEngineConfiguration final
{
    std::uint32_t supportedProjectFormatVersion;
    EngineVersion currentEngineVersion;
    ProjectCapabilityProfile capabilityProfile;
    ProjectCapabilitySnapshot capabilitySnapshot;
};

/// @brief Windows Root群とEditorControllerを一つのProject Sessionとして所有するComposition Root
class WindowsEditorSession final
{
  public:
    WindowsEditorSession(const WindowsEditorSession &) = delete;
    WindowsEditorSession &operator=(const WindowsEditorSession &) = delete;
    WindowsEditorSession(WindowsEditorSession &&) = delete;
    WindowsEditorSession &operator=(WindowsEditorSession &&) = delete;
    /// @brief Controllerより後にRootとRegistryを破棄する
    ~WindowsEditorSession() noexcept;

    /// @brief 起動値を再検証して完全なProject Sessionだけを公開する
    [[nodiscard]] static Result<std::unique_ptr<WindowsEditorSession>> create(
        WindowsEditorLaunchParameters a_parameters, WindowsEditorEngineConfiguration a_configuration,
        const AssertContext &a_assertContext) noexcept;

    /// @brief Project-only Sessionへ新しい未保存Sceneを一つ開く
    [[nodiscard]] Result<editor_core::EditorDocumentId> create_scene(RelativePath a_locator) noexcept;
    /// @brief Source Assets Rootから既存Sceneを完全Loadして一つ開く
    [[nodiscard]] Result<editor_core::EditorDocumentId> open_scene(RelativePath a_locator) noexcept;
    /// @brief 現在Sceneを維持したまま未保存Sceneを切替候補として準備する
    [[nodiscard]] Result<editor_core::EditorDocumentId> prepare_new_scene(RelativePath a_locator) noexcept;
    /// @brief 現在Sceneを維持したまま既存Sceneを切替候補として完全Loadする
    [[nodiscard]] Result<editor_core::EditorDocumentId> prepare_open_scene(RelativePath a_locator) noexcept;
    /// @brief 準備済みSceneへの切替に必要なActive Scene Closeを開始する
    [[nodiscard]] Result<editor_core::DocumentCloseState> request_activate_prepared_scene() noexcept;
    /// @brief 準備済みSceneだけを破棄して現在Sceneを維持する
    [[nodiscard]] Result<void> discard_prepared_scene() noexcept;
    /// @brief Saved Rootの検証済みRecovery候補からSceneを一つ開く
    [[nodiscard]] Result<editor_core::EditorDocumentId> open_recovery_scene(std::string_view a_sceneId) noexcept;
    /// @brief 現在ProjectのRecovery候補とEntry単位診断を列挙する
    [[nodiscard]] Result<std::vector<editor_core::RecoveryCandidateInspection>> list_recovery_candidates() noexcept;
    /// @brief Active Sceneを通常Saveまたは初回Save Asへ接続する
    [[nodiscard]] Result<scene::SceneSaveOutcome> save_active_scene() noexcept;
    /// @brief User確認済みの既存初回保存先を競合検査付きで置換する
    [[nodiscard]] Result<scene::SceneSaveOutcome> save_active_scene_overwriting_existing_destination() noexcept;
    /// @brief Active Sceneを未使用の別LocatorへSave Asする
    [[nodiscard]] Result<scene::SceneSaveOutcome> save_active_scene_as_new(RelativePath a_locator) noexcept;
    /// @brief User確認済みの別LocatorへActive SceneをSave Asする
    [[nodiscard]] Result<scene::SceneSaveOutcome> save_active_scene_as_overwriting(
        RelativePath a_locator) noexcept;
    /// @brief DirtyなActive SceneのRecoveryをSaved RootへAtomic保存する
    [[nodiscard]] Result<void> autosave_active_scene_recovery() noexcept;
    /// @brief Active Sceneを外部変更検査付きで再読込する
    [[nodiscard]] Result<editor_core::DocumentStateId> reload_active_scene() noexcept;
    /// @brief Active SceneのSave Uncertainを再検証または再試行する
    [[nodiscard]] Result<scene::SceneSaveStatus> retry_uncertain_save_active_scene() noexcept;
    /// @brief Active SceneのSave Uncertain記録だけを破棄してDirty内容を維持する
    [[nodiscard]] Result<void> discard_uncertain_save_active_scene() noexcept;
    /// @brief Active SceneのClose状態遷移を開始する
    [[nodiscard]] Result<editor_core::DocumentCloseState> request_close() noexcept;
    /// @brief Active SceneのSave、Discard、Cancel判断を適用する
    [[nodiscard]] Result<editor_core::DocumentCloseState> respond_to_close(
        editor_core::CloseDecision a_decision) noexcept;

    /// @brief Sessionが所有するEditor Controllerを返す
    [[nodiscard]] editor_core::EditorController &controller() noexcept;
    /// @brief Project Session全寿命で所有するFiles Workspace Serviceを返す
    [[nodiscard]] editor_core::FilesWorkspaceService &files_workspace() noexcept;
    /// @brief Sessionが所有するSchema Registryを返す
    [[nodiscard]] const schema::SchemaRegistry &schema_registry() const noexcept;
    /// @brief SceneとObjectのUUID生成に使用するSession-local Sourceを返す
    [[nodiscard]] scene::SceneIdentitySource &identity_source() noexcept;
    /// @brief Active SceneがあればProcess-local Document Identityを返す
    [[nodiscard]] const std::optional<editor_core::EditorDocumentId> &active_document_id() const noexcept;
    /// @brief Scene切替用の準備済みDocumentが存在するか返す
    [[nodiscard]] bool has_prepared_scene() const noexcept;
    /// @brief 再Open検証とWindow Titleに使用するProject Root Locatorを返す
    [[nodiscard]] std::string_view project_locator() const noexcept;

  private:
    /// @brief 検証済みProject Root群をController生成前のSessionへ移す
    WindowsEditorSession(std::string a_projectLocator, std::unique_ptr<FilesystemRoot> a_projectRoot,
                         std::unique_ptr<FilesystemRoot> a_sourceAssetsRoot,
                         std::unique_ptr<FilesystemRoot> a_savedRoot, const AssertContext &a_assertContext) noexcept;
    /// @brief Registry、Controller、任意の初期SceneをSession所有順で構築する
    [[nodiscard]] Result<void> initialize(ProjectDescriptor a_descriptor,
                                          const std::optional<std::string> &a_initialSceneLocator) noexcept;
    /// @brief Activeまたは準備済みDocumentが存在しないことを検証する
    [[nodiscard]] Result<void> require_project_only_state() const noexcept;
    /// @brief Active Documentが閉じられた場合にSession表示状態を同期する
    void reconcile_active_document() noexcept;
    /// @brief Active Sceneを初回保存先Policyに従って保存する共通経路
    [[nodiscard]] Result<scene::SceneSaveOutcome> save_active_scene_impl(
        bool a_allowExistingDestination) noexcept;
    /// @brief Save As先Directoryを準備してControllerの別Destination保存へ接続する
    [[nodiscard]] Result<scene::SceneSaveOutcome> save_active_scene_as_impl(
        RelativePath a_locator, bool a_allowExistingDestination) noexcept;
    /// @brief Scene LocatorのParent DirectoryをSource Assets Root内へ作成する
    [[nodiscard]] Result<void> ensure_scene_parent_directory(const RelativePath &a_locator) noexcept;

    std::string m_projectLocator;
    std::unique_ptr<FilesystemRoot> m_projectRoot;
    std::unique_ptr<FilesystemRoot> m_sourceAssetsRoot;
    std::unique_ptr<FilesystemRoot> m_savedRoot;
    std::unique_ptr<schema::SchemaRegistryIdentitySource> m_schemaIdentitySource;
    std::unique_ptr<schema::SchemaRegistry> m_schemaRegistry;
    std::unique_ptr<scene::ComponentValueSchemaRegistry> m_valueSchemaRegistry;
    std::unique_ptr<scene::SceneMigrationRegistry> m_sceneMigrations;
    std::unique_ptr<scene::ComponentMigrationRegistry> m_componentMigrations;
    std::unique_ptr<scene::SceneIdentitySource> m_sceneIdentitySource;
    std::unique_ptr<editor_core::EditorController> m_controller;
    std::unique_ptr<editor_core::FilesWorkspaceService> m_filesWorkspace;
    std::optional<editor_core::EditorDocumentId> m_activeDocumentId;
    std::optional<editor_core::EditorDocumentId> m_preparedDocumentId;
    const AssertContext *m_assertContext;
};
} // namespace cue::editor
