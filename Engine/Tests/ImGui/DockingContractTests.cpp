#include <imgui.h>

#ifndef IMGUI_HAS_DOCK
#error "CueEngine requires the Dear ImGui docking branch"
#endif

#ifndef CUE_IMGUI_COMPILE_VERSION
#error "CueEngine requires an explicit Dear ImGui compile version"
#endif

static_assert(CUE_IMGUI_COMPILE_VERSION == IMGUI_VERSION_NUM, "CueEngine Dear ImGui compile version is stale");
static_assert(CUE_IMGUI_COMPILE_VERSION == 19291, "CueEngine requires Dear ImGui 1.92.9b-docking");
static_assert(ImGuiConfigFlags_DockingEnable != 0, "Dear ImGui docking configuration flag is unavailable");
static_assert(requires(ImGuiID a_id) { ImGui::DockSpace(a_id); }, "Dear ImGui DockSpace API is unavailable");

/// @brief Dear ImGui の Version と Docking 契約が Compile／Link できることを終了 Code で示す
int main() noexcept
{
    return 0;
}
