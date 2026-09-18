#include <Cue/Editor/ImGui/DebugView.h>
#include <Cue/Editor/ImGui/EditorDockspace.h>

#include <cstdlib>

#include <imgui.h>
#include <imgui_internal.h>

namespace
{
/// @brief 条件違反をTest失敗へ変換する
void require(bool a_condition, int a_exitCode) noexcept
{
    if (!a_condition)
    {
        std::_Exit(a_exitCode);
    }
}

/// @brief 固定Editor DockspaceとDebug Viewの一Frameを構築する
[[nodiscard]] cue::editor::DebugViewRequest draw_frame() noexcept
{
    ImGui::NewFrame();
    static_cast<void>(cue::editor::begin_editor_dockspace_host());
    cue::editor::end_editor_dockspace_host();
    const cue::editor::DebugViewRequest request = cue::editor::draw_debug_view({});
    ImGui::Render();
    return request;
}
} // namespace

/// @brief Debug ViewがEditor Dockspace内で有効なSurface寸法を要求することを検証する
int main()
{
    require(ImGui::CreateContext() != nullptr, 1);
    ImGuiIO &input = ImGui::GetIO();
    input.IniFilename = nullptr;
    input.DisplaySize = ImVec2(1280.0F, 720.0F);
    input.DeltaTime = 1.0F / 60.0F;
    cue::editor::enable_editor_docking();
    static_cast<void>(input.Fonts->Build());

    const cue::editor::DebugViewRequest visible = draw_frame();
    const ImGuiWindow *debugView = ImGui::FindWindowByName("Debug View");
    require(debugView != nullptr && debugView->DockId != 0U, 2);
    require(visible.isVisible && visible.width != 0U && visible.height != 0U, 3);
    ImGui::DestroyContext();
    return 0;
}
