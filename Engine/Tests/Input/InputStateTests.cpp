#include <Cue/Input/InputEventQueue.h>
#include <Cue/Input/InputState.h>

#include <cstddef>

namespace
{
/// @brief Event QueueのFIFO順とOverflow時Resetを検証する
[[nodiscard]] bool test_event_queue() noexcept
{
    cue::InputEventQueue queue;
    if (!queue.push({cue::InputEventType::KeyDown, cue::InputKey::A}) ||
        !queue.push({cue::InputEventType::KeyRepeat, cue::InputKey::A}) ||
        !queue.push({cue::InputEventType::KeyUp, cue::InputKey::A}))
    {
        return false;
    }

    cue::InputEvent event = {};
    if (!queue.try_pop(event) || event.type != cue::InputEventType::KeyDown || event.key != cue::InputKey::A ||
        !queue.try_pop(event) || event.type != cue::InputEventType::KeyRepeat || !queue.try_pop(event) ||
        event.type != cue::InputEventType::KeyUp || queue.try_pop(event))
    {
        return false;
    }

    for (std::size_t index = 0; index < cue::InputEventQueue::k_capacity; ++index)
    {
        if (!queue.push({cue::InputEventType::KeyDown, cue::InputKey::B}))
        {
            return false;
        }
    }
    if (queue.push({cue::InputEventType::KeyUp, cue::InputKey::B}) || queue.overflow_count() != 1 ||
        queue.size() != 3 || !queue.try_pop(event) || event.type != cue::InputEventType::DeviceReset ||
        !queue.try_pop(event) || event.type != cue::InputEventType::FocusGained || !queue.try_pop(event) ||
        event.type != cue::InputEventType::KeyUp || event.key != cue::InputKey::B)
    {
        return false;
    }
    if (queue.try_pop(event) || !queue.push({cue::InputEventType::FocusLost}) || !queue.try_pop(event) ||
        event.type != cue::InputEventType::FocusLost || !queue.push({cue::InputEventType::FocusGained}))
    {
        return false;
    }

    for (std::size_t index = 1; index < cue::InputEventQueue::k_capacity; ++index)
    {
        if (!queue.push({cue::InputEventType::MouseMove}))
        {
            return false;
        }
    }
    if (queue.push({cue::InputEventType::KeyDown, cue::InputKey::C}) || queue.overflow_count() != 2 ||
        !queue.try_pop(event) || event.type != cue::InputEventType::DeviceReset || !queue.try_pop(event) ||
        event.type != cue::InputEventType::FocusGained || !queue.try_pop(event) ||
        event.type != cue::InputEventType::KeyDown || event.key != cue::InputKey::C || queue.try_pop(event))
    {
        return false;
    }

    cue::InputState state;
    state.begin_frame({});
    state.apply_event({cue::InputEventType::FocusLost});
    state.apply_event({cue::InputEventType::DeviceReset});
    state.apply_event({cue::InputEventType::FocusGained});
    state.apply_event({cue::InputEventType::KeyDown, cue::InputKey::C});
    return state.snapshot().has_focus() && state.snapshot().is_key_down(cue::InputKey::C);
}

/// @brief 同一FrameのDown、Repeat、Upと次Frame一時値Resetを検証する
[[nodiscard]] bool test_keyboard_snapshot() noexcept
{
    cue::InputState state;
    state.begin_frame({});
    state.apply_event({cue::InputEventType::KeyDown, cue::InputKey::A});
    state.apply_event({cue::InputEventType::KeyRepeat, cue::InputKey::A});
    state.apply_event({cue::InputEventType::KeyRepeat, cue::InputKey::A});
    state.apply_event({cue::InputEventType::KeyUp, cue::InputKey::A});

    const cue::FrameInputSnapshot &first = state.snapshot();
    if (first.is_key_down(cue::InputKey::A) || !first.was_key_pressed(cue::InputKey::A) ||
        !first.was_key_released(cue::InputKey::A) || first.key_repeat_count(cue::InputKey::A) != 2)
    {
        return false;
    }

    state.begin_frame({});
    const cue::FrameInputSnapshot &second = state.snapshot();
    return !second.was_key_pressed(cue::InputKey::A) && !second.was_key_released(cue::InputKey::A) &&
           second.key_repeat_count(cue::InputKey::A) == 0 && !second.is_key_down(cue::InputKey::None) &&
           !second.is_key_down(static_cast<cue::InputKey>(255));
}

/// @brief Mouse位置、符号付きDelta、Button、WheelをEvent順に反映することを検証する
[[nodiscard]] bool test_mouse_snapshot() noexcept
{
    cue::InputState state;
    state.begin_frame({});
    state.apply_event({cue::InputEventType::MouseMove, cue::InputKey::None, cue::InputMouseButton::None, {10, 20}});
    state.apply_event({cue::InputEventType::MouseMove, cue::InputKey::None, cue::InputMouseButton::None, {-5, 28}});
    state.apply_event(
        {cue::InputEventType::MouseButtonDown, cue::InputKey::None, cue::InputMouseButton::Left, {-5, 28}});
    state.apply_event(
        {cue::InputEventType::MouseWheel, cue::InputKey::None, cue::InputMouseButton::None, {-5, 28}, 120});
    state.apply_event(
        {cue::InputEventType::MouseWheel, cue::InputKey::None, cue::InputMouseButton::None, {-5, 28}, -40});

    const cue::FrameInputSnapshot &snapshot = state.snapshot();
    const cue::InputPoint position = snapshot.mouse_position();
    const cue::InputDelta delta = snapshot.mouse_delta();
    return snapshot.has_mouse_position() && position.x == -5 && position.y == 28 && delta.x == -15 && delta.y == 8 &&
           snapshot.is_mouse_button_down(cue::InputMouseButton::Left) &&
           snapshot.was_mouse_button_pressed(cue::InputMouseButton::Left) && snapshot.mouse_wheel_delta() == 80;
}

/// @brief Focus喪失で押下状態を解放し、Focus復帰まで新規入力を遮断することを検証する
[[nodiscard]] bool test_focus_reset() noexcept
{
    cue::InputState state;
    state.begin_frame({});
    state.apply_event({cue::InputEventType::KeyDown, cue::InputKey::B});
    state.apply_event(
        {cue::InputEventType::MouseButtonDown, cue::InputKey::None, cue::InputMouseButton::Right, {4, 5}});
    state.apply_event({cue::InputEventType::FocusLost});
    state.apply_event({cue::InputEventType::KeyDown, cue::InputKey::C});

    const cue::FrameInputSnapshot &lost = state.snapshot();
    if (lost.has_focus() || lost.is_key_down(cue::InputKey::B) || !lost.was_key_released(cue::InputKey::B) ||
        lost.is_key_down(cue::InputKey::C) || lost.is_mouse_button_down(cue::InputMouseButton::Right) ||
        !lost.was_mouse_button_released(cue::InputMouseButton::Right))
    {
        return false;
    }

    state.begin_frame({});
    state.apply_event({cue::InputEventType::FocusGained});
    state.apply_event({cue::InputEventType::KeyDown, cue::InputKey::C});
    return state.snapshot().has_focus() && state.snapshot().is_key_down(cue::InputKey::C);
}

/// @brief UI Capture開始でPlay入力を解放し、Capture終了後のRepeatで再押下しないことを検証する
[[nodiscard]] bool test_capture_routing() noexcept
{
    cue::InputState state;
    state.begin_frame({});
    state.apply_event({cue::InputEventType::KeyDown, cue::InputKey::D});
    state.apply_event(
        {cue::InputEventType::MouseButtonDown, cue::InputKey::None, cue::InputMouseButton::Middle, {10, 10}});

    state.begin_frame({true, true});
    state.apply_event({cue::InputEventType::KeyRepeat, cue::InputKey::D});
    state.apply_event(
        {cue::InputEventType::MouseButtonDown, cue::InputKey::None, cue::InputMouseButton::Left, {20, 20}});
    const cue::FrameInputSnapshot &captured = state.snapshot();
    if (!captured.is_keyboard_captured() || !captured.is_mouse_captured() || captured.is_key_down(cue::InputKey::D) ||
        !captured.was_key_released(cue::InputKey::D) || captured.is_mouse_button_down(cue::InputMouseButton::Middle) ||
        !captured.was_mouse_button_released(cue::InputMouseButton::Middle) || captured.has_mouse_position())
    {
        return false;
    }

    state.begin_frame({});
    state.apply_event({cue::InputEventType::KeyRepeat, cue::InputKey::D});
    if (state.snapshot().is_key_down(cue::InputKey::D) || state.snapshot().key_repeat_count(cue::InputKey::D) != 0)
    {
        return false;
    }
    state.apply_event({cue::InputEventType::KeyDown, cue::InputKey::D});
    return state.snapshot().is_key_down(cue::InputKey::D) && state.snapshot().was_key_pressed(cue::InputKey::D);
}

/// @brief 二つのInputStateが押下、Focus、Frame一時値を共有しないことを検証する
[[nodiscard]] bool test_session_isolation() noexcept
{
    cue::InputState first;
    cue::InputState second;
    first.begin_frame({});
    second.begin_frame({});
    first.apply_event({cue::InputEventType::KeyDown, cue::InputKey::E});
    first.apply_event({cue::InputEventType::FocusLost});
    second.apply_event({cue::InputEventType::KeyDown, cue::InputKey::F});

    return !first.snapshot().has_focus() && !first.snapshot().is_key_down(cue::InputKey::E) &&
           second.snapshot().has_focus() && second.snapshot().is_key_down(cue::InputKey::F) &&
           !second.snapshot().is_key_down(cue::InputKey::E);
}
} // namespace

/// @brief Portable Input Queue、Frame Snapshot、Focus、UI Routing、Session分離を検証する
int main()
{
    return test_event_queue() && test_keyboard_snapshot() && test_mouse_snapshot() && test_focus_reset() &&
                   test_capture_routing() && test_session_isolation()
               ? 0
               : 1;
}
