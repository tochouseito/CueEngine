#include <Cue/Editor/ImGui/EditorDockspace.h>

#include <imgui.h>

namespace
{
constexpr ImGuiID k_editorDockspaceId = 0xCEED1701U;
constexpr ImGuiWindowFlags k_editorDockspaceHostFlags =
    ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
} // namespace

namespace cue::editor
{
void enable_editor_docking() noexcept
{
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
}

bool begin_editor_dockspace_host() noexcept
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    return ImGui::Begin("CueEngine Editor", nullptr, k_editorDockspaceHostFlags);
}

void end_editor_dockspace_host() noexcept
{
    ImGui::DockSpace(k_editorDockspaceId, ImVec2(0.0F, 0.0F));
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void dock_editor_window_on_first_use() noexcept
{
    ImGui::SetNextWindowDockID(k_editorDockspaceId, ImGuiCond_FirstUseEver);
}
} // namespace cue::editor
