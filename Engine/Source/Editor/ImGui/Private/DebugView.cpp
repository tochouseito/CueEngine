#include <Cue/Editor/ImGui/DebugView.h>

#include <Cue/Editor/ImGui/EditorDockspace.h>

#include <algorithm>
#include <cmath>

#include <imgui.h>

namespace
{
constexpr std::uint32_t k_maximumDebugViewDimension = 16384U;

/// @brief ImGuiの利用可能寸法をD3D12 Texture寸法へ安全に変換する
[[nodiscard]] std::uint32_t to_surface_dimension(float a_value) noexcept
{
    if (!std::isfinite(a_value) || a_value < 1.0F)
    {
        return 0U;
    }
    const float clamped = (std::min)(a_value, static_cast<float>(k_maximumDebugViewDimension));
    return static_cast<std::uint32_t>(clamped);
}
} // namespace

namespace cue::editor
{
DebugViewRequest draw_debug_view(DebugViewSurface a_surface) noexcept
{
    dock_editor_window_on_first_use();
    DebugViewRequest request;
    const bool isOpen = ImGui::Begin("Debug View");
    if (isOpen)
    {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        request.width = to_surface_dimension(available.x);
        request.height = to_surface_dimension(available.y);
        request.isVisible = request.width > 0U && request.height > 0U;
        if (a_surface.textureId != 0U && a_surface.width > 0U && a_surface.height > 0U)
        {
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(a_surface.textureId)), available);
        }
        else
        {
            ImGui::TextDisabled("Debug Camera render surface is preparing...");
        }
    }
    ImGui::End();
    return request;
}
} // namespace cue::editor
