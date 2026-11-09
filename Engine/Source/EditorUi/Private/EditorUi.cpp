#include <EditorUi/EditorUi.h>

#include <algorithm>
#include <new>
#include <optional>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>

#include <Editor/SceneDocument.h>

#include "EditorUiPanels.h"

namespace cue
{
namespace
{
/// @brief Menu と Status の間を背景付き Docking 先にし、旧 Engine と同様に空き領域も Editor が描画する
void draw_editor_dockspace(const EditorUiContext &a_context)
{
    if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable))
        return;
    const auto *viewport = ImGui::GetMainViewport();
    const float top = editor_panel_top();
    const float statusHeight = ImGui::GetFrameHeight() + 6;
    ImGui::SetNextWindowPos({viewport->Pos.x, top});
    ImGui::SetNextWindowSize({(std::max)(a_context.displayWidth, 1.0f),
                              (std::max)(a_context.displayHeight - (top - viewport->Pos.y) - statusHeight, 1.0f)});
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    PanelScope background("EditorDockSpace", ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                                 ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(3);
    if (background.is_visible())
        ImGui::DockSpace(ImGui::GetID("EditorDockArea"), {0, 0}, ImGuiDockNodeFlags_None);
}
/// @brief Document がない起動状態を表示し、内部用の Scene や World を生成しない
void draw_empty_project_panels(const EditorUiContext &a_context)
{
    const auto *viewport = ImGui::GetMainViewport();
    const float width = (std::max)(a_context.displayWidth, 1.0f);
    const float height = (std::max)(a_context.displayHeight, 1.0f);
    const float top = editor_panel_top();
    const float panelHeight = (std::max)(height - (top - viewport->Pos.y) - 32, 64.0f);
    const float leftWidth = (std::min)(240.0f, width * 0.22f);
    const float rightWidth = (std::min)(300.0f, width * 0.27f);
    set_initial_panel(viewport->WorkPos.x, top, leftWidth, panelHeight);
    {
        PanelScope panel("Objects##EditorObjects", ImGuiWindowFlags_NoCollapse);
        if (panel.is_visible())
            ImGui::TextUnformatted("No project open");
    }
    set_initial_panel(viewport->WorkPos.x + width - rightWidth, top, rightWidth, panelHeight);
    {
        PanelScope panel("Inspector##EditorInspector", ImGuiWindowFlags_NoCollapse);
        if (panel.is_visible())
            ImGui::TextUnformatted("No project open");
    }
}
/// @brief Modal の選択・取消だけを返し、確認待ちの間も通常 Frame を進行させる
void draw_unsaved_confirmation(bool a_isRequested, bool a_isBuild, bool &a_wasRequested, bool &a_hasResponse,
                               bool &a_shouldClose, std::vector<EditorAction> &a_actions)
{
    constexpr const char *k_popupName = "Save changes?##EditorUnsavedChanges";
    // reset は Context 外から呼ばれるため、表示中 Popup の回収を描画境界へ遅延する
    if (a_shouldClose)
    {
        if (ImGui::BeginPopupModal(k_popupName))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        a_shouldClose = false;
    }
    // Host が要求を取り消した場合も Popup を閉じ、次の確認を別の操作として受け付ける
    if (!a_isRequested)
    {
        if (a_wasRequested && ImGui::BeginPopupModal(k_popupName))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        a_wasRequested = false;
        a_hasResponse = false;
        return;
    }
    if (!a_wasRequested)
    {
        ImGui::OpenPopup(k_popupName);
        a_wasRequested = true;
    }
    // Host が返却済み Action を適用するまで、同じ確認を再生成・再送しない
    if (a_hasResponse)
        return;

    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    bool isOpen = true;
    const bool isVisible = ImGui::BeginPopupModal(k_popupName, &isOpen,
                                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                                      ImGuiWindowFlags_NoDocking);
    if (isVisible)
    {
        struct PopupScope final
        {
            /// @brief 選択 Action の割当失敗でも Popup の Stack を閉じる
            ~PopupScope()
            {
                ImGui::EndPopup();
            }
        } popup;
        ImGui::TextUnformatted("This scene has unsaved changes.");
        ImGui::TextUnformatted(a_isBuild ? "Save before building the project startup scene?"
                                         : "Save before continuing?");
        if (a_isBuild)
            ImGui::TextUnformatted("Build Saved Files keeps your unsaved edits in the Editor.");
        ImGui::Separator();
        std::optional<EditorActionKind> response;
        if (ImGui::Button(a_isBuild ? "Save and Build" : "Save", {a_isBuild ? 150.0f : 100.0f, 0}))
            response = EditorActionKind::ConfirmSaveChanges;
        ImGui::SameLine();
        if (ImGui::Button(a_isBuild ? "Build Saved Files" : "Discard", {a_isBuild ? 150.0f : 100.0f, 0}))
            response = EditorActionKind::ConfirmDiscardChanges;
        ImGui::SameLine();
        // Modal の初回 Frame でも取消を観測する。Shortcut の前 Frame 予約や入力所有権へ依存しない
        const bool isCancelled = ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, ImGuiKeyOwner_Any);
        if (ImGui::Button("Cancel", {100, 0}) || isCancelled)
            response = EditorActionKind::CancelDocumentChange;
        if (response)
        {
            emit_editor_action(a_actions, *response);
            a_hasResponse = true;
            ImGui::CloseCurrentPopup();
        }
    }
    // Title Bar の X でも明示的な取消を返し、保留中の処理を残さない
    if (!isOpen && !a_hasResponse)
    {
        emit_editor_action(a_actions, EditorActionKind::CancelDocumentChange);
        a_hasResponse = true;
    }
}
} // namespace

struct EditorUi::State final : EditorUiState
{
    bool wasUnsavedConfirmationRequested = false;
    bool hasUnsavedResponse = false;
    bool shouldCloseUnsavedPopup = false;
};

EditorUi::EditorUi(CreateToken) noexcept
{
}
EditorUi::~EditorUi() = default;

Result<std::unique_ptr<EditorUi>> EditorUi::create()
{
    try
    {
        auto ui = std::make_unique<EditorUi>(CreateToken{});
        ui->m_state = std::make_unique<State>();
        return Result<std::unique_ptr<EditorUi>>::success(std::move(ui));
    }
    catch (const std::bad_alloc &)
    {
        return Result<std::unique_ptr<EditorUi>>::failure(
            {ErrorCategory::PlatformFailure, "EditorUi.create.allocation"});
    }
}

Result<std::vector<EditorAction>> EditorUi::draw(const SceneDocument &a_document, const EditorUiContext &a_context)
{
    return draw(&a_document, a_context);
}

Result<std::vector<EditorAction>> EditorUi::draw(const SceneDocument *a_document, const EditorUiContext &a_context)
{
    if (!ImGui::GetCurrentContext() || !m_state)
        return Result<std::vector<EditorAction>>::failure({ErrorCategory::InvalidState, "EditorUi.draw.context"});
    try
    {
        auto context = a_context;
        if (!a_document)
        {
            // 呼出側の表示値が残っていても、Document のない Frame では編集要求を生成しない
            reset();
            context.isProjectOpen = false;
            context.isSceneViewEnabled = false;
            context.isUnsavedConfirmationRequested = false;
            context.isBuildConfirmationRequested = false;
            context.canPlay = false;
            context.isPlaying = false;
            context.isPlayBusy = false;
            context.canStop = false;
            context.script = {};
            context.build = {};
        }
        std::vector<EditorAction> actions;
        actions.reserve(16);
        // Modal の初回 Frame も無効化し、Popup がまだ開く前の背景入力を処理しない
        ImGui::BeginDisabled(context.isUnsavedConfirmationRequested);
        struct DisabledScope final
        {
            /// @brief Panel の失敗・例外でも Disabled の Stack を対応させる
            ~DisabledScope()
            {
                ImGui::EndDisabled();
            }
        };
        {
            DisabledScope disabled;
            draw_editor_dockspace(context);
            if (a_document)
            {
                // 未選択の起動 Shell は表示だけを維持し、仮 Document を編集対象として公開しない
                EditorDisabledScope noProject(!context.isProjectOpen);
                draw_object_list(*a_document, context, *m_state, actions);
                auto inspector = draw_transform_inspector(*a_document, context, *m_state, actions);
                if (!inspector.has_value())
                    return Result<std::vector<EditorAction>>::failure(*inspector.try_error());
                draw_build_panel(context, *m_state, actions);
            }
            else
                draw_empty_project_panels(context);
            draw_scene_view(context, actions);
            draw_editor_status(a_document, context);
            // Panel より後に Menu を構築し、旧 Layout から移行する Frame でも入力領域を確保する
            draw_scene_menu(a_document, *m_state, actions, context, !context.isUnsavedConfirmationRequested);
        }
        if (context.isUnsavedConfirmationRequested)
            actions.clear();
        draw_unsaved_confirmation(context.isUnsavedConfirmationRequested, context.isBuildConfirmationRequested,
                                  m_state->wasUnsavedConfirmationRequested, m_state->hasUnsavedResponse,
                                  m_state->shouldCloseUnsavedPopup, actions);
        return Result<std::vector<EditorAction>>::success(std::move(actions));
    }
    catch (const std::bad_alloc &)
    {
        return Result<std::vector<EditorAction>>::failure({ErrorCategory::PlatformFailure, "EditorUi.draw.allocation"});
    }
}

void EditorUi::reset() noexcept
{
    if (!m_state)
        return;
    m_state->selectedId.clear();
    m_state->hasTransform = false;
    m_state->hasEdit = false;
    m_state->activeField = -1;
    m_state->activeInputId = 0;
    m_state->needsSync = true;
    m_state->hasInvalidRotationInput = false;
    m_state->playTransform = {};
    m_state->wasPlaying = false;
    // 応答消費直後の次 Pump が別の確認を開始しても、false Frame を挟まず再表示できるようにする
    m_state->shouldCloseUnsavedPopup |= m_state->wasUnsavedConfirmationRequested && !m_state->hasUnsavedResponse;
    m_state->wasUnsavedConfirmationRequested = false;
    m_state->hasUnsavedResponse = false;
}

void emit_editor_action(std::vector<EditorAction> &a_actions, EditorActionKind a_kind, std::string_view a_objectId,
                        const math::floatTransform &a_transform)
{
    a_actions.push_back({a_kind, std::string(a_objectId), a_transform});
}

void finish_transform_input(EditorUiState &a_state, std::vector<EditorAction> &a_actions, EditorActionKind a_kind)
{
    if (!a_state.hasEdit)
        return;
    emit_editor_action(a_actions, a_kind, a_state.selectedId);
    a_state.hasEdit = false;
    a_state.activeField = -1;
    a_state.activeInputId = 0;
    a_state.needsSync = true;
}
} // namespace cue
