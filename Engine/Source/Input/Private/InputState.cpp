#include <Cue/Input/InputState.h>

#include <algorithm>
#include <limits>

namespace
{
/// @brief Keyが状態配列へ安全に変換できるPortable値か判定する
[[nodiscard]] bool is_valid_key(cue::InputKey a_key) noexcept
{
    return a_key > cue::InputKey::None && a_key < cue::InputKey::Count;
}

/// @brief Mouse Buttonが状態配列へ安全に変換できるPortable値か判定する
[[nodiscard]] bool is_valid_button(cue::InputMouseButton a_button) noexcept
{
    return a_button > cue::InputMouseButton::None && a_button < cue::InputMouseButton::Count;
}

/// @brief Keyを検証済み状態配列Indexへ変換する
[[nodiscard]] std::size_t key_index(cue::InputKey a_key) noexcept
{
    return static_cast<std::size_t>(a_key);
}

/// @brief Mouse Buttonを検証済み状態配列Indexへ変換する
[[nodiscard]] std::size_t button_index(cue::InputMouseButton a_button) noexcept
{
    return static_cast<std::size_t>(a_button);
}

/// @brief 符号付きInput DeltaをOverflowさせずに加算する
[[nodiscard]] std::int32_t saturating_add(std::int32_t a_left, std::int64_t a_right) noexcept
{
    const std::int64_t value = static_cast<std::int64_t>(a_left) + static_cast<std::int64_t>(a_right);
    return static_cast<std::int32_t>(std::clamp(value,
                                                static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
                                                static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

/// @brief Repeat回数をWrapさせずに一回増やす
void increment_repeat(std::uint32_t &a_count) noexcept
{
    if (a_count != std::numeric_limits<std::uint32_t>::max())
    {
        ++a_count;
    }
}
} // namespace

namespace cue
{
bool FrameInputSnapshot::has_focus() const noexcept
{
    return m_hasFocus;
}

bool FrameInputSnapshot::is_keyboard_captured() const noexcept
{
    return m_capture.isKeyboardCaptured;
}

bool FrameInputSnapshot::is_mouse_captured() const noexcept
{
    return m_capture.isMouseCaptured;
}

bool FrameInputSnapshot::is_key_down(InputKey a_key) const noexcept
{
    return is_valid_key(a_key) && m_keyDown[key_index(a_key)];
}

bool FrameInputSnapshot::was_key_pressed(InputKey a_key) const noexcept
{
    return is_valid_key(a_key) && m_keyPressed[key_index(a_key)];
}

bool FrameInputSnapshot::was_key_released(InputKey a_key) const noexcept
{
    return is_valid_key(a_key) && m_keyReleased[key_index(a_key)];
}

std::uint32_t FrameInputSnapshot::key_repeat_count(InputKey a_key) const noexcept
{
    return is_valid_key(a_key) ? m_keyRepeatCount[key_index(a_key)] : 0;
}

bool FrameInputSnapshot::is_mouse_button_down(InputMouseButton a_button) const noexcept
{
    return is_valid_button(a_button) && m_buttonDown[button_index(a_button)];
}

bool FrameInputSnapshot::was_mouse_button_pressed(InputMouseButton a_button) const noexcept
{
    return is_valid_button(a_button) && m_buttonPressed[button_index(a_button)];
}

bool FrameInputSnapshot::was_mouse_button_released(InputMouseButton a_button) const noexcept
{
    return is_valid_button(a_button) && m_buttonReleased[button_index(a_button)];
}

bool FrameInputSnapshot::has_mouse_position() const noexcept
{
    return m_hasMousePosition;
}

InputPoint FrameInputSnapshot::mouse_position() const noexcept
{
    return m_mousePosition;
}

InputDelta FrameInputSnapshot::mouse_delta() const noexcept
{
    return m_mouseDelta;
}

std::int32_t FrameInputSnapshot::mouse_wheel_delta() const noexcept
{
    return m_mouseWheelDelta;
}

void InputState::begin_frame(InputCapture a_capture) noexcept
{
    m_snapshot.m_keyPressed.fill(false);
    m_snapshot.m_keyReleased.fill(false);
    m_snapshot.m_keyRepeatCount.fill(0);
    m_snapshot.m_buttonPressed.fill(false);
    m_snapshot.m_buttonReleased.fill(false);
    m_snapshot.m_mouseDelta = {};
    m_snapshot.m_mouseWheelDelta = 0;

    if (a_capture.isKeyboardCaptured && !m_snapshot.m_capture.isKeyboardCaptured)
    {
        release_keyboard();
    }
    if (a_capture.isMouseCaptured && !m_snapshot.m_capture.isMouseCaptured)
    {
        release_mouse();
        reset_mouse_baseline();
    }

    m_snapshot.m_capture = a_capture;
}

void InputState::apply_event(const InputEvent &a_event) noexcept
{
    if (a_event.type == InputEventType::FocusGained)
    {
        m_snapshot.m_hasFocus = true;
        reset_mouse_baseline();
        return;
    }
    if (a_event.type == InputEventType::FocusLost)
    {
        release_keyboard();
        release_mouse();
        reset_mouse_baseline();
        m_snapshot.m_hasFocus = false;
        return;
    }
    if (a_event.type == InputEventType::DeviceReset)
    {
        release_keyboard();
        release_mouse();
        reset_mouse_baseline();
        return;
    }
    if (!m_snapshot.m_hasFocus)
    {
        return;
    }

    if (a_event.type == InputEventType::KeyDown || a_event.type == InputEventType::KeyUp ||
        a_event.type == InputEventType::KeyRepeat)
    {
        if (m_snapshot.m_capture.isKeyboardCaptured || !is_valid_key(a_event.key))
        {
            return;
        }

        const std::size_t index = key_index(a_event.key);
        if (a_event.type == InputEventType::KeyDown)
        {
            if (!m_snapshot.m_keyDown[index])
            {
                m_snapshot.m_keyPressed[index] = true;
            }
            m_snapshot.m_keyDown[index] = true;
        }
        else if (a_event.type == InputEventType::KeyUp)
        {
            if (m_snapshot.m_keyDown[index])
            {
                m_snapshot.m_keyReleased[index] = true;
            }
            m_snapshot.m_keyDown[index] = false;
        }
        else if (m_snapshot.m_keyDown[index])
        {
            increment_repeat(m_snapshot.m_keyRepeatCount[index]);
        }
        return;
    }

    if (a_event.type == InputEventType::MouseMove || a_event.type == InputEventType::MouseButtonDown ||
        a_event.type == InputEventType::MouseButtonUp || a_event.type == InputEventType::MouseWheel)
    {
        if (m_snapshot.m_capture.isMouseCaptured)
        {
            reset_mouse_baseline();
            return;
        }

        update_mouse_position(a_event.mousePosition);

        if (a_event.type == InputEventType::MouseWheel)
        {
            m_snapshot.m_mouseWheelDelta = saturating_add(m_snapshot.m_mouseWheelDelta, a_event.wheelDelta);
            return;
        }
        if (a_event.type == InputEventType::MouseMove || !is_valid_button(a_event.mouseButton))
        {
            return;
        }

        const std::size_t index = button_index(a_event.mouseButton);
        if (a_event.type == InputEventType::MouseButtonDown)
        {
            if (!m_snapshot.m_buttonDown[index])
            {
                m_snapshot.m_buttonPressed[index] = true;
            }
            m_snapshot.m_buttonDown[index] = true;
        }
        else
        {
            if (m_snapshot.m_buttonDown[index])
            {
                m_snapshot.m_buttonReleased[index] = true;
            }
            m_snapshot.m_buttonDown[index] = false;
        }
    }
}

const FrameInputSnapshot &InputState::snapshot() const noexcept
{
    return m_snapshot;
}

void InputState::release_keyboard() noexcept
{
    for (std::size_t index = 0; index < m_snapshot.m_keyDown.size(); ++index)
    {
        if (m_snapshot.m_keyDown[index])
        {
            m_snapshot.m_keyDown[index] = false;
            m_snapshot.m_keyReleased[index] = true;
        }
    }
}

void InputState::release_mouse() noexcept
{
    for (std::size_t index = 0; index < m_snapshot.m_buttonDown.size(); ++index)
    {
        if (m_snapshot.m_buttonDown[index])
        {
            m_snapshot.m_buttonDown[index] = false;
            m_snapshot.m_buttonReleased[index] = true;
        }
    }
}

void InputState::reset_mouse_baseline() noexcept
{
    m_snapshot.m_hasMousePosition = false;
    m_snapshot.m_mouseDelta = {};
}

void InputState::update_mouse_position(InputPoint a_position) noexcept
{
    if (m_snapshot.m_hasMousePosition)
    {
        m_snapshot.m_mouseDelta.x = saturating_add(m_snapshot.m_mouseDelta.x, static_cast<std::int64_t>(a_position.x) -
                                                                                  m_snapshot.m_mousePosition.x);
        m_snapshot.m_mouseDelta.y = saturating_add(m_snapshot.m_mouseDelta.y, static_cast<std::int64_t>(a_position.y) -
                                                                                  m_snapshot.m_mousePosition.y);
    }

    m_snapshot.m_mousePosition = a_position;
    m_snapshot.m_hasMousePosition = true;
}
} // namespace cue
