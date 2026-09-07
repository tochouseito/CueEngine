#pragma once

#include <cstdint>

namespace cue
{
/// @brief Native Key Codeから独立したRuntime Keyboard Key
enum class InputKey : std::uint8_t
{
    None,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    Escape,
    Enter,
    Tab,
    Backspace,
    Space,
    Shift,
    Control,
    Alt,
    Left,
    Right,
    Up,
    Down,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    CapsLock,
    Minus,
    Equal,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    Grave,
    Comma,
    Period,
    Slash,
    Count,
};

/// @brief Native Button Codeから独立したRuntime Mouse Button
enum class InputMouseButton : std::uint8_t
{
    None,
    Left,
    Right,
    Middle,
    X1,
    X2,
    Count,
};

/// @brief Input EventのPortableな意味分類
enum class InputEventType : std::uint8_t
{
    KeyDown,
    KeyUp,
    KeyRepeat,
    MouseMove,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
    FocusGained,
    FocusLost,
    DeviceReset,
};

/// @brief Window Client Area内の符号付きMouse位置
struct InputPoint final
{
    std::int32_t x = 0;
    std::int32_t y = 0;
};

/// @brief 一Frameで蓄積した符号付きMouse移動量
struct InputDelta final
{
    std::int32_t x = 0;
    std::int32_t y = 0;
};

/// @brief Native Pointerを保持せずQueueへ値で格納できるPortable Input Event
struct InputEvent final
{
    InputEventType type = InputEventType::DeviceReset;
    InputKey key = InputKey::None;
    InputMouseButton mouseButton = InputMouseButton::None;
    InputPoint mousePosition = {};
    std::int32_t wheelDelta = 0;
};
} // namespace cue
