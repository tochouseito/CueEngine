#include <EditorUi/EditorUi.h>

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
