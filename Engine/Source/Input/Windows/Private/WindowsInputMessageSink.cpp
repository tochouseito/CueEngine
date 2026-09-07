#include <Cue/Input/Windows/WindowsInputMessageSink.h>

#include <Windows.h>
#include <windowsx.h>

#include <limits>
#include <optional>

namespace
{
/// @brief Windows Virtual-Keyを上位へ数値を漏らさずPortable Keyへ変換する
[[nodiscard]] std::optional<cue::InputKey> translate_key(std::uintptr_t a_key) noexcept
{
    const UINT key = static_cast<UINT>(a_key);
    if (key >= 'A' && key <= 'Z')
    {
        return static_cast<cue::InputKey>(static_cast<unsigned int>(cue::InputKey::A) + key - 'A');
    }
    if (key >= '0' && key <= '9')
    {
        return static_cast<cue::InputKey>(static_cast<unsigned int>(cue::InputKey::Digit0) + key - '0');
    }

    switch (key)
    {
    case VK_ESCAPE:
        return cue::InputKey::Escape;
    case VK_RETURN:
        return cue::InputKey::Enter;
    case VK_TAB:
        return cue::InputKey::Tab;
    case VK_BACK:
        return cue::InputKey::Backspace;
    case VK_SPACE:
        return cue::InputKey::Space;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return cue::InputKey::Shift;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return cue::InputKey::Control;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return cue::InputKey::Alt;
    case VK_LEFT:
        return cue::InputKey::Left;
    case VK_RIGHT:
        return cue::InputKey::Right;
    case VK_UP:
        return cue::InputKey::Up;
    case VK_DOWN:
        return cue::InputKey::Down;
    case VK_INSERT:
        return cue::InputKey::Insert;
    case VK_DELETE:
        return cue::InputKey::Delete;
    case VK_HOME:
        return cue::InputKey::Home;
    case VK_END:
        return cue::InputKey::End;
    case VK_PRIOR:
        return cue::InputKey::PageUp;
    case VK_NEXT:
        return cue::InputKey::PageDown;
    case VK_F1:
        return cue::InputKey::F1;
    case VK_F2:
        return cue::InputKey::F2;
    case VK_F3:
        return cue::InputKey::F3;
    case VK_F4:
        return cue::InputKey::F4;
    case VK_F5:
        return cue::InputKey::F5;
    case VK_F6:
        return cue::InputKey::F6;
    case VK_F7:
        return cue::InputKey::F7;
    case VK_F8:
        return cue::InputKey::F8;
    case VK_F9:
        return cue::InputKey::F9;
    case VK_F10:
        return cue::InputKey::F10;
    case VK_F11:
        return cue::InputKey::F11;
    case VK_F12:
        return cue::InputKey::F12;
    case VK_CAPITAL:
        return cue::InputKey::CapsLock;
    case VK_OEM_MINUS:
        return cue::InputKey::Minus;
    case VK_OEM_PLUS:
        return cue::InputKey::Equal;
    case VK_OEM_4:
        return cue::InputKey::LeftBracket;
    case VK_OEM_6:
        return cue::InputKey::RightBracket;
    case VK_OEM_5:
        return cue::InputKey::Backslash;
    case VK_OEM_1:
        return cue::InputKey::Semicolon;
    case VK_OEM_7:
        return cue::InputKey::Apostrophe;
    case VK_OEM_3:
        return cue::InputKey::Grave;
    case VK_OEM_COMMA:
        return cue::InputKey::Comma;
    case VK_OEM_PERIOD:
        return cue::InputKey::Period;
    case VK_OEM_2:
        return cue::InputKey::Slash;
    default:
        return std::nullopt;
    }
}

/// @brief Mouse Messageの符号付きClient座標をPortable値へ変換する
[[nodiscard]] cue::InputPoint client_point(std::intptr_t a_longParameter) noexcept
{
    const LPARAM value = static_cast<LPARAM>(a_longParameter);
    return {GET_X_LPARAM(value), GET_Y_LPARAM(value)};
}

/// @brief XButton識別値をPortable Mouse Buttonへ変換する
[[nodiscard]] std::optional<cue::InputMouseButton> translate_x_button(std::uintptr_t a_wordParameter) noexcept
{
    const WORD button = GET_XBUTTON_WPARAM(static_cast<WPARAM>(a_wordParameter));
    if (button == XBUTTON1)
    {
        return cue::InputMouseButton::X1;
    }
    if (button == XBUTTON2)
    {
        return cue::InputMouseButton::X2;
    }
    return std::nullopt;
}
} // namespace

namespace cue
{
WindowsInputMessageSink::WindowsInputMessageSink(InputEventQueue &a_queue, WindowsMessageSink *a_downstream) noexcept
    : m_queue(&a_queue), m_downstream(a_downstream)
{
}

WindowsMessageResult WindowsInputMessageSink::process_message(const WindowsMessageView &a_message) noexcept
{
    const UINT message = static_cast<UINT>(a_message.message);
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP)
    {
        const std::optional<InputKey> key = translate_key(a_message.wordParameter);
        if (key)
        {
            InputEventType type = InputEventType::KeyUp;
            if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
            {
                constexpr std::uintptr_t k_previousStateMask = static_cast<std::uintptr_t>(1) << 30;
                const bool wasDown = (static_cast<std::uintptr_t>(a_message.longParameter) & k_previousStateMask) != 0;
                type = wasDown ? InputEventType::KeyRepeat : InputEventType::KeyDown;
            }
            enqueue({type, *key});
        }
    }
    else if (message == WM_MOUSEMOVE)
    {
        enqueue(
            {InputEventType::MouseMove, InputKey::None, InputMouseButton::None, client_point(a_message.longParameter)});
    }
    else if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_RBUTTONDOWN ||
             message == WM_RBUTTONUP || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP)
    {
        InputMouseButton button = InputMouseButton::Left;
        if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
        {
            button = InputMouseButton::Right;
        }
        else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP)
        {
            button = InputMouseButton::Middle;
        }
        const bool isDown = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
        enqueue({isDown ? InputEventType::MouseButtonDown : InputEventType::MouseButtonUp, InputKey::None, button,
                 client_point(a_message.longParameter)});
    }
    else if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP)
    {
        const std::optional<InputMouseButton> button = translate_x_button(a_message.wordParameter);
        if (button)
        {
            enqueue({message == WM_XBUTTONDOWN ? InputEventType::MouseButtonDown : InputEventType::MouseButtonUp,
                     InputKey::None, *button, client_point(a_message.longParameter)});
        }
    }
    else if (message == WM_MOUSEWHEEL)
    {
        POINT point = {GET_X_LPARAM(static_cast<LPARAM>(a_message.longParameter)),
                       GET_Y_LPARAM(static_cast<LPARAM>(a_message.longParameter))};
        HWND window = static_cast<HWND>(const_cast<void *>(a_message.nativeWindow));
        if (window == nullptr || ScreenToClient(window, &point) == FALSE)
        {
            record_conversion_failure();
        }
        else
        {
            enqueue({InputEventType::MouseWheel,
                     InputKey::None,
                     InputMouseButton::None,
                     {static_cast<std::int32_t>(point.x), static_cast<std::int32_t>(point.y)},
                     static_cast<std::int32_t>(GET_WHEEL_DELTA_WPARAM(static_cast<WPARAM>(a_message.wordParameter)))});
        }
    }
    else if (message == WM_SETFOCUS)
    {
        enqueue({InputEventType::FocusGained});
    }
    else if (message == WM_KILLFOCUS)
    {
        enqueue({InputEventType::FocusLost});
    }
    else if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED)
    {
        enqueue({InputEventType::DeviceReset});
    }

    return m_downstream != nullptr ? m_downstream->process_message(a_message) : WindowsMessageResult{false, 0};
}

std::uint64_t WindowsInputMessageSink::conversion_failure_count() const noexcept
{
    return m_conversionFailureCount;
}

void WindowsInputMessageSink::enqueue(InputEvent a_event) noexcept
{
    static_cast<void>(m_queue->push(a_event));
}

void WindowsInputMessageSink::record_conversion_failure() noexcept
{
    if (m_conversionFailureCount != std::numeric_limits<std::uint64_t>::max())
    {
        ++m_conversionFailureCount;
    }
    enqueue({InputEventType::DeviceReset});
}
} // namespace cue
