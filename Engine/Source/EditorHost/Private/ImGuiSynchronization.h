#pragma once

#include <mutex>

#include <imgui.h>

namespace cue
{
/// @brief TLS 化されていない公式 GImGui を全 Editor Context 間で直列化する
inline std::recursive_mutex &imgui_context_mutex()
{
    // Context の所有者は Manager のままにし、公式の共有状態を操作する期間だけ排他する
    static std::recursive_mutex mutex;
    return mutex;
}

/// @brief Callback の非所有 UserData を Worker に持ち出さず、公式 State 切替だけを許可する
inline bool is_imgui_draw_callback_supported(ImDrawCallback a_callback, const ImGuiPlatformIO &a_platform) noexcept
{
    return !a_callback || a_callback == ImDrawCallback_ResetRenderState ||
           a_callback == a_platform.DrawCallback_ResetRenderState ||
           a_callback == a_platform.DrawCallback_SetSamplerLinear ||
           a_callback == a_platform.DrawCallback_SetSamplerNearest;
}
} // namespace cue
