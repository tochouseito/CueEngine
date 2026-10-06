#pragma once

#include <mutex>

namespace cue
{
/// @brief TLS 化されていない公式 GImGui を全 Editor Context 間で直列化する
inline std::recursive_mutex &imgui_context_mutex()
{
    // Context の所有者は Manager のままにし、公式の共有状態を操作する期間だけ排他する
    static std::recursive_mutex mutex;
    return mutex;
}
} // namespace cue
