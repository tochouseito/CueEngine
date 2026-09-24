#pragma once

#include <cstdint>

namespace cue
{
struct WindowSize final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

enum class WindowEventType
{
    CloseRequested,
    Resized,
    Minimized,
    Restored,
    Destroyed,
};

struct WindowEvent final
{
    WindowEventType type;
    WindowSize clientSize;
};
} // namespace cue
