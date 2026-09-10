#pragma once

#include <Cue/Build/Plan.h>
#include <Cue/Editor/ImGui/BuildPresenter.h>
#include <Cue/EditorCore/EditorController.h>
#include <Cue/Package/Workflow.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cue
{
class AssertContext;
class Error;
} // namespace cue

namespace cue::editor
{
/// @brief Package UIからWorkflow Serviceへ送る意味Command
enum class EditorPackageCommand : std::uint8_t
{
    Start,
    Cancel,
    Retry,
    Run,
    Stop
};

/// @brief PackageまたはRuntime実行中のEditor終了確認へ適用するUser判断
enum class EditorPackageShutdownDecision : std::uint8_t
{
    StopAndClose,
    KeepEditorOpen
};

/// @brief Build・Package・Run Coordinatorの状態を一つのImGui Windowへ変換するAdapter
///
/// Workflow ServiceとEditor ControllerはPresenterより長く生存させる。PresenterはFilesystemまたはProcess APIを
/// 直接参照せず、生成Threadだけで操作する。
class PackagePresenter final
{
  public:
    PackagePresenter(const PackagePresenter &) = delete;
    PackagePresenter &operator=(const PackagePresenter &) = delete;
    PackagePresenter(PackagePresenter &&) = delete;
    PackagePresenter &operator=(PackagePresenter &&) = delete;
    /// @brief Operation ID SourceとUI状態を解放する
    ~PackagePresenter() noexcept = default;

    /// @brief Workflow、Project Session、Build設定を一つのPresentation Ownerへ束ねる
    [[nodiscard]] static std::unique_ptr<PackagePresenter> create(
        package::GamePackageWorkflowService &a_service, editor_core::EditorController &a_controller,
        std::string a_projectRoot, BuildWorkspaceCompatibility a_workspaceCompatibility,
        std::unique_ptr<BuildOperationIdSource> a_operationIdSource, const AssertContext &a_assertContext) noexcept;

    /// @brief Coordinatorを進めてUI Snapshotと終了待ちを同期する
    void refresh() noexcept;
    /// @brief Build、Package、Run操作と結果を描画する
    void draw() noexcept;
    /// @brief Commandが現在有効な場合だけWorkflow Serviceへ送る
    [[nodiscard]] bool submit(EditorPackageCommand a_command) noexcept;
    /// @brief Package対象のActive Document Identityを更新する
    void set_active_document(std::optional<editor_core::EditorDocumentId> a_documentId) noexcept;
    /// @brief 次回Startで使用するConfigurationを停止中だけ変更する
    [[nodiscard]] bool set_configuration(BuildConfiguration a_configuration) noexcept;
    /// @brief 次回StartがConfigureを必須にするか停止中だけ変更する
    [[nodiscard]] bool set_force_configure(bool a_forceConfigure) noexcept;

    /// @brief Active Workflowがあれば終了確認を開始し、停止済みなら即時終了可能を返す
    [[nodiscard]] bool begin_editor_shutdown() noexcept;
    /// @brief 終了確認判断を適用する
    [[nodiscard]] bool respond_to_editor_shutdown(EditorPackageShutdownDecision a_decision) noexcept;
    /// @brief Cancel完了後の終了可能通知を一度だけ返す
    [[nodiscard]] bool take_shutdown_ready() noexcept;

    /// @brief 現在のWorkflow Snapshotを返す
    [[nodiscard]] const package::PackageWorkflowSnapshot &current_snapshot() const noexcept;
    /// @brief 最後のUI操作またはCoordinator診断Messageを返す
    [[nodiscard]] std::string_view message() const noexcept;
    /// @brief 現在Messageが失敗を表すか返す
    [[nodiscard]] bool has_error_message() const noexcept;
    /// @brief 新しいBuild・Package開始が可能か返す
    [[nodiscard]] bool can_start() const noexcept;
    /// @brief Active Workflowを取消できるか返す
    [[nodiscard]] bool can_cancel() const noexcept;
    /// @brief 最後のWorkflowを新Identityで再実行できるか返す
    [[nodiscard]] bool can_retry() const noexcept;
    /// @brief 公開済みPackageを起動できるか返す
    [[nodiscard]] bool can_run() const noexcept;
    /// @brief 起動中Runtimeを停止できるか返す
    [[nodiscard]] bool can_stop() const noexcept;
    /// @brief Editor終了確認がUser判断またはCancel完了待ちか返す
    [[nodiscard]] bool is_shutdown_confirmation_pending() const noexcept;

  private:
    /// @brief 検証済み依存とUI状態からPresenterを構築する
    PackagePresenter(package::GamePackageWorkflowService &a_service, editor_core::EditorController &a_controller,
                     std::string a_projectRoot, BuildWorkspaceCompatibility a_workspaceCompatibility,
                     std::unique_ptr<BuildOperationIdSource> a_operationIdSource,
                     const AssertContext &a_assertContext) noexcept;

    /// @brief Foundation Errorを日本語UIへ変換する
    void set_error(const Error &a_error, std::string_view a_operation);
    /// @brief 正常操作の短い日本語Messageを設定する
    void set_status(std::string_view a_status);
    /// @brief Build・Package・Run操作Toolbarを描画する
    void draw_toolbar() noexcept;
    /// @brief Stage、Manifest、Artifact、Output Locator、Run Logを描画する
    void draw_result() noexcept;
    /// @brief Active WorkflowのEditor終了確認を描画する
    void draw_shutdown_confirmation() noexcept;
    /// @brief 終了確認を解除して一回のClose Ready通知を確定する
    void mark_shutdown_ready() noexcept;
    /// @brief 予期しない例外をFatalHandlerへ渡す
    [[noreturn]] void terminate_exception() const noexcept;

    package::GamePackageWorkflowService *m_service;
    editor_core::EditorController *m_controller;
    const AssertContext *m_assertContext;
    std::unique_ptr<BuildOperationIdSource> m_operationIdSource;
    std::string m_projectRoot;
    BuildWorkspaceCompatibility m_workspaceCompatibility;
    package::PackageWorkflowSnapshot m_current;
    std::optional<editor_core::EditorDocumentId> m_activeDocumentId;
    std::string m_message;
    BuildConfiguration m_configuration = BuildConfiguration::Debug;
    bool m_forceConfigure = true;
    bool m_hasError = false;
    bool m_hasPresenterDiagnostic = false;
    bool m_openShutdownConfirmation = false;
    bool m_closeShutdownConfirmation = false;
    bool m_isShutdownConfirmationPending = false;
    bool m_isShutdownWaitingForCancel = false;
    bool m_isShutdownReady = false;
};
} // namespace cue::editor
