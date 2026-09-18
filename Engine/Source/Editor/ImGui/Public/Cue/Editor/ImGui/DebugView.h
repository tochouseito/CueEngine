#pragma once

#include <cstdint>

namespace cue::editor
{
/// @brief ToolHost所有TextureをDebugViewへ渡す非所有値
struct DebugViewSurface final
{
    std::uint64_t textureId = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

/// @brief DebugViewが次Frameに必要とする描画領域
struct DebugViewRequest final
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    bool isVisible = false;
};

/// @brief DebugCameraのOffscreen Textureを表示し次Frameの描画寸法を返す
[[nodiscard]] DebugViewRequest draw_debug_view(DebugViewSurface a_surface) noexcept;
} // namespace cue::editor
