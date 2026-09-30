#pragma once

#include <cstdint>

namespace cue
{
/// @brief Windowの描画領域をPixel単位で表す
struct WindowSize final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// @brief HostがWindowの継続と表示状態を判断するためのEvent種別
enum class WindowEventType
{
    CloseRequested,
    Resized,
    Minimized,
    Restored,
    Destroyed,
};

/// @brief Window ProcedureからHostへ渡す所有値のEvent
struct WindowEvent final
{
    WindowEventType type;
    WindowSize clientSize;
};
} // namespace cue
