#include "EditorUiPanels.h"

#include <algorithm>

namespace cue
{
void draw_scene_view(const EditorUiContext &a_context)
{
    const auto *viewport = ImGui::GetMainViewport();
    const float width = (std::max)(a_context.displayWidth, 1.0f);
    const float height = (std::max)(a_context.displayHeight, 1.0f);
    const float left = (std::min)(240.0f, width * 0.22f);
    const float right = (std::min)(300.0f, width * 0.27f);
    const float top = editor_panel_top();
    set_initial_panel(viewport->WorkPos.x + left, top, (std::max)(width - left - right, 64.0f),
                      (std::max)(height - (top - viewport->Pos.y) - 32, 64.0f));
    PanelScope panel("Scene View##EditorSceneView", ImGuiWindowFlags_NoCollapse);
    if (!panel.is_visible())
        return;
    if (!a_context.isProjectOpen)
    {
        ImGui::TextUnformatted("Open a project from File > Open Project...");
        return;
    }
    if (!a_context.isSceneViewEnabled || a_context.sceneTextureId == 0)
    {
        ImGui::TextUnformatted("Scene View unavailable.");
        return;
    }
    const auto available = ImGui::GetContentRegionAvail();
    if (available.x <= 0 || available.y <= 0)
        return;
    const float aspect = width / height;
    ImVec2 size{available.x, available.x / aspect};
    if (size.y > available.y)
        size = {available.y * aspect, available.y};
    const auto cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos({cursor.x + (available.x - size.x) * 0.5f, cursor.y + (available.y - size.y) * 0.5f});
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(a_context.sceneTextureId)), size);
}

void draw_editor_status(const SceneDocument *a_document, const EditorUiContext &a_context)
{
    const auto *viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight() + 6;
    ImGui::SetNextWindowPos({viewport->Pos.x, viewport->Pos.y + a_context.displayHeight - height});
    ImGui::SetNextWindowSize({(std::max)(a_context.displayWidth, 1.0f), height});
    PanelScope panel("Status##EditorStatus", ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                                 ImGuiWindowFlags_NoNav);
    if (!panel.is_visible())
        return;
    if (!a_context.errorText.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, {1.0f, 0.45f, 0.4f, 1.0f});
        ImGui::TextUnformatted(a_context.errorText.data(), a_context.errorText.data() + a_context.errorText.size());
        ImGui::PopStyleColor();
    }
    else if (!a_context.noticeText.empty())
        ImGui::TextUnformatted(a_context.noticeText.data(), a_context.noticeText.data() + a_context.noticeText.size());
    else if (!a_document || !a_context.isProjectOpen)
        ImGui::TextUnformatted("No project open");
    else
        ImGui::TextUnformatted(a_document->is_dirty() ? "Unsaved changes" : "Ready");
}
} // namespace cue
