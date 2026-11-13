#include "EditorUiPanels.h"

#include <array>
#include <utility>

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

/// @brief 選択要求の所有値確保に失敗しても、開いた Combo の Stack を戻す
class ConfigurationComboScope final
{
  public:
    /// @brief BeginCombo 成功後の Scope だけを受け持つ
    ConfigurationComboScope() = default;
    /// @brief 構成選択の Popup を閉じる
    ~ConfigurationComboScope()
    {
        ImGui::EndCombo();
    }
};

/// @brief Host が所有する Game 構成を表示し、変更を所有 Action だけで要求する
void draw_game_configuration(const EditorUiContext &a_context, bool a_isInputEnabled, EditorUiState &a_state,
                             std::vector<EditorAction> &a_actions)
{
    const bool canSelect = a_isInputEnabled && a_context.isProjectOpen && a_context.script.canSelectConfiguration &&
                           !a_context.isPlaying && !a_context.isPlayBusy && !a_context.script.isBusy &&
                           !a_context.build.isBuilding && !a_context.build.isApplicationRunning;
    EditorDisabledScope disabled(!canSelect);
    ImGui::TextUnformatted(a_context.text("Game:", "ゲーム構成"));
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Development").x + ImGui::GetFrameHeight() +
                            ImGui::GetStyle().FramePadding.x * 2);
    const auto current = game_build_configuration_name(a_context.script.configuration);
    if (!ImGui::BeginCombo("##GameConfiguration",
                           current.empty() ? a_context.text("Select configuration", "構成を選択") : current.data()))
        return;
    ConfigurationComboScope combo;
    constexpr std::array configurations{GameBuildConfiguration::Debug, GameBuildConfiguration::Development,
                                        GameBuildConfiguration::Release};
    for (const auto configuration : configurations)
    {
        const auto label = game_build_configuration_name(configuration);
        const bool isSelected = configuration == a_context.script.configuration;
        if (ImGui::Selectable(label.data(), isSelected) && !isSelected)
        {
            finish_transform_input(a_state, a_actions);
            EditorAction action;
            action.kind = EditorActionKind::SetGameConfiguration;
            action.gameConfiguration = configuration;
            a_actions.push_back(std::move(action));
        }
        if (isSelected)
            ImGui::SetItemDefaultFocus();
    }
}
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
        if (ImGui::BeginMenu(a_context.text("File", "ファイル###File"), !a_context.isPlaying))
        {
            MenuScope menu;
            if (ImGui::MenuItem(a_context.text("Open Project...", "プロジェクトを開く...###Open Project..."),
                                "Ctrl+Shift+O", false, canOpenProject))
                request(EditorActionKind::OpenProject);
            ImGui::Separator();
            const bool canEditScene = a_document && a_context.isProjectOpen && !a_context.isPlaying;
            if (ImGui::MenuItem(a_context.text("New Scene", "新規シーン###New Scene"), "Ctrl+N", false, canEditScene))
                request(EditorActionKind::NewScene);
            if (ImGui::MenuItem(a_context.text("Open Scene...", "シーンを開く...###Open Scene..."), "Ctrl+O", false,
                                canEditScene))
                request(EditorActionKind::OpenScene);
            ImGui::Separator();
            if (ImGui::MenuItem(a_context.text("Save Scene", "シーンを保存###Save Scene"), "Ctrl+S", false,
                                canEditScene && !a_context.build.isBuilding))
                request(EditorActionKind::SaveScene);
            if (ImGui::MenuItem(a_context.text("Save Scene As...", "名前を付けてシーンを保存...###Save Scene As..."),
                                "Ctrl+Shift+S", false, canEditScene && !a_context.build.isBuilding))
                request(EditorActionKind::SaveSceneAs);
            ImGui::Separator();
            if (ImGui::MenuItem(a_context.text("Exit", "終了###Exit")))
                request(EditorActionKind::Exit);
        }
        if (ImGui::BeginMenu(a_context.text("Edit", "編集###Edit"),
                             a_document && a_context.isProjectOpen && !a_context.isPlaying))
        {
            MenuScope menu;
            if (ImGui::MenuItem(a_context.text("Undo", "元に戻す###Undo"), "Ctrl+Z", false, a_document->can_undo()))
                request(EditorActionKind::Undo);
            if (ImGui::MenuItem(a_context.text("Redo", "やり直す###Redo"), "Ctrl+Y", false, a_document->can_redo()))
                request(EditorActionKind::Redo);
            ImGui::Separator();
            if (ImGui::MenuItem(a_context.text("Add Cube", "キューブを追加###Add Cube"), nullptr, false,
                                a_isInputEnabled))
                request(EditorActionKind::AddCube);
            if (ImGui::MenuItem(a_context.text("New Script...", "スクリプトを作成...###New Script..."), nullptr, false,
                                a_isInputEnabled && can_create_editor_script(a_context)))
            {
                finish_transform_input(a_state, a_actions);
                a_state.scriptName.fill(0);
                a_state.isScriptCreationRequested = true;
                a_state.shouldOpenScriptCreationPopup = true;
                a_state.needsScriptNameFocus = true;
            }
        }
        if (ImGui::BeginMenu(a_context.text("View", "表示###View")))
        {
            MenuScope menu;
            if (ImGui::MenuItem(a_context.text("Diagnostics", "診断###Diagnostics")))
                request(EditorActionKind::ToggleDiagnostics);
            if (ImGui::BeginMenu(a_context.text("Language", "言語###Language"), a_isInputEnabled))
            {
                MenuScope languageMenu;
                // 言語名は各言語の表記で固定し、切り替え先を選び直せるようにする
                constexpr std::array languages{EditorLanguage::English, EditorLanguage::Japanese};
                for (const auto language : languages)
                {
                    const bool isSelected = language == a_context.language;
                    if (ImGui::MenuItem(editor_text(language, "English", "日本語"), nullptr, isSelected) && !isSelected)
                    {
                        finish_transform_input(a_state, a_actions);
                        EditorAction action;
                        action.kind = EditorActionKind::SetLanguage;
                        action.language = language;
                        a_actions.push_back(std::move(action));
                    }
                }
            }
        }
        if (ImGui::BeginMenu(a_context.text("Script", "スクリプト###Script"), a_context.isProjectOpen))
        {
            MenuScope menu;
            if (ImGui::MenuItem(a_context.text("Open Visual Studio", "Visual Studio で開く###Open Visual Studio"),
                                nullptr, false, a_context.script.canOpenIde))
                request(EditorActionKind::OpenIde);
            if (ImGui::MenuItem(a_context.text("Build Scripts", "スクリプトをビルド###Build Scripts"), nullptr, false,
                                a_context.script.canBuild))
                request(EditorActionKind::BuildScripts);
            if (ImGui::MenuItem(
                    a_context.text("Cancel Script Operation", "スクリプト操作を中止###Cancel Script Operation"),
                    nullptr, false, a_context.script.canCancel))
                request(EditorActionKind::CancelScriptOperation);
            ImGui::Separator();
            if (ImGui::MenuItem(a_context.text("Reload Script Types", "スクリプト型を再読み込み###Reload Script Types"),
                                nullptr, false,
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
        if (ImGui::BeginMenu(a_context.text("Build", "ビルド###Build"),
                             a_context.isProjectOpen &&
                                 (!a_context.build.platforms.empty() || !a_context.build.statusText.empty() ||
                                  !a_context.build.logText.empty())))
        {
            MenuScope menu;
            if (ImGui::MenuItem(a_context.text("Build Settings...", "ビルド設定...###Build Settings...")))
                a_state.isBuildPanelVisible = true;
            if (ImGui::MenuItem(a_context.text("Show Build Panel", "ビルドパネルを表示###Show Build Panel"), nullptr,
                                a_state.isBuildPanelVisible))
                a_state.isBuildPanelVisible = !a_state.isBuildPanelVisible;
            ImGui::Separator();
            const auto &build = a_context.build;
            const bool isBusy =
                build.isBuilding || build.isApplicationRunning || a_context.isPlaying || a_context.isPlayBusy;
            if (ImGui::MenuItem(a_context.text("Build Game", "ゲームをビルド###Build Game"), nullptr, false,
                                build.canBuild && !isBusy && has_game_build_target(build)))
                request(EditorActionKind::BuildGame);
            if (ImGui::MenuItem(a_context.text("Cancel Build", "ビルドを中止###Cancel Build"), nullptr, false,
                                build.isBuilding && build.canCancel))
                request(EditorActionKind::CancelGameBuild);
            if (ImGui::MenuItem(a_context.text("Run Game", "ゲームを起動###Run Game"), nullptr, false,
                                build.canRun && !isBusy))
                request(EditorActionKind::RunGame);
        }
        ImGui::Separator();
        if (a_context.isPlaying)
        {
            if (ImGui::MenuItem(a_context.text("Stop", "停止###Stop"), nullptr, false, a_context.canStop))
                request(EditorActionKind::Stop);
        }
        else if (ImGui::MenuItem(a_context.text("Play", "再生###Play"), nullptr, false,
                                 a_context.isProjectOpen && a_context.canPlay && !a_context.isPlayBusy))
            request(EditorActionKind::Play);
        if (ImGui::MenuItem(a_context.text("Debug Play", "デバッグ再生###Debug Play"), nullptr, false,
                            a_context.isProjectOpen && a_context.script.canDebugPlay))
            request(EditorActionKind::DebugPlay);
        draw_game_configuration(a_context, a_isInputEnabled, a_state, a_actions);
        if (a_context.isProjectOpen && a_context.script.isBuildRequired)
        {
            ImGui::TextDisabled(a_context.text("Build required", "ビルドが必要です"));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(
                    a_context.text("Use Script > Build Scripts to build the selected Game configuration.",
                                   "「スクリプト」→「スクリプトをビルド」で、選択したゲーム構成をビルドできます。"));
        }
        if (a_context.script.isWaitingForDebugger)
            ImGui::Text(a_context.text("Debugger wait (PID %u)", "デバッガーの接続待ち（PID %u）"),
                        static_cast<unsigned>(a_context.script.processId));
        if (a_context.isPlayBusy)
            ImGui::TextUnformatted(a_context.text("Waiting for Play process...", "再生プロセスを待機中..."));
        ImGui::Separator();
        ImGui::TextUnformatted(a_document && a_context.isProjectOpen
                                   ? a_document->scene().descriptor().name.c_str()
                                   : a_context.text("No project open", "プロジェクト未選択"));
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
