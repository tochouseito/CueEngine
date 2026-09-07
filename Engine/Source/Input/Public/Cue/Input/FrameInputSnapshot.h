#pragma once

#include <Cue/Input/InputEvent.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace cue
{
class InputState;

/// @brief UIが現在FrameでKeyboardとMouseを占有するか示すPortable Routing値
struct InputCapture final
{
    bool isKeyboardCaptured = false;
    bool isMouseCaptured = false;
};

/// @brief 一Frame中は不変としてRuntime Systemへ渡すPortable Input状態
///
/// 返却参照は所有InputStateの次回begin_frameまたは破棄まで有効であり、別Threadへ保持しない
class FrameInputSnapshot final
{
  public:
    /// @brief 入力を持たない初期Frame値を生成する
    FrameInputSnapshot() noexcept = default;
    /// @brief Frame値を独立した所有Snapshotへ複製する
    FrameInputSnapshot(const FrameInputSnapshot &) = default;
    /// @brief Frame値を独立した所有Snapshotへ複製代入する
    FrameInputSnapshot &operator=(const FrameInputSnapshot &) = default;
    /// @brief 値だけを保持するSnapshotを破棄する
    ~FrameInputSnapshot() noexcept = default;

    /// @brief Window Focusがある場合にtrueを返す
    [[nodiscard]] bool has_focus() const noexcept;
    /// @brief UIがKeyboard EventをPlay側から遮断している場合にtrueを返す
    [[nodiscard]] bool is_keyboard_captured() const noexcept;
    /// @brief UIがMouse EventをPlay側から遮断している場合にtrueを返す
    [[nodiscard]] bool is_mouse_captured() const noexcept;
    /// @brief 指定Keyが現在押下中の場合にtrueを返す
    [[nodiscard]] bool is_key_down(InputKey a_key) const noexcept;
    /// @brief 指定Keyが現在Frameで新しく押された場合にtrueを返す
    [[nodiscard]] bool was_key_pressed(InputKey a_key) const noexcept;
    /// @brief 指定Keyが現在Frameで解放された場合にtrueを返す
    [[nodiscard]] bool was_key_released(InputKey a_key) const noexcept;
    /// @brief 指定Keyの現在Frame中Repeat回数を返す
    [[nodiscard]] std::uint32_t key_repeat_count(InputKey a_key) const noexcept;
    /// @brief 指定Mouse Buttonが現在押下中の場合にtrueを返す
    [[nodiscard]] bool is_mouse_button_down(InputMouseButton a_button) const noexcept;
    /// @brief 指定Mouse Buttonが現在Frameで新しく押された場合にtrueを返す
    [[nodiscard]] bool was_mouse_button_pressed(InputMouseButton a_button) const noexcept;
    /// @brief 指定Mouse Buttonが現在Frameで解放された場合にtrueを返す
    [[nodiscard]] bool was_mouse_button_released(InputMouseButton a_button) const noexcept;
    /// @brief Mouse位置を一度以上受理済みの場合にtrueを返す
    [[nodiscard]] bool has_mouse_position() const noexcept;
    /// @brief 最後に受理したClient Area内Mouse位置を返す
    [[nodiscard]] InputPoint mouse_position() const noexcept;
    /// @brief 現在Frameで蓄積したMouse移動量を返す
    [[nodiscard]] InputDelta mouse_delta() const noexcept;
    /// @brief 現在Frameで蓄積したPortable Wheel Deltaを返す
    [[nodiscard]] std::int32_t mouse_wheel_delta() const noexcept;

  private:
    friend class InputState;

    static constexpr std::size_t k_keyCount = static_cast<std::size_t>(InputKey::Count);
    static constexpr std::size_t k_buttonCount = static_cast<std::size_t>(InputMouseButton::Count);

    std::array<bool, k_keyCount> m_keyDown = {};
    std::array<bool, k_keyCount> m_keyPressed = {};
    std::array<bool, k_keyCount> m_keyReleased = {};
    std::array<std::uint32_t, k_keyCount> m_keyRepeatCount = {};
    std::array<bool, k_buttonCount> m_buttonDown = {};
    std::array<bool, k_buttonCount> m_buttonPressed = {};
    std::array<bool, k_buttonCount> m_buttonReleased = {};
    InputPoint m_mousePosition = {};
    InputDelta m_mouseDelta = {};
    std::int32_t m_mouseWheelDelta = 0;
    bool m_hasMousePosition = false;
    bool m_hasFocus = true;
    InputCapture m_capture = {};
};
} // namespace cue
