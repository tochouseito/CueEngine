#include <EditorUi/EditorUi.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>

#include <Editor/EditorApplication.h>
#include <Scene/Scene.h>

namespace
{
#define CUE_SCRIPT_UI_CHECK(...)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Script UI test failed at line %d: %s\n", __LINE__, #__VA_ARGS__);                    \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Native Window を生成せず、Script Panel の実 Widget 入力を所有値の Action へ変換する
cue::Result<std::vector<cue::EditorAction>> frame(cue::EditorUi &a_ui, const cue::SceneDocument &a_document,
                                                  const cue::EditorUiContext &a_display)
{
    ImGui::NewFrame();
    auto result = a_ui.draw(a_document, a_display);
    ImGui::Render();
    return result;
}

/// @brief Play 用の三構成を独立した Action にし、製品 Target と実行中の選択を変更しないことを確認する
int test_game_configuration(cue::EditorUi &a_ui, const cue::SceneDocument &a_document,
                            const cue::EditorUiContext &a_display)
{
    auto display = a_display;
    display.script.canSelectConfiguration = true;
    display.build.selectedTarget = {"Windows", "x64", cue::GameBuildConfiguration::Release};
    const auto productTarget = display.build.selectedTarget;
    CUE_SCRIPT_UI_CHECK(display.script.configuration == cue::GameBuildConfiguration::Development);
    CUE_SCRIPT_UI_CHECK(frame(a_ui, a_document, display).has_value());
    auto *menuBar = ImGui::FindWindowByName("##MainMenuBar");
    CUE_SCRIPT_UI_CHECK(menuBar && menuBar->Active);
    // 表示 Text はメモリだけへ捕捉し、未 Build の案内が操作要求や File I/O を発生させないことを確認する
    for (const bool isRequired : {false, true})
    {
        display.script.isBuildRequired = isRequired;
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        auto status = a_ui.draw(a_document, display);
        const bool hasNotice = std::strstr(ImGui::GetCurrentContext()->LogBuffer.c_str(), "Build required") != nullptr;
        ImGui::LogFinish();
        ImGui::Render();
        CUE_SCRIPT_UI_CHECK(status.has_value() && status.try_value()->empty());
        CUE_SCRIPT_UI_CHECK(hasNotice == isRequired);
    }
    display.script.isBuildRequired = false;
    const auto menuBarSeed = ImHashStr("##MenuBar", 0, menuBar->ID);
    const auto configurationId = ImHashStr("##GameConfiguration", 0, menuBarSeed);
    constexpr std::array configurations{cue::GameBuildConfiguration::Debug, cue::GameBuildConfiguration::Development,
                                        cue::GameBuildConfiguration::Release};
    for (const auto configuration : configurations)
    {
        ImGui::ActivateItemByID(configurationId);
        auto opened = frame(a_ui, a_document, display);
        CUE_SCRIPT_UI_CHECK(opened.has_value() && opened.try_value()->empty());
        auto *combo = ImGui::FindWindowByName("##Combo_00");
        CUE_SCRIPT_UI_CHECK(combo && combo->Active);
        const auto label = cue::game_build_configuration_name(configuration);
        ImGui::ActivateItemByID(ImHashStr(label.data(), 0, combo->ID));
        auto selected = frame(a_ui, a_document, display);
        CUE_SCRIPT_UI_CHECK(selected.has_value() && selected.try_value()->size() == 1);
        const auto &action = selected.try_value()->front();
        CUE_SCRIPT_UI_CHECK(action.kind == cue::EditorActionKind::SetGameConfiguration);
        CUE_SCRIPT_UI_CHECK(action.gameConfiguration == configuration);
        CUE_SCRIPT_UI_CHECK(display.script.configuration != configuration);
        CUE_SCRIPT_UI_CHECK(display.build.selectedTarget == productTarget);
        // Host による採用を再現し、次の Frame へ選択値だけを返す
        display.script.configuration = configuration;
        CUE_SCRIPT_UI_CHECK(frame(a_ui, a_document, display).has_value());
    }
    // 同じ値を選び直しても再 Build や設定保存の要求を増やさない
    ImGui::ActivateItemByID(configurationId);
    CUE_SCRIPT_UI_CHECK(frame(a_ui, a_document, display).has_value());
    auto *combo = ImGui::FindWindowByName("##Combo_00");
    CUE_SCRIPT_UI_CHECK(combo && combo->Active);
    ImGui::ActivateItemByID(ImHashStr("Release", 0, combo->ID));
    auto unchanged = frame(a_ui, a_document, display);
    CUE_SCRIPT_UI_CHECK(unchanged.has_value() && unchanged.try_value()->empty());
    CUE_SCRIPT_UI_CHECK(frame(a_ui, a_document, display).has_value());

    for (unsigned reason = 0; reason < 8; ++reason)
    {
        auto blocked = display;
        switch (reason)
        {
        case 0:
            blocked.script.canSelectConfiguration = false;
            break;
        case 1:
            blocked.isPlaying = true;
            break;
        case 2:
            blocked.isPlayBusy = true;
            break;
        case 3:
            blocked.script.isBusy = true;
            break;
        case 4:
            blocked.build.isBuilding = true;
            break;
        case 5:
            blocked.build.isApplicationRunning = true;
            break;
        case 6:
            blocked.isProjectOpen = false;
            break;
        case 7:
            blocked.isUnsavedConfirmationRequested = true;
            break;
        }
        CUE_SCRIPT_UI_CHECK(frame(a_ui, a_document, blocked).has_value());
        ImGui::ActivateItemByID(configurationId);
        auto rejected = frame(a_ui, a_document, blocked);
        CUE_SCRIPT_UI_CHECK(rejected.has_value() && rejected.try_value()->empty());
        const auto *popup = ImGui::FindWindowByName("##Combo_00");
        CUE_SCRIPT_UI_CHECK(!popup || !popup->Active);
        CUE_SCRIPT_UI_CHECK(blocked.script.configuration == display.script.configuration);
    }
    CUE_SCRIPT_UI_CHECK(frame(a_ui, a_document, a_display).has_value());
    CUE_SCRIPT_UI_CHECK(frame(a_ui, a_document, a_display).has_value());
    CUE_SCRIPT_UI_CHECK(a_display.script.configuration == cue::GameBuildConfiguration::Development);
    return 0;
}

/// @brief Source 作成 Modal の実入力を所有値の要求へ変換し、取消・無効入力・操作競合を確認する
int test_script_creation(cue::EditorUi &a_ui, const cue::SceneDocument &a_document,
                         const cue::EditorUiContext &a_display)
{
    auto display = a_display;
    display.script.canCreate = true;
    auto &io = ImGui::GetIO();
    const auto advance = [&]()
    {
        auto result = frame(a_ui, a_document, display);
        return result.has_value() && result.try_value()->empty();
    };
    const auto open = [&]()
    {
        if (!advance() || !advance())
            return false;
        auto *menuBar = ImGui::FindWindowByName("##MainMenuBar");
        if (!menuBar || !menuBar->Active)
            return false;
        float scriptX = menuBar->Pos.x + ImGui::GetStyle().FramePadding.x;
        for (const char *label : {"File", "Edit", "View"})
            scriptX += ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemSpacing.x * 2;
        scriptX += ImGui::CalcTextSize("Script").x * 0.5f + ImGui::GetStyle().ItemSpacing.x;
        io.AddMousePosEvent(scriptX, menuBar->Pos.y + menuBar->Size.y * 0.5f);
        if (!advance())
            return false;
        io.AddMouseButtonEvent(0, true);
        if (!advance())
            return false;
        io.AddMouseButtonEvent(0, false);
        if (!advance())
            return false;
        const auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
        auto *menu = stack.empty() ? nullptr : stack.back().Window;
        if (!menu || !menu->Active)
            return false;
        ImGui::ActivateItemByID(ImHashStr("New Script...", 0, menu->ID));
        if (!advance() || !advance())
            return false;
        const auto *popup = ImGui::FindWindowByName("New Script##EditorCreateScript");
        return popup && popup->Active;
    };
    const auto type = [&](const char *a_name)
    {
        auto *popup = ImGui::FindWindowByName("New Script##EditorCreateScript");
        if (!popup || !popup->Active)
            return false;
        ImGui::ActivateItemByID(ImHashStr("Name", 0, popup->ID));
        if (!advance())
            return false;
        io.AddInputCharactersUTF8(a_name);
        return advance();
    };
    const auto click = [&](const char *a_label)
    {
        const auto *popup = ImGui::FindWindowByName("New Script##EditorCreateScript");
        ImGui::ActivateItemByID(ImHashStr(a_label, 0, popup->ID));
        return frame(a_ui, a_document, display);
    };

    CUE_SCRIPT_UI_CHECK(open() && type("../Unsafe"));
    auto invalid = click("Create");
    CUE_SCRIPT_UI_CHECK(invalid.has_value() && invalid.try_value()->empty());
    CUE_SCRIPT_UI_CHECK(ImGui::FindWindowByName("New Script##EditorCreateScript")->Active);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    CUE_SCRIPT_UI_CHECK(advance());
    io.AddKeyEvent(ImGuiKey_Escape, false);
    CUE_SCRIPT_UI_CHECK(advance());

    CUE_SCRIPT_UI_CHECK(open() && type("RotateCube"));
    auto created = click("Create");
    CUE_SCRIPT_UI_CHECK(created.has_value() && created.try_value()->size() == 1);
    CUE_SCRIPT_UI_CHECK(created.try_value()->front().kind == cue::EditorActionKind::CreateScript);
    CUE_SCRIPT_UI_CHECK(created.try_value()->front().scriptName == "RotateCube");
    CUE_SCRIPT_UI_CHECK(advance() && advance());
    CUE_SCRIPT_UI_CHECK(!ImGui::FindWindowByName("New Script##EditorCreateScript")->Active);

    CUE_SCRIPT_UI_CHECK(open() && type("Cancelled"));
    auto cancelled = click("Cancel");
    CUE_SCRIPT_UI_CHECK(cancelled.has_value() && cancelled.try_value()->empty());
    CUE_SCRIPT_UI_CHECK(advance());

    CUE_SCRIPT_UI_CHECK(open() && type("PreviousProject"));
    a_ui.reset();
    CUE_SCRIPT_UI_CHECK(advance() && advance());
    CUE_SCRIPT_UI_CHECK(!ImGui::FindWindowByName("New Script##EditorCreateScript")->Active);
    CUE_SCRIPT_UI_CHECK(open());
    ImGui::NewFrame();
    ImGui::LogToBuffer();
    auto emptyName = a_ui.draw(a_document, display);
    CUE_SCRIPT_UI_CHECK(std::strstr(ImGui::GetCurrentContext()->LogBuffer.c_str(), "Source/.cpp") != nullptr);
    ImGui::LogFinish();
    ImGui::Render();
    CUE_SCRIPT_UI_CHECK(emptyName.has_value() && emptyName.try_value()->empty());
    CUE_SCRIPT_UI_CHECK(click("Cancel").has_value() && advance());

    for (unsigned reason = 0; reason < 9; ++reason)
    {
        display = a_display;
        display.script.canCreate = true;
        CUE_SCRIPT_UI_CHECK(open() && type("Blocked"));
        switch (reason)
        {
        case 0:
            display.script.canCreate = false;
            break;
        case 1:
            display.isProjectOpen = false;
            break;
        case 2:
            display.isPlaying = true;
            break;
        case 3:
            display.isPlayBusy = true;
            break;
        case 4:
            display.script.isBusy = true;
            break;
        case 5:
            display.build.isBuilding = true;
            break;
        case 6:
            display.build.isApplicationRunning = true;
            break;
        case 7:
            display.isUnsavedConfirmationRequested = true;
            break;
        case 8:
            display.isBuildConfirmationRequested = true;
            break;
        }
        auto rejected = click("Create");
        CUE_SCRIPT_UI_CHECK(rejected.has_value() && rejected.try_value()->empty());
        display = a_display;
        CUE_SCRIPT_UI_CHECK(advance() && advance());
        CUE_SCRIPT_UI_CHECK(!ImGui::FindWindowByName("New Script##EditorCreateScript")->Active);
    }
    CUE_SCRIPT_UI_CHECK(frame(a_ui, a_document, a_display).has_value());
    return 0;
}
} // namespace

/// @brief Script の有効化操作と Play Property 操作が編集正本を直接変更しないことを検証する
int main()
{
    cue::SceneDescriptor descriptor;
    descriptor.sceneId = "11111111-1111-4111-8111-111111111111";
    descriptor.name = "Script UI";
    cue::SceneObjectDescriptor object;
    object.objectId = "22222222-2222-4222-8222-222222222222";
    object.name = "Cube";
    cue::ScriptDescriptor script;
    script.instanceId = "33333333-3333-4333-8333-333333333333";
    script.typeId = "sample.RotateCube";
    script.properties.push_back({"reverse", cue::ScriptPropertyKind::Boolean, 0, false});
    script.properties.push_back({"speed", cue::ScriptPropertyKind::Number, 1, false});
    object.scripts.push_back(script);
    descriptor.objects.push_back(object);
    auto scene = cue::Scene::create(descriptor);
    CUE_SCRIPT_UI_CHECK(scene.has_value());
    auto appResult = cue::EditorApplication::create(scene.take_value());
    CUE_SCRIPT_UI_CHECK(appResult.has_value());
    auto app = appResult.take_value();
    CUE_SCRIPT_UI_CHECK(app->select_object(object.objectId).has_value());
    auto uiResult = cue::EditorUi::create();
    CUE_SCRIPT_UI_CHECK(uiResult.has_value());
    auto ui = uiResult.take_value();

    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> imgui(ImGui::CreateContext(),
                                                                          &ImGui::DestroyContext);
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = {1280, 720};
    io.DeltaTime = 1.0f / 60.0f;
    CUE_SCRIPT_UI_CHECK(io.Fonts->Build());
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    cue::EditorUiContext display;
    // 不明型のまま描画しても保存値を保持し、ゲーム Code を呼ばない
    auto original = app->document().scene().serialize();
    CUE_SCRIPT_UI_CHECK(original.has_value());
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    CUE_SCRIPT_UI_CHECK(test_game_configuration(*ui, app->document(), display) == 0);
    CUE_SCRIPT_UI_CHECK(test_script_creation(*ui, app->document(), display) == 0);
    auto *inspector = ImGui::FindWindowByName("Inspector##EditorInspector");
    CUE_SCRIPT_UI_CHECK(inspector && inspector->Active);
    const auto authoringSeed = ImHashStr("0", 0, ImHashStr("AuthoringScripts", 0, inspector->ID));
    const auto scriptSeed = ImHashStr(script.instanceId.c_str(), 0, authoringSeed);
    ImGui::ActivateItemByID(ImHashStr("Enabled", 0, scriptSeed));
    auto enabled = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(enabled.has_value() && enabled.try_value()->size() == 1);
    const auto &enabledAction = enabled.try_value()->front();
    CUE_SCRIPT_UI_CHECK(enabledAction.kind == cue::EditorActionKind::SetScriptEnabled);
    CUE_SCRIPT_UI_CHECK(enabledAction.objectId == object.objectId &&
                        enabledAction.scriptInstanceId == script.instanceId);
    CUE_SCRIPT_UI_CHECK(!enabledAction.isEnabled);
    auto unchanged = app->document().scene().serialize();
    CUE_SCRIPT_UI_CHECK(unchanged.has_value() && *unchanged.try_value() == *original.try_value());

    // Source 表示は所有する型 ID だけを要求し、Scene 正本や未知型の保存値を変更しない
    display.script.canEditSource = true;
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    ImGui::ActivateItemByID(ImHashStr("Edit Source", 0, scriptSeed));
    auto editSource = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(editSource.has_value() && editSource.try_value()->size() == 1);
    CUE_SCRIPT_UI_CHECK(editSource.try_value()->front().kind == cue::EditorActionKind::OpenScriptSource);
    CUE_SCRIPT_UI_CHECK(editSource.try_value()->front().scriptTypeId == script.typeId);
    display.script.canEditSource = false;
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    ImGui::ActivateItemByID(ImHashStr("Edit Source", 0, scriptSeed));
    auto disabledSource = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(disabledSource.has_value() && disabledSource.try_value()->empty());

    cue::ScriptTypeDescriptor type;
    type.typeId = script.typeId;
    type.displayName = "Rotate Cube";
    type.properties.push_back({"reverse", "Reverse", cue::ScriptPropertyKind::Boolean, 0, false});
    type.properties.push_back({"speed", "Speed", cue::ScriptPropertyKind::Number, 1, false});
    const std::vector types{type};
    display.scriptTypes = types;
    display.canPlay = true;
    display.isPlaying = true;
    display.playSessionId = 123;
    auto playScene = descriptor;
    display.playScene = &playScene;
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    // Play 中は Script の構造操作を拒否するが、登録済み Property は通信要求へ変換する
    const auto playSeed = ImHashStr("123", 0, ImHashStr("PlayScripts", 0, inspector->ID));
    const auto playScriptSeed = ImHashStr(script.instanceId.c_str(), 0, playSeed);
    ImGui::ActivateItemByID(ImHashStr("Enabled", 0, playScriptSeed));
    auto blocked = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(blocked.has_value() && blocked.try_value()->empty());
    const auto propertySeed = ImHashStr("reverse", 0, playScriptSeed);
    ImGui::ActivateItemByID(ImHashStr("##Value", 0, propertySeed));
    auto property = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(property.has_value() && property.try_value()->size() == 1);
    const auto &propertyAction = property.try_value()->front();
    CUE_SCRIPT_UI_CHECK(propertyAction.kind == cue::EditorActionKind::SetScriptProperty);
    CUE_SCRIPT_UI_CHECK(propertyAction.objectId == object.objectId &&
                        propertyAction.scriptInstanceId == script.instanceId);
    CUE_SCRIPT_UI_CHECK(propertyAction.property.propertyId == "reverse" && propertyAction.property.boolean);
    // 数値は入力 Buffer に保持し、Enter より前に通信 Action や保存値へ反映しない
    const auto speedSeed = ImHashStr("speed", 0, playScriptSeed);
    const auto speedId = ImHashStr("##Value", 0, speedSeed);
    ImGui::ActivateItemByID(speedId);
    auto activated = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(activated.has_value() && activated.try_value()->empty());
    CUE_SCRIPT_UI_CHECK(ImGui::GetActiveID() == speedId);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    io.AddInputCharactersUTF8("12.5");
    auto typing = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(typing.has_value() && typing.try_value()->empty());
    for (int index = 0; index < 3; ++index)
    {
        auto pending = frame(*ui, app->document(), display);
        CUE_SCRIPT_UI_CHECK(pending.has_value() && pending.try_value()->empty());
        const auto *input = ImGui::GetInputTextState(speedId);
        CUE_SCRIPT_UI_CHECK(input && std::strcmp(input->TextA.Data, "12.5") == 0);
        CUE_SCRIPT_UI_CHECK(playScene.objects.front().scripts.front().properties.back().number == 1);
    }
    io.AddKeyEvent(ImGuiKey_Enter, true);
    auto committed = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(committed.has_value() && committed.try_value()->size() == 1);
    const auto &numberAction = committed.try_value()->front();
    CUE_SCRIPT_UI_CHECK(numberAction.kind == cue::EditorActionKind::SetScriptProperty);
    CUE_SCRIPT_UI_CHECK(numberAction.property.propertyId == "speed" && numberAction.property.number == 12.5);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    auto afterEnter = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(afterEnter.has_value() && afterEnter.try_value()->empty());
    // Play が入力途中で終了しても、その Buffer を Authoring の同じ Property へ流用しない
    ImGui::ActivateItemByID(speedId);
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    io.AddInputCharactersUTF8("42");
    auto incomplete = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(incomplete.has_value() && incomplete.try_value()->empty());
    const auto *incompleteInput = ImGui::GetInputTextState(speedId);
    CUE_SCRIPT_UI_CHECK(incompleteInput && std::strcmp(incompleteInput->TextA.Data, "42") == 0);
    display.isPlaying = false;
    display.playScene = nullptr;
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    io.AddKeyEvent(ImGuiKey_Enter, true);
    auto afterPlay = frame(*ui, app->document(), display);
    CUE_SCRIPT_UI_CHECK(afterPlay.has_value() && afterPlay.try_value()->empty());
    io.AddKeyEvent(ImGuiKey_Enter, false);
    CUE_SCRIPT_UI_CHECK(frame(*ui, app->document(), display).has_value());
    auto final = app->document().scene().serialize();
    CUE_SCRIPT_UI_CHECK(final.has_value() && *final.try_value() == *original.try_value());
    CUE_SCRIPT_UI_CHECK(!playScene.objects.front().scripts.front().properties.front().boolean);
    CUE_SCRIPT_UI_CHECK(app->shutdown().has_value());
    return 0;
}
