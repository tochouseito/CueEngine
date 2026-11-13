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
    set_initial_panel(viewport->WorkPos.x, top, leftWidth, (std::max)(height - (top - viewport->Pos.y) - 32, 64.0f));
    PanelScope panel(hierarchy_panel_title(a_context), ImGuiWindowFlags_NoCollapse);
    if (!panel.is_visible())
        return;
    if (!a_context.isProjectOpen)
    {
        ImGui::TextUnformatted(a_context.text("No project open", "プロジェクト未選択"));
        return;
    }
    const auto selected = a_document.selected_object_id();
    for (const auto &object : a_document.scene().descriptor().objects)
    {
        ImGui::PushID(object.objectId.c_str());
        const bool wasSelected = selected == object.objectId;
        const bool clicked = ImGui::Selectable(object.name.c_str(), wasSelected);
        ImGui::PopID();
        if (clicked && !wasSelected)
        {
            finish_transform_input(a_state, a_actions);
            emit_editor_action(a_actions, EditorActionKind::SelectObject, object.objectId);
        }
    }
}
} // namespace cue
