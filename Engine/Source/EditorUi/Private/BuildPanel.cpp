#include "EditorUiPanels.h"

#include <array>
#include <string>
#include <utility>

namespace cue
{
namespace
{
/// @brief 選択項目の所有値確保が失敗しても Combo の Stack を対応させる
class ComboScope final
{
  public:
    /// @brief BeginCombo が成功した Scope だけを受け持つ
    ComboScope() = default;
    /// @brief 開いた Combo を閉じる
    ~ComboScope()
    {
        ImGui::EndCombo();
    }
};

/// @brief 登録された Platform / Architecture の組合せだけを入力候補として扱う
const GameBuildPlatform *selected_platform(const EditorBuildContext &a_build)
{
    const auto found = std::find_if(a_build.platforms.begin(), a_build.platforms.end(), [&](const auto &a_platform)
                                    { return a_platform.id == a_build.selectedTarget.platform; });
    return found == a_build.platforms.end() ? nullptr : &*found;
}

/// @brief Target の候補を値で返す。Host が採用するまで表示 Context を書き換えない
void request_target(EditorUiState &a_state, std::vector<EditorAction> &a_actions, GameBuildTarget a_target)
{
    finish_transform_input(a_state, a_actions);
    EditorAction action;
    action.kind = EditorActionKind::SetGameBuildTarget;
    action.buildTarget = std::move(a_target);
    a_actions.push_back(std::move(action));
}

/// @brief 登録一覧をそのまま使い、未対応 Target を UI が暗黙に選択しない
void draw_target_selection(const EditorBuildContext &a_build, EditorUiState &a_state,
                           std::vector<EditorAction> &a_actions)
{
    const auto *platform = selected_platform(a_build);
    ImGui::SetNextItemWidth(-120);
    if (ImGui::BeginCombo("Platform", platform ? a_build.selectedTarget.platform.c_str() : "Select platform"))
    {
        ComboScope combo;
        for (const auto &entry : a_build.platforms)
        {
            const std::string label(entry.displayName.empty() ? entry.id : entry.displayName);
            if (ImGui::Selectable(label.c_str(), entry.id == a_build.selectedTarget.platform) &&
                entry.id != a_build.selectedTarget.platform)
            {
                auto target = a_build.selectedTarget;
                target.platform = entry.id;
                // 新しい Platform で Architecture が未対応なら明示的な再選択を求める
                if (std::find(entry.architectures.begin(), entry.architectures.end(), target.architecture) ==
                    entry.architectures.end())
                    target.architecture.clear();
                request_target(a_state, a_actions, std::move(target));
            }
        }
    }
    ImGui::SetNextItemWidth(-120);
    if (ImGui::BeginCombo("Architecture", a_build.selectedTarget.architecture.empty()
                                              ? "Select architecture"
                                              : a_build.selectedTarget.architecture.c_str()))
    {
        ComboScope combo;
        if (platform)
            for (const auto architecture : platform->architectures)
            {
                const std::string label(architecture);
                if (ImGui::Selectable(label.c_str(), architecture == a_build.selectedTarget.architecture) &&
                    architecture != a_build.selectedTarget.architecture)
                {
                    auto target = a_build.selectedTarget;
                    target.architecture = architecture;
                    request_target(a_state, a_actions, std::move(target));
                }
            }
    }
    const auto configuration = game_build_configuration_name(a_build.selectedTarget.configuration);
    ImGui::SetNextItemWidth(-120);
    if (ImGui::BeginCombo("Configuration", configuration.empty() ? "Select configuration" : configuration.data()))
    {
        ComboScope combo;
        constexpr std::array configurations{GameBuildConfiguration::Debug, GameBuildConfiguration::Development,
                                            GameBuildConfiguration::Release};
        for (const auto value : configurations)
            if (ImGui::Selectable(game_build_configuration_name(value).data(),
                                  value == a_build.selectedTarget.configuration) &&
                value != a_build.selectedTarget.configuration)
            {
                auto target = a_build.selectedTarget;
                target.configuration = value;
                request_target(a_state, a_actions, std::move(target));
            }
    }
}

/// @brief Host の借用文字列を Format String として評価せず、その長さだけ表示する
void draw_value(const char *a_label, std::string_view a_value)
{
    ImGui::TextUnformatted(a_label);
    ImGui::SameLine();
    if (a_value.empty())
        ImGui::TextUnformatted("--");
    else
        ImGui::TextUnformatted(a_value.data(), a_value.data() + a_value.size());
}
} // namespace

bool has_game_build_target(const EditorBuildContext &a_build) noexcept
{
    const auto *platform = selected_platform(a_build);
    return platform &&
           std::find(platform->architectures.begin(), platform->architectures.end(),
                     a_build.selectedTarget.architecture) != platform->architectures.end() &&
           !game_build_configuration_name(a_build.selectedTarget.configuration).empty();
}

void draw_build_panel(const EditorUiContext &a_context, EditorUiState &a_state, std::vector<EditorAction> &a_actions)
{
    const auto &build = a_context.build;
    if (!a_state.isBuildPanelVisible || (build.platforms.empty() && build.statusText.empty() && build.logText.empty()))
        return;
    const auto *viewport = ImGui::GetMainViewport();
    const float width = (std::max)(a_context.displayWidth, 1.0f);
    const float left = (std::min)(240.0f, width * 0.22f);
    const float right = (std::min)(300.0f, width * 0.27f);
    set_initial_panel(viewport->Pos.x + left, editor_panel_top() + 40, (std::max)(width - left - right, 320.0f), 430);
    PanelScope panel("Build##EditorBuild");
    if (!panel.is_visible())
        return;
    const bool isBusy = build.isBuilding || build.isApplicationRunning || a_context.isPlaying || a_context.isPlayBusy;
    {
        EditorDisabledScope disabled(isBusy);
        draw_target_selection(build, a_state, a_actions);
    }
    ImGui::Separator();
    const auto request = [&](EditorActionKind a_kind)
    {
        finish_transform_input(a_state, a_actions);
        emit_editor_action(a_actions, a_kind);
    };
    {
        EditorDisabledScope disabled(!build.canBuild || isBusy || !has_game_build_target(build));
        if (ImGui::Button("Build", {100, 0}))
            request(EditorActionKind::BuildGame);
    }
    ImGui::SameLine();
    {
        EditorDisabledScope disabled(!build.canCancel || !build.isBuilding);
        if (ImGui::Button("Cancel Build", {110, 0}))
            request(EditorActionKind::CancelGameBuild);
    }
    ImGui::SameLine();
    {
        EditorDisabledScope disabled(!build.canRun || isBusy);
        if (ImGui::Button("Run", {100, 0}))
            request(EditorActionKind::RunGame);
    }
    if (build.isApplicationRunning)
        ImGui::TextUnformatted("Application running. Close it before building again.");
    draw_value("State:", build.statusText);
    if (build.exitCode)
        ImGui::Text("Build exit code: %u", static_cast<unsigned>(*build.exitCode));
    draw_value("Startup scene:", build.startupScene);
    draw_value("Output:", build.outputDirectory);
    ImGui::TextWrapped("The project startup scene is packaged from saved files.");
    if (ImGui::CollapsingHeader("Build Log"))
    {
        if (build.isLogTruncated)
            ImGui::TextUnformatted("Older output was omitted to keep the log bounded.");
        // 折り畳み中は巨大 Log を走査せず、展開中も Host の所有 Buffer を複写しない
        const bool isVisible =
            ImGui::BeginChild("BuildLogText", {0, 160}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        struct ChildScope final
        {
            /// @brief Log 表示の例外時も Child Stack を閉じる
            ~ChildScope()
            {
                ImGui::EndChild();
            }
        } child;
        if (isVisible && !build.logText.empty())
            ImGui::TextUnformatted(build.logText.data(), build.logText.data() + build.logText.size());
    }
}
} // namespace cue
