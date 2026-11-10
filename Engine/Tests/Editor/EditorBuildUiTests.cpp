#include <EditorUi/EditorUi.h>

#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>

#include <Editor/EditorApplication.h>

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Editor Build UI failed at %d: %s\n", __LINE__, #__VA_ARGS__);                        \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Native Process を起動せず、実 ImGui Widget の結果だけを所有 Action へ変換する
cue::Result<std::vector<cue::EditorAction>> frame(cue::EditorUi &a_ui, const cue::SceneDocument &a_document,
                                                  const cue::EditorUiContext &a_context)
{
    ImGui::NewFrame();
    auto actions = a_ui.draw(a_document, a_context);
    ImGui::Render();
    return actions;
}

/// @brief Widget 自身の ID で Navigation Activate を送り、同じ公開 draw 経路で応答を確認する
cue::Result<std::vector<cue::EditorAction>> activate(cue::EditorUi &a_ui, const cue::SceneDocument &a_document,
                                                     const cue::EditorUiContext &a_context, ImGuiWindow &a_window,
                                                     const char *a_label)
{
    ImGui::ActivateItemByID(ImHashStr(a_label, 0, a_window.ID));
    return frame(a_ui, a_document, a_context);
}

/// @brief UI が返した単一の要求を、Host へ渡す前に確認する
bool is_action(const cue::Result<std::vector<cue::EditorAction>> &a_actions, cue::EditorActionKind a_kind)
{
    return a_actions.has_value() && a_actions.try_value()->size() == 1 && a_actions.try_value()->front().kind == a_kind;
}
} // namespace

/// @brief 対象設定・実行状態・保存確認が正本や Process を直接操作しないことを確認する
int main()
{
    cue::EditorApplicationConfig config;
    unsigned id = 0;
    config.generateId = [&]()
    {
        char text[37]{};
        std::snprintf(text, sizeof(text), "10000000-0000-4000-8000-%012u", ++id);
        return cue::Result<std::string>::success(text);
    };
    auto created = cue::EditorApplication::create(std::nullopt, std::move(config));
    CHECK(created.has_value());
    auto app = created.take_value();
    CHECK(app->add_cube().has_value());
    CHECK(app->document().is_dirty());
    auto original = app->document().scene().serialize();
    CHECK(original.has_value());
    auto createdUi = cue::EditorUi::create();
    CHECK(createdUi.has_value());
    auto ui = createdUi.take_value();

    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(),
                                                                            &ImGui::DestroyContext);
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = {1280, 720};
    io.DeltaTime = 1.0f / 60.0f;
    CHECK(io.Fonts->Build());
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    cue::EditorUiContext display;
    display.build.platforms = cue::supported_game_build_platforms();
    display.build.selectedTarget = {"Windows", "x64", cue::GameBuildConfiguration::Debug};
    display.build.canBuild = true;
    display.build.statusText = "Ready";
    display.build.startupScene = "Content/Scenes/RotateCube.cuescene";
    CHECK(frame(*ui, app->document(), display).has_value());
    CHECK(frame(*ui, app->document(), display).has_value());
    // 通常の Scene 表示を覆わず、Build Menu から設定 Panel を明示的に開く
    const auto *hiddenPanel = ImGui::FindWindowByName("Build##EditorBuild");
    CHECK(!hiddenPanel || !hiddenPanel->Active);
    auto *menuBar = ImGui::FindWindowByName("##MainMenuBar");
    CHECK(menuBar && menuBar->Active);
    // Menu の内部 ID 構成へ依存せず、表示ラベルの幅と Style から実入力位置を求める
    float buildX = menuBar->Pos.x + ImGui::GetStyle().FramePadding.x;
    for (const char *label : {"File", "Edit", "View", "Script"})
        buildX += ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemSpacing.x * 2;
    buildX += ImGui::CalcTextSize("Build").x * 0.5f + ImGui::GetStyle().ItemSpacing.x;
    io.AddMousePosEvent(buildX, menuBar->Pos.y + menuBar->Size.y * 0.5f);
    CHECK(frame(*ui, app->document(), display).has_value());
    io.AddMouseButtonEvent(0, true);
    CHECK(frame(*ui, app->document(), display).has_value());
    io.AddMouseButtonEvent(0, false);
    CHECK(frame(*ui, app->document(), display).has_value());
    const auto &popupStack = ImGui::GetCurrentContext()->OpenPopupStack;
    auto *menu = popupStack.empty() ? nullptr : popupStack.back().Window;
    CHECK(menu && menu->Active && (menu->Flags & ImGuiWindowFlags_ChildMenu));
    io.AddMousePosEvent(menu->DC.CursorStartPos.x + ImGui::CalcTextSize("Build Settings...").x * 0.5f,
                        menu->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * 0.5f);
    CHECK(frame(*ui, app->document(), display).has_value());
    io.AddMouseButtonEvent(0, true);
    CHECK(frame(*ui, app->document(), display).has_value());
    io.AddMouseButtonEvent(0, false);
    CHECK(frame(*ui, app->document(), display).has_value());
    CHECK(frame(*ui, app->document(), display).has_value());
    auto *panel = ImGui::FindWindowByName("Build##EditorBuild");
    CHECK(panel && panel->Active);
    auto build = activate(*ui, app->document(), display, *panel, "Build");
    CHECK(is_action(build, cue::EditorActionKind::BuildGame));
    CHECK(!display.build.isBuilding);
    auto runDisabled = activate(*ui, app->document(), display, *panel, "Run");
    CHECK(runDisabled.has_value() && runDisabled.try_value()->empty());

    // 設定を選んでも借用 Context を変更せず、所有 Target の Action だけを返す
    CHECK(activate(*ui, app->document(), display, *panel, "Configuration").has_value());
    auto *combo = ImGui::FindWindowByName("##Combo_00");
    CHECK(combo && combo->Active);
    auto selected = activate(*ui, app->document(), display, *combo, "Release");
    CHECK(is_action(selected, cue::EditorActionKind::SetGameBuildTarget));
    CHECK(selected.try_value()->front().buildTarget.platform == "Windows" &&
          selected.try_value()->front().buildTarget.architecture == "x64" &&
          selected.try_value()->front().buildTarget.configuration == cue::GameBuildConfiguration::Release);
    CHECK(display.build.selectedTarget.configuration == cue::GameBuildConfiguration::Debug);
    CHECK(frame(*ui, app->document(), display).has_value());

    // Host の誤った canBuild 表示値でも、未選択・未対応 Architecture では起動要求を作らない
    display.build.selectedTarget.architecture = "ARM64";
    auto unsupported = activate(*ui, app->document(), display, *panel, "Build");
    CHECK(unsupported.has_value() && unsupported.try_value()->empty());
    display.build.selectedTarget.architecture = "x64";
    display.build.isBuilding = true;
    display.build.canCancel = true;
    display.build.canRun = true;
    auto duplicate = activate(*ui, app->document(), display, *panel, "Build");
    CHECK(duplicate.has_value() && duplicate.try_value()->empty());
    auto cancel = activate(*ui, app->document(), display, *panel, "Cancel Build");
    CHECK(is_action(cancel, cue::EditorActionKind::CancelGameBuild));
    auto runBusy = activate(*ui, app->document(), display, *panel, "Run");
    CHECK(runBusy.has_value() && runBusy.try_value()->empty());
    // Build 中は Scene 保存 Shortcut も抑止する
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_S, true);
    auto saveBusy = frame(*ui, app->document(), display);
    CHECK(saveBusy.has_value() && saveBusy.try_value()->empty());
    io.AddKeyEvent(ImGuiKey_S, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CHECK(frame(*ui, app->document(), display).has_value());
    display.build.isBuilding = false;
    display.build.canCancel = false;
    display.build.isApplicationRunning = true;
    auto appRunning = activate(*ui, app->document(), display, *panel, "Build");
    CHECK(appRunning.has_value() && appRunning.try_value()->empty());
    display.build.isApplicationRunning = false;
    auto run = activate(*ui, app->document(), display, *panel, "Run");
    CHECK(is_action(run, cue::EditorActionKind::RunGame));

    // ログ内の書式記号は Text として描画し、所有 Buffer は展開しても変更しない
    const std::string log = "日本語 Build output\n%n %s %%\n";
    display.build.logText = log;
    display.build.outputDirectory = "C:/配布済み Cube/App";
    display.build.statusText = "Succeeded";
    display.build.exitCode = 0;
    display.build.isLogTruncated = true;
    CHECK(activate(*ui, app->document(), display, *panel, "Build Log").has_value());
    CHECK(frame(*ui, app->document(), display).has_value());
    CHECK(display.build.logText == log);

    // Build 確認は通常 Discard と異なり、保存済み File の採用を明示する
    const std::array choices{std::pair{"Save and Build", cue::EditorActionKind::ConfirmSaveChanges},
                             std::pair{"Build Saved Files", cue::EditorActionKind::ConfirmDiscardChanges},
                             std::pair{"Cancel", cue::EditorActionKind::CancelDocumentChange}};
    for (const auto &[label, kind] : choices)
    {
        display.isUnsavedConfirmationRequested = true;
        display.isBuildConfirmationRequested = true;
        ui->reset();
        CHECK(frame(*ui, app->document(), display).has_value());
        CHECK(frame(*ui, app->document(), display).has_value());
        auto *popup = ImGui::FindWindowByName("Save changes?##EditorUnsavedChanges");
        CHECK(popup && popup->Active);
        auto blocked = activate(*ui, app->document(), display, *panel, "Build");
        CHECK(blocked.has_value() && blocked.try_value()->empty());
        auto answer = activate(*ui, app->document(), display, *popup, label);
        CHECK(is_action(answer, kind));
        auto once = frame(*ui, app->document(), display);
        CHECK(once.has_value() && once.try_value()->empty());
        display.isUnsavedConfirmationRequested = false;
        display.isBuildConfirmationRequested = false;
        CHECK(frame(*ui, app->document(), display).has_value());
    }
    auto unchanged = app->document().scene().serialize();
    CHECK(unchanged.has_value() && *unchanged.try_value() == *original.try_value());
    CHECK(app->document().is_dirty());
    CHECK(app->shutdown().has_value());
    return 0;
}
