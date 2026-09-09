#include <Cue/Editor/ImGui/PackagePresenter.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Scene/Instantiation.h>

#include <cstdlib>
#include <utility>

#include <imgui.h>

namespace
{
/// @brief Workflow状態をUI表示名へ変換する
[[nodiscard]] const char *state_label(cue::package::PackageWorkflowState a_state) noexcept
{
    using cue::package::PackageWorkflowState;
    switch (a_state)
    {
    case PackageWorkflowState::Idle:
        return "Idle";
    case PackageWorkflowState::Building:
        return "Building";
    case PackageWorkflowState::Packaging:
        return "Packaging";
    case PackageWorkflowState::PackageReady:
        return "Package Ready";
    case PackageWorkflowState::Running:
        return "Running";
    case PackageWorkflowState::RunSucceeded:
        return "Run Succeeded";
    case PackageWorkflowState::Failed:
        return "Failed";
    case PackageWorkflowState::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

/// @brief Workflow StageをUI表示名へ変換する
[[nodiscard]] const char *stage_label(cue::package::PackageWorkflowStage a_stage) noexcept
{
    using cue::package::PackageWorkflowStage;
    switch (a_stage)
    {
    case PackageWorkflowStage::None:
        return "-";
    case PackageWorkflowStage::Build:
        return "Build";
    case PackageWorkflowStage::Package:
        return "Package";
    case PackageWorkflowStage::Run:
        return "Run";
    }
    return "Unknown";
}

/// @brief Build ConfigurationをUI表示名へ変換する
[[nodiscard]] const char *configuration_label(cue::BuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case cue::BuildConfiguration::Debug:
        return "Debug";
    case cue::BuildConfiguration::Development:
        return "Development";
    case cue::BuildConfiguration::Release:
        return "Release";
    }
    return "Unknown";
}

/// @brief Workflow状態がBuild、Package、RunのActive状態か返す
[[nodiscard]] bool is_active(cue::package::PackageWorkflowState a_state) noexcept
{
    return a_state == cue::package::PackageWorkflowState::Building ||
           a_state == cue::package::PackageWorkflowState::Packaging ||
           a_state == cue::package::PackageWorkflowState::Running;
}
} // namespace

namespace cue::editor
{
PackagePresenter::PackagePresenter(package::GamePackageWorkflowService &a_service,
                                   const editor_core::ProjectWorkspaceSession &a_session, std::string a_projectRoot,
                                   BuildWorkspaceCompatibility a_workspaceCompatibility,
                                   std::unique_ptr<BuildOperationIdSource> a_operationIdSource,
                                   const AssertContext &a_assertContext) noexcept
    : m_service(&a_service), m_session(&a_session), m_assertContext(&a_assertContext),
      m_operationIdSource(std::move(a_operationIdSource)), m_projectRoot(std::move(a_projectRoot)),
      m_workspaceCompatibility(a_workspaceCompatibility), m_current(a_service.snapshot())
{
}

std::unique_ptr<PackagePresenter> PackagePresenter::create(
    package::GamePackageWorkflowService &a_service, const editor_core::ProjectWorkspaceSession &a_session,
    std::string a_projectRoot, BuildWorkspaceCompatibility a_workspaceCompatibility,
    std::unique_ptr<BuildOperationIdSource> a_operationIdSource, const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!a_operationIdSource)
        {
            a_assertContext.fatal_handler().terminate("Package Presenter operation identity source is missing");
        }
        return std::unique_ptr<PackagePresenter>(new PackagePresenter(
            a_service, a_session, std::move(a_projectRoot), a_workspaceCompatibility,
            std::move(a_operationIdSource), a_assertContext));
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Package Presenter creation failed unexpectedly");
        std::abort();
    }
}

void PackagePresenter::refresh() noexcept
{
    try
    {
        m_service->advance();
        m_current = m_service->snapshot();
        if (!m_current.message.empty())
        {
            m_message = m_current.message;
        }
        m_hasError = m_current.state == package::PackageWorkflowState::Failed;
        if (m_isShutdownWaitingForCancel && !is_active(m_current.state))
        {
            mark_shutdown_ready();
        }
    }
    catch (...)
    {
        terminate_exception();
    }
}

void PackagePresenter::draw() noexcept
{
    try
    {
        refresh();
        if (ImGui::Begin("Build Package Run"))
        {
            draw_toolbar();
            if (!m_message.empty())
            {
                const ImVec4 color = m_hasError ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F)
                                                : ImVec4(0.45F, 0.85F, 0.55F, 1.0F);
                ImGui::TextColored(color, "%s", m_message.c_str());
            }
            ImGui::Separator();
            draw_result();
        }
        ImGui::End();
        draw_shutdown_confirmation();
    }
    catch (...)
    {
        terminate_exception();
    }
}

bool PackagePresenter::submit(EditorPackageCommand a_command) noexcept
{
    try
    {
        Result<void> result = Result<void>::success();
        if (a_command == EditorPackageCommand::Cancel || a_command == EditorPackageCommand::Stop)
        {
            result = a_command == EditorPackageCommand::Stop ? m_service->stop() : m_service->request_cancel();
        }
        else if (a_command == EditorPackageCommand::Run)
        {
            result = m_service->run(package::PackageRunMode::Interactive);
        }
        else
        {
            Result<std::string> operationId = m_operationIdSource->next_operation_id();
            if (!operationId)
            {
                set_error(*operationId.try_error(), "Operation IDの生成");
                return false;
            }
            if (a_command == EditorPackageCommand::Retry)
            {
                result = m_service->retry(std::move(*operationId.try_value()));
            }
            else
            {
                const editor_core::EditorDocument *document =
                    m_activeDocumentId ? m_session->find_document(*m_activeDocumentId) : nullptr;
                if (document == nullptr)
                {
                    set_status("Package対象のSceneを開いてください。");
                    m_hasError = true;
                    return false;
                }
                Result<scene::SceneSnapshot> sceneSnapshot =
                    scene::create_scene_snapshot(document->scene_document(), *m_assertContext);
                Result<package::MinimalRuntimeDataPublication> runtimeData =
                    sceneSnapshot ? package::publish_minimal_runtime_data(m_session->project_descriptor(),
                                                                          *sceneSnapshot.try_value(), *m_assertContext)
                                  : Result<package::MinimalRuntimeDataPublication>::failure(
                                        std::move(*sceneSnapshot.try_error()));
                Result<BuildProfile> profile =
                    BuildProfile::create(m_configuration, BuildTarget::GameModule, *m_assertContext);
                if (!runtimeData || !profile)
                {
                    set_error(runtimeData ? *profile.try_error() : *runtimeData.try_error(),
                              runtimeData ? "Build Profileの作成" : "Runtime Dataの生成");
                    return false;
                }
                BuildRequest request{m_projectRoot, std::move(*profile.try_value()),
                                     std::move(*operationId.try_value()), m_workspaceCompatibility};
                result = m_service->start(
                    std::move(request), m_forceConfigure ? CMakeConfigureMode::Required
                                                         : CMakeConfigureMode::ReuseCompatibleTree,
                    {1U, 0U, 0U}, std::string(m_session->project_descriptor().project_id().text()),
                    std::move(*runtimeData.try_value()));
            }
        }
        if (!result)
        {
            set_error(*result.try_error(), "Build・Package・Run操作");
            return false;
        }
        refresh();
        switch (a_command)
        {
        case EditorPackageCommand::Start:
            set_status("BuildからStandalone Package公開までを開始しました。");
            break;
        case EditorPackageCommand::Cancel:
            set_status("進行中Workflowへキャンセルを要求しました。");
            break;
        case EditorPackageCommand::Retry:
            set_status("BuildからStandalone Package公開までを再実行しました。");
            break;
        case EditorPackageCommand::Run:
            set_status("Standalone Runtimeを起動しました。");
            break;
        case EditorPackageCommand::Stop:
            set_status("Standalone Runtimeへ停止を要求しました。");
            break;
        }
        return true;
    }
    catch (...)
    {
        terminate_exception();
    }
}

void PackagePresenter::set_active_document(std::optional<editor_core::EditorDocumentId> a_documentId) noexcept
{
    m_activeDocumentId = a_documentId;
}

bool PackagePresenter::set_configuration(BuildConfiguration a_configuration) noexcept
{
    if (!can_start())
    {
        return false;
    }
    m_configuration = a_configuration;
    return true;
}

bool PackagePresenter::set_force_configure(bool a_forceConfigure) noexcept
{
    if (!can_start())
    {
        return false;
    }
    m_forceConfigure = a_forceConfigure;
    return true;
}

bool PackagePresenter::begin_editor_shutdown() noexcept
{
    refresh();
    if (!is_active(m_current.state))
    {
        return true;
    }
    m_openShutdownConfirmation = true;
    m_isShutdownConfirmationPending = true;
    return false;
}

bool PackagePresenter::respond_to_editor_shutdown(EditorPackageShutdownDecision a_decision) noexcept
{
    try
    {
        if (!m_isShutdownConfirmationPending)
        {
            return !is_active(m_current.state);
        }
        if (a_decision == EditorPackageShutdownDecision::KeepEditorOpen)
        {
            m_closeShutdownConfirmation = true;
            m_isShutdownConfirmationPending = false;
            m_isShutdownWaitingForCancel = false;
            return false;
        }
        Result<void> cancelled = m_service->request_cancel();
        if (!cancelled)
        {
            refresh();
            if (!is_active(m_current.state))
            {
                mark_shutdown_ready();
                return true;
            }
            set_error(*cancelled.try_error(), "終了前のWorkflow停止");
            return false;
        }
        m_isShutdownWaitingForCancel = true;
        refresh();
        return !m_isShutdownWaitingForCancel;
    }
    catch (...)
    {
        terminate_exception();
    }
}

bool PackagePresenter::take_shutdown_ready() noexcept
{
    const bool ready = m_isShutdownReady;
    m_isShutdownReady = false;
    return ready;
}

const package::PackageWorkflowSnapshot &PackagePresenter::current_snapshot() const noexcept
{
    return m_current;
}

std::string_view PackagePresenter::message() const noexcept
{
    return m_message;
}

bool PackagePresenter::has_error_message() const noexcept
{
    return m_hasError;
}

bool PackagePresenter::can_start() const noexcept
{
    return m_activeDocumentId.has_value() && !is_active(m_current.state);
}

bool PackagePresenter::can_cancel() const noexcept
{
    return m_current.state == package::PackageWorkflowState::Building ||
           m_current.state == package::PackageWorkflowState::Packaging;
}

bool PackagePresenter::can_retry() const noexcept
{
    return !is_active(m_current.state) && !m_current.build.operationId.empty();
}

bool PackagePresenter::can_run() const noexcept
{
    return m_current.package.has_value() &&
           (m_current.state == package::PackageWorkflowState::PackageReady ||
            m_current.state == package::PackageWorkflowState::RunSucceeded);
}

bool PackagePresenter::can_stop() const noexcept
{
    return m_current.state == package::PackageWorkflowState::Running;
}

bool PackagePresenter::is_shutdown_confirmation_pending() const noexcept
{
    return m_isShutdownConfirmationPending || m_isShutdownWaitingForCancel;
}

void PackagePresenter::set_error(const Error &a_error, std::string_view a_operation)
{
    m_message.assign(a_operation);
    m_message.append("に失敗しました: ");
    m_message.append(a_error.root_code().domain());
    m_message.push_back('/');
    m_message.append(std::to_string(a_error.root_code().value()));
    m_message.push_back(' ');
    m_message.append(a_error.summary());
    m_hasError = true;
}

void PackagePresenter::set_status(std::string_view a_status)
{
    m_message.assign(a_status);
    m_hasError = false;
}

void PackagePresenter::draw_toolbar() noexcept
{
    const bool enabled = can_start();
    if (!enabled)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::BeginCombo("Configuration", configuration_label(m_configuration)))
    {
        constexpr BuildConfiguration configurations[] = {BuildConfiguration::Debug,
                                                          BuildConfiguration::Development,
                                                          BuildConfiguration::Release};
        for (const BuildConfiguration configuration : configurations)
        {
            const bool selected = configuration == m_configuration;
            if (ImGui::Selectable(configuration_label(configuration), selected))
            {
                static_cast<void>(set_configuration(configuration));
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Force Configure", &m_forceConfigure);
    if (ImGui::Button("Build & Package"))
    {
        static_cast<void>(submit(EditorPackageCommand::Start));
    }
    if (!enabled)
    {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!can_cancel())
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Cancel"))
    {
        static_cast<void>(submit(EditorPackageCommand::Cancel));
    }
    if (!can_cancel())
    {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!can_retry())
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Retry"))
    {
        static_cast<void>(submit(EditorPackageCommand::Retry));
    }
    if (!can_retry())
    {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!can_run())
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Run"))
    {
        static_cast<void>(submit(EditorPackageCommand::Run));
    }
    if (!can_run())
    {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!can_stop())
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Stop"))
    {
        static_cast<void>(submit(EditorPackageCommand::Stop));
    }
    if (!can_stop())
    {
        ImGui::EndDisabled();
    }
}

void PackagePresenter::draw_result() noexcept
{
    ImGui::Text("State: %s", state_label(m_current.state));
    ImGui::Text("Stage: %s", stage_label(m_current.activeStage));
    if (!m_current.build.operationId.empty())
    {
        ImGui::Text("Operation: %s", m_current.build.operationId.c_str());
    }
    const std::optional<package::PublishedRuntimePackageSnapshot> &shown =
        m_current.package ? m_current.package : m_current.latestSuccessfulPackage;
    if (shown)
    {
        ImGui::SeparatorText("Published Package");
        ImGui::Text("Artifact: %s", shown->artifactId.c_str());
        ImGui::Text("Project: %s", shown->manifest.projectId.c_str());
        ImGui::Text("Configuration: %s", configuration_label(shown->manifest.configuration));
        ImGui::Text("Files: %zu", shown->manifest.fileCount);
        ImGui::Text("Bytes: %llu", static_cast<unsigned long long>(shown->manifest.inventoryBytes));
        ImGui::TextWrapped("Output: %s", shown->destination.c_str());
        ImGui::TextWrapped("Run target: %s", shown->executable.c_str());
    }
    if (!m_current.runOutput.empty())
    {
        ImGui::SeparatorText("Runtime Output");
        for (const ChildProcessOutputChunk &chunk : m_current.runOutput)
        {
            ImGui::TextUnformatted(chunk.bytes.data(), chunk.bytes.data() + chunk.bytes.size());
        }
    }
}

void PackagePresenter::draw_shutdown_confirmation() noexcept
{
    if (m_openShutdownConfirmation)
    {
        ImGui::OpenPopup("Build・Package・Runを停止して終了しますか？");
        m_openShutdownConfirmation = false;
    }
    if (m_closeShutdownConfirmation)
    {
        ImGui::CloseCurrentPopup();
        m_closeShutdownConfirmation = false;
    }
    if (ImGui::BeginPopupModal("Build・Package・Runを停止して終了しますか？", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(m_isShutdownWaitingForCancel ? "停止完了を待っています。"
                                                            : "実行中の処理またはRuntimeを停止します。");
        if (m_isShutdownWaitingForCancel)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("停止して終了"))
        {
            static_cast<void>(respond_to_editor_shutdown(EditorPackageShutdownDecision::StopAndClose));
        }
        ImGui::SameLine();
        if (ImGui::Button("Editorへ戻る"))
        {
            static_cast<void>(respond_to_editor_shutdown(EditorPackageShutdownDecision::KeepEditorOpen));
        }
        if (m_isShutdownWaitingForCancel)
        {
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
}

void PackagePresenter::mark_shutdown_ready() noexcept
{
    m_closeShutdownConfirmation = true;
    m_isShutdownConfirmationPending = false;
    m_isShutdownWaitingForCancel = false;
    m_isShutdownReady = true;
}

[[noreturn]] void PackagePresenter::terminate_exception() const noexcept
{
    m_assertContext->fatal_handler().terminate("Package Presenter failed unexpectedly");
    std::abort();
}
} // namespace cue::editor
