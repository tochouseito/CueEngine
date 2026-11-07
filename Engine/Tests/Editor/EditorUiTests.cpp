#include <EditorUi/EditorUi.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>

#include <Editor/EditorApplication.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>

namespace
{
#define CUE_EDITOR_UI_CHECK(...)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Editor UI test failed at line %d: %s\n", __LINE__, #__VA_ARGS__);                    \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Native Window / Renderer なしで一 Frame の実 UI 入力と描画 Data を確定する
cue::Result<std::vector<cue::EditorAction>> frame(cue::EditorUi &a_ui, const cue::SceneDocument &a_document,
                                                  const cue::EditorUiContext &a_context)
{
    ImGui::NewFrame();
    auto result = a_ui.draw(a_document, a_context);
    ImGui::Render();
    return result;
}

/// @brief Document を一つも作らず、未選択 Editor の入力と描画だけを確定する
cue::Result<std::vector<cue::EditorAction>> empty_frame(cue::EditorUi &a_ui, const cue::EditorUiContext &a_context)
{
    ImGui::NewFrame();
    auto result = a_ui.draw(nullptr, a_context);
    ImGui::Render();
    return result;
}

/// @brief 描画 Command が Host の論理 Texture ID を維持しているか確認する
bool has_texture(ImTextureID a_texture)
{
    const auto *data = ImGui::GetDrawData();
    if (!data || !data->Valid)
        return false;
    for (const auto *list : data->CmdLists)
        for (const auto &command : list->CmdBuffer)
            if (command.GetTexID() == a_texture && command.ElemCount != 0)
                return true;
    return false;
}

/// @brief Scene / World / Document が存在しなくても開く要求だけを構築できることを確認する
int test_unselected_editor()
{
    auto uiResult = cue::EditorUi::create();
    CUE_EDITOR_UI_CHECK(uiResult.has_value());
    auto ui = uiResult.take_value();
    cue::EditorUiContext display;
    display.sceneTextureId = 0xCE002;
    // 旧表示値が混入しても、Document のない Frame は編集や保存確認へ進まない
    display.canPlay = true;
    display.script.canBuild = true;
    display.isUnsavedConfirmationRequested = true;
    CUE_EDITOR_UI_CHECK(!ui->draw(nullptr, display).has_value());
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(),
                                                                            &ImGui::DestroyContext);
    CUE_EDITOR_UI_CHECK(context != nullptr);
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = {display.displayWidth, display.displayHeight};
    io.DeltaTime = 1.0f / 60.0f;
    CUE_EDITOR_UI_CHECK(io.Fonts->Build());
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    CUE_EDITOR_UI_CHECK(empty_frame(*ui, display).has_value());
    auto idle = empty_frame(*ui, display);
    CUE_EDITOR_UI_CHECK(idle.has_value() && idle.try_value()->empty());
    CUE_EDITOR_UI_CHECK(!has_texture(static_cast<ImTextureID>(display.sceneTextureId)));
    CUE_EDITOR_UI_CHECK(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    for (const char *name : {"Objects##EditorObjects", "Inspector##EditorInspector", "Scene View##EditorSceneView",
                             "##MainMenuBar", "Status##EditorStatus"})
    {
        const auto *panel = ImGui::FindWindowByName(name);
        CUE_EDITOR_UI_CHECK(panel && panel->Active);
    }
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddKeyEvent(ImGuiKey_O, true);
    auto open = empty_frame(*ui, display);
    CUE_EDITOR_UI_CHECK(open.has_value() && open.try_value()->size() == 1);
    CUE_EDITOR_UI_CHECK(open.try_value()->front().kind == cue::EditorActionKind::OpenProject);
    io.AddKeyEvent(ImGuiKey_O, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    CUE_EDITOR_UI_CHECK(empty_frame(*ui, display).has_value());
    io.AddKeyEvent(ImGuiKey_S, true);
    auto save = empty_frame(*ui, display);
    CUE_EDITOR_UI_CHECK(save.has_value() && save.try_value()->empty());
    io.AddKeyEvent(ImGuiKey_S, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CUE_EDITOR_UI_CHECK(empty_frame(*ui, display).has_value());
    return 0;
}
} // namespace

/// @brief UI が正本を直接変えず、実入力を所有 Action と論理 Texture へ変換することを検証する
int main()
{
    CUE_EDITOR_UI_CHECK(test_unselected_editor() == 0);
    cue::EditorApplicationConfig config;
    unsigned id = 0;
    config.generateId = [&id]()
    {
        char text[37]{};
        std::snprintf(text, sizeof(text), "10000000-0000-4000-8000-%012u", ++id);
        return cue::Result<std::string>::success(text);
    };
    auto applicationResult = cue::EditorApplication::create(std::nullopt, std::move(config));
    CUE_EDITOR_UI_CHECK(applicationResult.has_value());
    auto application = applicationResult.take_value();
    auto uiResult = cue::EditorUi::create();
    CUE_EDITOR_UI_CHECK(uiResult.has_value());
    auto ui = uiResult.take_value();
    cue::EditorUiContext display;
    display.sceneTextureId = 0xCE001;
    CUE_EDITOR_UI_CHECK(!ui->draw(application->document(), display).has_value());

    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(),
                                                                            &ImGui::DestroyContext);
    CUE_EDITOR_UI_CHECK(context != nullptr);
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = {display.displayWidth, display.displayHeight};
    io.DeltaTime = 1.0f / 60.0f;
    CUE_EDITOR_UI_CHECK(io.Fonts->Build());
    // GPU を使わない Fixture なので Font Atlas にも識別用の論理 ID だけを与える
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    auto idle = frame(*ui, application->document(), display);
    CUE_EDITOR_UI_CHECK(idle.has_value() && idle.try_value()->empty());
    CUE_EDITOR_UI_CHECK(has_texture(static_cast<ImTextureID>(display.sceneTextureId)));

    // 初回 Frame と旧 Layout のどちらでも Menu の表示・入力領域を各 Panel が覆わない
    auto *menu = ImGui::FindWindowByName("##MainMenuBar");
    CUE_EDITOR_UI_CHECK(menu && menu->Active && menu->DrawList->VtxBuffer.Size > 0);
    const float menuBottom = menu->Pos.y + menu->Size.y;
    for (const char *name :
         {"Objects##EditorObjects", "Inspector##EditorInspector", "Scene View##EditorSceneView", "EditorDockSpace"})
    {
        auto *panel = ImGui::FindWindowByName(name);
        CUE_EDITOR_UI_CHECK(panel && panel->Pos.y >= menuBottom);
        if (std::strcmp(name, "EditorDockSpace") != 0)
        {
            ImGui::SetWindowPos(name, {panel->Pos.x, 0});
            ImGui::SetWindowSize(name, {panel->Size.x, 4000});
        }
    }
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    // SetWindowSize は次の Begin で確定するため、旧 Layout の補正後にもう一 Frame 進める
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    const auto *status = ImGui::FindWindowByName("Status##EditorStatus");
    CUE_EDITOR_UI_CHECK(status != nullptr);
    for (const char *name : {"Objects##EditorObjects", "Inspector##EditorInspector", "Scene View##EditorSceneView"})
    {
        const auto *panel = ImGui::FindWindowByName(name);
        CUE_EDITOR_UI_CHECK(panel->Pos.y >= menuBottom);
        CUE_EDITOR_UI_CHECK(panel->Pos.y + panel->Size.y <= status->Pos.y);
    }

    // 実際の Button を Mouse で押す。位置だけを Layout から借用し、内部 Action 関数は使わない
    auto *objects = ImGui::FindWindowByName("Objects##EditorObjects");
    CUE_EDITOR_UI_CHECK(objects != nullptr);
    const ImVec2 buttonCenter{objects->DC.CursorStartPos.x + 40,
                              objects->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f};
    io.AddMousePosEvent(buttonCenter.x, buttonCenter.y);
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    io.AddMouseButtonEvent(0, true);
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    io.AddMouseButtonEvent(0, false);
    const auto beforeCount = application->document().scene().descriptor().objects.size();
    auto clicked = frame(*ui, application->document(), display);
    CUE_EDITOR_UI_CHECK(clicked.has_value() && clicked.try_value()->size() == 1);
    CUE_EDITOR_UI_CHECK(clicked.try_value()->front().kind == cue::EditorActionKind::AddCube);
    CUE_EDITOR_UI_CHECK(application->document().scene().descriptor().objects.size() == beforeCount);

    // 適用は UI 構築の外側だけで行い、次の Frame が確定済みの Object を表示する
    auto cube = application->add_cube();
    CUE_EDITOR_UI_CHECK(cube.has_value());
    ui->reset();
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_S, true);
    auto save = frame(*ui, application->document(), display);
    CUE_EDITOR_UI_CHECK(save.has_value() && save.try_value()->size() == 1);
    CUE_EDITOR_UI_CHECK(save.try_value()->front().kind == cue::EditorActionKind::SaveScene);
    CUE_EDITOR_UI_CHECK(application->document().is_dirty());
    io.AddKeyEvent(ImGuiKey_S, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());

    // Rotation の実数値入力へ nan を入力しても、Editor を停止せず有効な姿勢を維持する
    auto *inspector = ImGui::FindWindowByName("Inspector##EditorInspector");
    CUE_EDITOR_UI_CHECK(inspector != nullptr);
    const auto rotationBefore = application->document().scene().descriptor().objects.back().transform->rotation;
    const auto revisionBefore = application->document().revision();
    bool sawBegin = false;
    bool sawCommit = false;
    bool sawCancel = false;
    unsigned cancelCount = 0;
    const auto advance = [&]()
    {
        auto result = frame(*ui, application->document(), display);
        if (!result.has_value())
            return false;
        for (const auto &action : *result.try_value())
        {
            switch (action.kind)
            {
            case cue::EditorActionKind::BeginTransformEdit:
                sawBegin = true;
                if (!application->begin_transform_edit(action.objectId).has_value())
                    return false;
                break;
            case cue::EditorActionKind::PreviewTransform:
                if (!application->preview_transform(action.transform).has_value())
                    return false;
                break;
            case cue::EditorActionKind::CommitTransformEdit:
                sawCommit = true;
                if (!application->commit_transform_edit().has_value())
                    return false;
                break;
            case cue::EditorActionKind::CancelTransformEdit:
                sawCancel = true;
                ++cancelCount;
                if (!application->cancel_transform_edit().has_value())
                    return false;
                break;
            default:
                return false;
            }
        }
        return true;
    };
    const float rotationOffset = 3 * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() +
                                 ImGui::GetStyle().ItemSpacing.y + 1;
    io.AddMousePosEvent(inspector->DC.CursorStartPos.x + 30,
                        inspector->DC.CursorStartPos.y + rotationOffset + ImGui::GetFrameHeight() * 0.5f);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddMouseButtonEvent(0, true);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddMouseButtonEvent(0, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CUE_EDITOR_UI_CHECK(advance());
    CUE_EDITOR_UI_CHECK(io.WantTextInput);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddKeyEvent(ImGuiKey_A, true);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddInputCharactersUTF8("nan");
    CUE_EDITOR_UI_CHECK(advance());
    CUE_EDITOR_UI_CHECK(std::strcmp(context->InputTextState.GetText(), "nan") == 0);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddKeyEvent(ImGuiKey_Enter, false);
    CUE_EDITOR_UI_CHECK(advance());
    CUE_EDITOR_UI_CHECK(sawBegin && sawCommit && !application->document().is_transform_editing());
    CUE_EDITOR_UI_CHECK(application->document().revision() == revisionBefore);
    CUE_EDITOR_UI_CHECK(application->document().scene().descriptor().objects.back().transform->rotation ==
                        rotationBefore);

    // Enter を押す前に有効な入力が正本 Draft と Preview に反映され、Escape で開始値へ戻る
    const auto revisionBeforeLiveInput = application->document().revision();
    sawBegin = false;
    sawCommit = false;
    io.AddMousePosEvent(inspector->DC.CursorStartPos.x + 30,
                        inspector->DC.CursorStartPos.y + rotationOffset + ImGui::GetFrameHeight() * 0.5f);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddMouseButtonEvent(0, true);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddMouseButtonEvent(0, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CUE_EDITOR_UI_CHECK(advance());
    CUE_EDITOR_UI_CHECK(io.WantTextInput);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddKeyEvent(ImGuiKey_A, true);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddInputCharactersUTF8("30");
    CUE_EDITOR_UI_CHECK(advance());
    CUE_EDITOR_UI_CHECK(std::strcmp(context->InputTextState.GetText(), "30") == 0);
    CUE_EDITOR_UI_CHECK(sawBegin && !sawCommit && application->document().is_transform_editing());
    CUE_EDITOR_UI_CHECK(application->document().revision() > revisionBeforeLiveInput);
    const auto draftRotation = application->document().scene().descriptor().objects.back().transform->rotation;
    CUE_EDITOR_UI_CHECK(draftRotation != rotationBefore);
    auto *preview = application->preview_world();
    CUE_EDITOR_UI_CHECK(preview && application->preview_instance());
    auto previewEntity = preview->find_by_id(*application->preview_instance(), *cube.try_value());
    CUE_EDITOR_UI_CHECK(previewEntity.has_value());
    auto previewTransform = preview->get_component<cue::TransformComponent>(*previewEntity.try_value());
    CUE_EDITOR_UI_CHECK(previewTransform.has_value());
    CUE_EDITOR_UI_CHECK((*previewTransform.try_value())->local().rotation == draftRotation);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    CUE_EDITOR_UI_CHECK(advance());
    io.AddKeyEvent(ImGuiKey_Escape, false);
    CUE_EDITOR_UI_CHECK(advance());
    if (!(sawCancel && !sawCommit && !application->document().is_transform_editing()))
        std::fprintf(stderr, "Escape state: cancel=%d commit=%d editing=%d active=%u text=%s\n", sawCancel, sawCommit,
                     application->document().is_transform_editing(), context->ActiveId,
                     context->InputTextState.GetText());
    CUE_EDITOR_UI_CHECK(sawCancel && cancelCount == 1 && !sawCommit && !application->document().is_transform_editing());
    CUE_EDITOR_UI_CHECK(application->document().scene().descriptor().objects.back().transform->rotation ==
                        rotationBefore);
    CUE_EDITOR_UI_CHECK((*previewTransform.try_value())->local().rotation == rotationBefore);

    // 見えているだけでなく File Menu を実 Mouse 入力で開けることを確認する
    io.AddMousePosEvent(menu->Pos.x + 12, menu->Pos.y + menu->Size.y * 0.5f);
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    io.AddMouseButtonEvent(0, true);
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    io.AddMouseButtonEvent(0, false);
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    CUE_EDITOR_UI_CHECK(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    io.AddKeyEvent(ImGuiKey_Escape, true);
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    io.AddKeyEvent(ImGuiKey_Escape, false);
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());

    display.isSceneViewEnabled = false;
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    CUE_EDITOR_UI_CHECK(!has_texture(static_cast<ImTextureID>(display.sceneTextureId)));

    // 未保存 Modal は Frame を止めず、選択だけを一度返す。背景の編集や Shortcut は通さない
    auto beforeModal = application->document().scene().serialize();
    CUE_EDITOR_UI_CHECK(beforeModal.has_value());
    const auto textBeforeModal = beforeModal.take_value();
    const cue::EditorActionKind responses[] = {
        cue::EditorActionKind::ConfirmSaveChanges, cue::EditorActionKind::ConfirmDiscardChanges,
        cue::EditorActionKind::CancelDocumentChange, cue::EditorActionKind::CancelDocumentChange,
        cue::EditorActionKind::CancelDocumentChange};
    for (int choice = 0; choice < 5; ++choice)
    {
        display.isUnsavedConfirmationRequested = false;
        auto resumed = frame(*ui, application->document(), display);
        CUE_EDITOR_UI_CHECK(resumed.has_value() && resumed.try_value()->empty());
        display.isUnsavedConfirmationRequested = true;
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_S, true);
        auto opening = frame(*ui, application->document(), display);
        CUE_EDITOR_UI_CHECK(opening.has_value() && opening.try_value()->empty());
        io.AddKeyEvent(ImGuiKey_S, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        auto waiting = frame(*ui, application->document(), display);
        CUE_EDITOR_UI_CHECK(waiting.has_value() && waiting.try_value()->empty());
        auto *popup = ImGui::FindWindowByName("Save changes?##EditorUnsavedChanges");
        CUE_EDITOR_UI_CHECK(popup && popup->Active && popup->Flags & ImGuiWindowFlags_Modal);

        io.AddMousePosEvent(buttonCenter.x, buttonCenter.y);
        CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
        io.AddMouseButtonEvent(0, true);
        auto blockedDown = frame(*ui, application->document(), display);
        CUE_EDITOR_UI_CHECK(blockedDown.has_value() && blockedDown.try_value()->empty());
        io.AddMouseButtonEvent(0, false);
        auto blockedClick = frame(*ui, application->document(), display);
        CUE_EDITOR_UI_CHECK(blockedClick.has_value() && blockedClick.try_value()->empty());

        cue::Result<std::vector<cue::EditorAction>> response = cue::Result<std::vector<cue::EditorAction>>::success({});
        if (choice < 3 || choice == 4)
        {
            const ImVec2 point = choice == 4
                                     ? ImVec2{popup->Pos.x + popup->Size.x - ImGui::GetStyle().FramePadding.x -
                                                  ImGui::GetFontSize() * 0.5f,
                                              popup->Pos.y + popup->TitleBarHeight * 0.5f}
                                     : ImVec2{popup->DC.CursorStartPos.x + 50 +
                                                  static_cast<float>(choice) * (100 + ImGui::GetStyle().ItemSpacing.x),
                                              popup->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * 0.5f};
            io.AddMousePosEvent(point.x, point.y);
            CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
            io.AddMouseButtonEvent(0, true);
            auto pressed = frame(*ui, application->document(), display);
            CUE_EDITOR_UI_CHECK(pressed.has_value() && pressed.try_value()->empty());
            io.AddMouseButtonEvent(0, false);
            response = frame(*ui, application->document(), display);
        }
        else
        {
            io.AddKeyEvent(ImGuiKey_Escape, true);
            response = frame(*ui, application->document(), display);
        }
        CUE_EDITOR_UI_CHECK(response.has_value() && response.try_value()->size() == 1);
        CUE_EDITOR_UI_CHECK(response.try_value()->front().kind == responses[choice]);
        io.AddKeyEvent(ImGuiKey_Escape, false);
        auto answered = frame(*ui, application->document(), display);
        CUE_EDITOR_UI_CHECK(answered.has_value() && answered.try_value()->empty());
        CUE_EDITOR_UI_CHECK(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    }
    // Host 側の取消でも Popup を残さず、次の要求を新しい確認として受け付ける
    display.isUnsavedConfirmationRequested = false;
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    // Project 未選択でも開く操作を提供し、Scene の保存・編集 Shortcut を発行しない
    display.isProjectOpen = false;
    display.canOpenProject = true;
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    CUE_EDITOR_UI_CHECK(!has_texture(static_cast<ImTextureID>(display.sceneTextureId)));
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddKeyEvent(ImGuiKey_O, true);
    auto openProject = frame(*ui, application->document(), display);
    CUE_EDITOR_UI_CHECK(openProject.has_value() && openProject.try_value()->size() == 1);
    CUE_EDITOR_UI_CHECK(openProject.try_value()->front().kind == cue::EditorActionKind::OpenProject);
    io.AddKeyEvent(ImGuiKey_O, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    io.AddKeyEvent(ImGuiKey_S, true);
    auto noProjectSave = frame(*ui, application->document(), display);
    CUE_EDITOR_UI_CHECK(noProjectSave.has_value() && noProjectSave.try_value()->empty());
    io.AddKeyEvent(ImGuiKey_S, false);
    io.AddKeyEvent(ImGuiMod_Shift, true);
    display.canOpenProject = false;
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    io.AddKeyEvent(ImGuiKey_O, true);
    auto unavailableOpen = frame(*ui, application->document(), display);
    CUE_EDITOR_UI_CHECK(unavailableOpen.has_value() && unavailableOpen.try_value()->empty());
    io.AddKeyEvent(ImGuiKey_O, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    display.isProjectOpen = true;
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    display.isUnsavedConfirmationRequested = true;
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    display.isUnsavedConfirmationRequested = false;
    auto aborted = frame(*ui, application->document(), display);
    CUE_EDITOR_UI_CHECK(aborted.has_value() && aborted.try_value()->empty());
    CUE_EDITOR_UI_CHECK(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    // 応答の消費直後に次の要求が来ても、false Frame なしで確認を再表示する
    display.isUnsavedConfirmationRequested = true;
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    io.AddKeyEvent(ImGuiKey_Escape, true);
    auto cancelBeforeReopen = frame(*ui, application->document(), display);
    CUE_EDITOR_UI_CHECK(cancelBeforeReopen.has_value() && cancelBeforeReopen.try_value()->size() == 1);
    CUE_EDITOR_UI_CHECK(cancelBeforeReopen.try_value()->front().kind == cue::EditorActionKind::CancelDocumentChange);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    ui->reset();
    auto reopened = frame(*ui, application->document(), display);
    CUE_EDITOR_UI_CHECK(reopened.has_value() && reopened.try_value()->empty());
    CUE_EDITOR_UI_CHECK(ImGui::FindWindowByName("Save changes?##EditorUnsavedChanges")->Active);
    // 表示中の reset でも、次の描画境界で旧 Modal を閉じる
    ui->reset();
    display.isUnsavedConfirmationRequested = false;
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    CUE_EDITOR_UI_CHECK(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    // 初回表示と同じ Frame の Escape も取りこぼさない
    display.isUnsavedConfirmationRequested = true;
    io.AddKeyEvent(ImGuiKey_Escape, true);
    auto immediateCancel = frame(*ui, application->document(), display);
    CUE_EDITOR_UI_CHECK(immediateCancel.has_value() && immediateCancel.try_value()->size() == 1);
    CUE_EDITOR_UI_CHECK(immediateCancel.try_value()->front().kind == cue::EditorActionKind::CancelDocumentChange);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    display.isUnsavedConfirmationRequested = false;
    CUE_EDITOR_UI_CHECK(frame(*ui, application->document(), display).has_value());
    auto afterModal = application->document().scene().serialize();
    CUE_EDITOR_UI_CHECK(afterModal.has_value() && *afterModal.try_value() == textBeforeModal);
    CUE_EDITOR_UI_CHECK(application->shutdown().has_value());
    return 0;
}
