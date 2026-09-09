#pragma once

#include <Cue/ProjectHub/Service.h>

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace cue
{
class AssertContext;
}

namespace cue::project_hub
{
/// @brief ProjectHubServiceの所有ViewをImGui操作へ変換するPresentation Adapter
class ProjectHubPresenter final
{
  public:
    /// @brief Presentation StateとService参照の一意性を保つためCopy構築を禁止する
    ProjectHubPresenter(const ProjectHubPresenter &) = delete;
    /// @brief Presentation StateとService参照の一意性を保つためCopy代入を禁止する
    ProjectHubPresenter &operator=(const ProjectHubPresenter &) = delete;
    /// @brief ImGui Session中のAddress安定性を保つためMove構築を禁止する
    ProjectHubPresenter(ProjectHubPresenter &&) = delete;
    /// @brief ImGui Session中のAddress安定性を保つためMove代入を禁止する
    ProjectHubPresenter &operator=(ProjectHubPresenter &&) = delete;

    /// @brief Presentation Stateを解放し、注入Serviceの所有権は変更しない
    ~ProjectHubPresenter() = default;

    /// @brief Serviceより短命なPresentation Adapterを初期状態から生成する
    [[nodiscard]] static Result<std::unique_ptr<ProjectHubPresenter>> create(
        ProjectHubService &a_service, const AssertContext &a_assertContext) noexcept;

    /// @brief 現在のService ViewからProject Hub画面を描画し、Editor起動可否を操作へ反映する
    void draw(bool a_canLaunchEditor = true) noexcept;

    /// @brief Open操作で生成されたEditor Launch Requestを一度だけ移動して返す
    [[nodiscard]] std::optional<EditorLaunchRequest> take_editor_launch_request() noexcept;

    /// @brief Composition Rootで失敗したEditor Process起動を日本語Messageへ反映する
    void report_editor_launch_failure(const Error &a_error) noexcept;

    /// @brief 監視中Editor Processの正常終了を再操作可能な状態として表示する
    void report_editor_process_completed() noexcept;

    /// @brief Escapeまたは終了操作がTool Session終了を要求したか返す
    [[nodiscard]] bool is_exit_requested() const noexcept;

  private:
    ProjectHubPresenter(ProjectHubService &a_service, const AssertContext &a_assertContext) noexcept;

    /// @brief Recent Project一覧と選択中Projectの操作を描画する
    void draw_project_list(bool a_canLaunchEditor) noexcept;
    /// @brief Blank Project作成Dialogを描画してServiceへIntentを渡す
    void draw_create_dialog() noexcept;
    /// @brief 既存Project登録Dialogを描画してServiceへIntentを渡す
    void draw_register_dialog() noexcept;
    /// @brief 一覧除外確認Dialogを描画する
    void draw_remove_dialog() noexcept;
    /// @brief 旧Project Descriptorの明示Migration確認Dialogを描画する
    void draw_migrate_dialog() noexcept;
    /// @brief 選択Projectを再検証してEditor Launch Requestを生成する
    void open_selected_project() noexcept;
    /// @brief Service Errorを日本語のUser Messageへ変換する
    void set_error(const Error &a_error) noexcept;
    /// @brief Project作成後の部分成功を復旧情報付き警告Messageへ変換する
    void set_creation_warning(const ProjectCreationOutcome &a_outcome) noexcept;
    /// @brief 回復操作が必要な警告Messageを更新する
    void set_warning(std::string_view a_warning) noexcept;
    /// @brief 同期操作の成功Messageを更新する
    void set_status(std::string_view a_status) noexcept;

    ProjectHubService *m_service;
    const AssertContext *m_assertContext;
    std::string m_selectedProjectId;
    std::string m_selectedTemplateId;
    std::string m_pendingRemoveProjectId;
    std::string m_pendingMigrateProjectId;
    std::string m_message;
    std::optional<EditorLaunchRequest> m_launchRequest;
    std::string m_parentLocator;
    std::string m_registerLocator;
    std::array<char, 128> m_projectName{};
    std::array<char, 257> m_displayName{};
    bool m_openCreateDialog = false;
    bool m_openRegisterDialog = false;
    bool m_openRemoveDialog = false;
    bool m_openMigrateDialog = false;
    bool m_confirmMovedProject = false;
    bool m_hasError = false;
    bool m_hasWarning = false;
    bool m_isExitRequested = false;
};
} // namespace cue::project_hub
