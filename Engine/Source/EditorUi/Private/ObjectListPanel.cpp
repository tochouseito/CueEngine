#include "EditorUiPanels.h"

#include <algorithm>

namespace cue
{
void draw_object_list(const SceneDocument &a_document, const EditorUiContext &a_context, EditorUiState &a_state,
                      std::vector<EditorAction> &a_actions)
{
    const auto *viewport = ImGui::GetMainViewport();
    const float width = (std::max)(a_context.displayWidth, 1.0f);
    const float height = (std::max)(a_context.displayHeight, 1.0f);
    const float top = editor_panel_top();
    const float leftWidth = (std::min)(240.0f, width * 0.22f);
    set_initial_panel(viewport->WorkPos.x, top, leftWidth,
                      (std::max)(height - (top - viewport->Pos.y) - 32 - a_context.reservedBottomHeight, 64.0f));
    PanelScope panel(hierarchy_panel_title(a_context), ImGuiWindowFlags_NoCollapse);
    if (!panel.is_visible())
        return;
    if (!a_context.isProjectOpen)
    {
        ImGui::TextUnformatted(a_context.text("No project open", "プロジェクト未選択"));
        return;
    }
    const auto selected = a_document.selected_object_id();
    const bool canEdit = !a_context.isPlaying && !a_context.isPlayBusy;
    // 多数の Object を並べても、Label の確保を行ごとに繰り返さない
    std::string label;
    for (const auto &object : a_document.scene().descriptor().objects)
    {
        label.assign(object.camera ? editor_icon::k_camera : editor_icon::k_object);
        label += ' ';
        label += object.name;
        label += "###HierarchyObject";
        ImGui::PushID(object.objectId.c_str());
        const bool wasSelected = selected == object.objectId;
        bool requestedSelect = ImGui::Selectable(label.c_str(), wasSelected) && !wasSelected;
        bool requestedDelete = false;
        if (ImGui::BeginPopupContextItem())
        {
            EditorPopupScope popup;
            ImGui::SeparatorText(object.name.c_str());
            if (ImGui::MenuItem(a_context.text("Select", "選択###Select"), nullptr, false, !wasSelected))
                requestedSelect = true;
            ImGui::Separator();
            requestedDelete = ImGui::MenuItem(a_context.text("Delete", "削除###Delete"), nullptr, false,
                                               canEdit);
        }
        ImGui::PopID();
        if (requestedDelete)
        {
            finish_transform_input(a_state, a_actions);
            emit_editor_action(a_actions, EditorActionKind::DeleteObject, object.objectId);
        }
        else if (requestedSelect)
        {
            finish_transform_input(a_state, a_actions);
            emit_editor_action(a_actions, EditorActionKind::SelectObject, object.objectId);
        }
    }
    if (ImGui::BeginPopupContextWindow("HierarchyActions",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        EditorPopupScope popup;
        if (ImGui::MenuItem(a_context.text("Add Cube", "キューブを追加###Add Cube"), nullptr, false, canEdit))
        {
            finish_transform_input(a_state, a_actions);
            emit_editor_action(a_actions, EditorActionKind::AddCube);
        }
    }
}
} // namespace cue
