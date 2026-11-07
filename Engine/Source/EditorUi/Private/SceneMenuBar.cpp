#include "EditorUiPanels.h"

namespace cue
{
namespace
{
/// @brief 例外時にも開いた Menu の Stack を閉じる
class MenuScope final
{
  public:
    /// @brief BeginMenu 成功後にのみ生成する
    MenuScope() = default;
    /// @brief 開いている Menu を閉じる
    ~MenuScope()
    {
        ImGui::EndMenu();
    }
};
/// @brief Main Menu Bar の Stack を失敗経路でも閉じる
class MenuBarScope final
{
  public:
    /// @brief BeginMainMenuBar 成功後にのみ生成する
    MenuBarScope() = default;
    /// @brief 開いている Menu Bar を閉じる
    ~MenuBarScope()
    {
        ImGui::EndMainMenuBar();
    }
};
} // namespace

void draw_scene_menu(const SceneDocument *a_document, EditorUiState &a_state, std::vector<EditorAction> &a_actions,
                     const EditorUiContext &a_context, bool a_isInputEnabled)
{
    const bool canOpenProject = a_context.canOpenProject && !a_context.isPlaying && !a_context.isPlayBusy &&
                                !a_context.build.isBuilding && !a_context.build.isApplicationRunning &&
                                !a_context.script.isBusy;
    const auto request = [&](EditorActionKind a_kind)
    {
        finish_transform_input(a_state, a_actions);
        emit_editor_action(a_actions, a_kind);
    };
    if (ImGui::BeginMainMenuBar())
    {
        MenuBarScope menuBar;
        if (ImGui::BeginMenu("File", !a_context.isPlaying))
        {
            MenuScope menu;
            if (ImGui::MenuItem("Open Project...", "Ctrl+Shift+O", false, canOpenProject))
                request(EditorActionKind::OpenProject);
            ImGui::Separator();
            const bool canEditScene = a_document && a_context.isProjectOpen && !a_context.isPlaying;
            if (ImGui::MenuItem("New Scene", "Ctrl+N", false, canEditScene))
                request(EditorActionKind::NewScene);
            if (ImGui::MenuItem("Open Scene...", "Ctrl+O", false, canEditScene))
                request(EditorActionKind::OpenScene);
            ImGui::Separator();
            if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, canEditScene && !a_context.build.isBuilding))
                request(EditorActionKind::SaveScene);
            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, canEditScene && !a_context.build.isBuilding))
                request(EditorActionKind::SaveSceneAs);
            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
                request(EditorActionKind::Exit);
        }
        if (ImGui::BeginMenu("Edit", a_document && a_context.isProjectOpen && !a_context.isPlaying))
        {
            MenuScope menu;
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, a_document->can_undo()))
                request(EditorActionKind::Undo);
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, a_document->can_redo()))
                request(EditorActionKind::Redo);
        }
        if (ImGui::BeginMenu("View"))
        {
            MenuScope menu;
            if (ImGui::MenuItem("Diagnostics"))
                request(EditorActionKind::ToggleDiagnostics);
        }
        if (ImGui::BeginMenu("Script", a_context.isProjectOpen))
        {
            MenuScope menu;
            if (ImGui::MenuItem("Open Visual Studio", nullptr, false, a_context.script.canOpenIde))
                request(EditorActionKind::OpenIde);
            if (ImGui::MenuItem("Build Scripts", nullptr, false, a_context.script.canBuild))
                request(EditorActionKind::BuildScripts);
            if (ImGui::MenuItem("Cancel Script Operation", nullptr, false, a_context.script.canCancel))
                request(EditorActionKind::CancelScriptOperation);
            ImGui::Separator();
            if (ImGui::MenuItem("Reload Script Types", nullptr, false,
                                a_context.canPlay && !a_context.isPlayBusy && !a_context.isPlaying &&
                                    !a_context.script.isBusy))
                request(EditorActionKind::ReloadScripts);
            if (!a_context.script.statusText.empty())
            {
                ImGui::Separator();
                ImGui::TextUnformatted(a_context.script.statusText.data(),
                                       a_context.script.statusText.data() + a_context.script.statusText.size());
            }
        }
        if (ImGui::BeginMenu("Build", a_context.isProjectOpen &&
                                          (!a_context.build.platforms.empty() || !a_context.build.statusText.empty() ||
                                           !a_context.build.logText.empty())))
        {
            MenuScope menu;
            if (ImGui::MenuItem("Build Settings..."))
                a_state.isBuildPanelVisible = true;
            if (ImGui::MenuItem("Show Build Panel", nullptr, a_state.isBuildPanelVisible))
                a_state.isBuildPanelVisible = !a_state.isBuildPanelVisible;
            ImGui::Separator();
            const auto &build = a_context.build;
            const bool isBusy =
                build.isBuilding || build.isApplicationRunning || a_context.isPlaying || a_context.isPlayBusy;
            if (ImGui::MenuItem("Build Game", nullptr, false,
                                build.canBuild && !isBusy && has_game_build_target(build)))
                request(EditorActionKind::BuildGame);
            if (ImGui::MenuItem("Cancel Build", nullptr, false, build.isBuilding && build.canCancel))
                request(EditorActionKind::CancelGameBuild);
            if (ImGui::MenuItem("Run Game", nullptr, false, build.canRun && !isBusy))
                request(EditorActionKind::RunGame);
        }
        ImGui::Separator();
        if (a_context.isPlaying)
        {
            if (ImGui::MenuItem("Stop", nullptr, false, a_context.canStop))
                request(EditorActionKind::Stop);
        }
        else if (ImGui::MenuItem("Play", nullptr, false,
                                 a_context.isProjectOpen && a_context.canPlay && !a_context.isPlayBusy))
            request(EditorActionKind::Play);
        if (ImGui::MenuItem("Debug Play", nullptr, false, a_context.isProjectOpen && a_context.script.canDebugPlay))
            request(EditorActionKind::DebugPlay);
        if (a_context.script.isWaitingForDebugger)
            ImGui::Text("Debugger wait (PID %u)", static_cast<unsigned>(a_context.script.processId));
        if (a_context.isPlayBusy)
            ImGui::TextUnformatted("Waiting for Play process...");
        ImGui::Separator();
        ImGui::TextUnformatted(a_document && a_context.isProjectOpen ? a_document->scene().descriptor().name.c_str()
                                                                     : "No project open");
        if (a_document && a_context.isProjectOpen && a_document->is_dirty())
        {
            ImGui::SameLine(0, 4);
            ImGui::TextUnformatted("*");
        }
    }
    // Text 入力中の Ctrl+Z 等を Scene の履歴として奪わない
    if (!a_isInputEnabled || a_context.isPlaying)
        return;
    const auto &io = ImGui::GetIO();
    if (!io.KeyCtrl)
        return;
    if (io.KeyShift && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_O, false))
    {
        if (canOpenProject)
            request(EditorActionKind::OpenProject);
        return;
    }
    if (!a_document || !a_context.isProjectOpen)
        return;
    if (!a_context.build.isBuilding && ImGui::IsKeyPressed(ImGuiKey_S, false))
        request(io.KeyShift ? EditorActionKind::SaveSceneAs : EditorActionKind::SaveScene);
    if (io.WantTextInput)
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_N, false))
        request(EditorActionKind::NewScene);
    if (ImGui::IsKeyPressed(ImGuiKey_O, false))
        request(EditorActionKind::OpenScene);
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && a_document->can_undo())
        request(EditorActionKind::Undo);
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false) && a_document->can_redo())
        request(EditorActionKind::Redo);
}
} // namespace cue
