#pragma once

#include <Cue/Input/FrameInputSnapshot.h>

namespace cue
{
/// @brief FIFO EventからSession-localなFrame Snapshotを決定的に構築する状態Owner
///
/// 全操作は同じOwner Threadから呼び、Snapshotを次回begin_frame以降へ保持しない
class InputState final
{
  public:
    /// @brief Focusを持つ空のPlay入力状態を生成する
    InputState() noexcept = default;
    /// @brief Session-local入力状態の複製を禁止する
    InputState(const InputState &) = delete;
    /// @brief Session-local入力状態の複製代入を禁止する
    InputState &operator=(const InputState &) = delete;
    /// @brief 値だけを保持する入力状態を破棄する
    ~InputState() noexcept = default;

    /// @brief Frame一時値をResetし、UI Capture開始時はPlay側押下状態を解放する
    void begin_frame(InputCapture a_capture) noexcept;
    /// @brief 一つのPortable Eventを現在Frameへ順番どおり適用する
    void apply_event(const InputEvent &a_event) noexcept;
    /// @brief 現在Frameの不変Viewとして利用するSnapshotを返す
    [[nodiscard]] const FrameInputSnapshot &snapshot() const noexcept;

  private:
    /// @brief Keyboard押下状態を解放し、現在FrameのReleaseへ反映する
    void release_keyboard() noexcept;
    /// @brief Mouse Button押下状態を解放し、現在FrameのReleaseへ反映する
    void release_mouse() noexcept;
    /// @brief Mouse位置とDeltaの連続性を切り、再開時の大きなJumpを防ぐ
    void reset_mouse_baseline() noexcept;
    /// @brief Client位置を更新し、連続位置なら現在FrameDeltaへ加算する
    void update_mouse_position(InputPoint a_position) noexcept;

    FrameInputSnapshot m_snapshot = {};
};
} // namespace cue
