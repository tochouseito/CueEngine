#pragma once

#include <Cue/Input/InputEventQueue.h>
#include <Cue/Platform/Windows/WindowsMessageSink.h>

#include <cstdint>

namespace cue
{
/// @brief Win32 MessageをPortable Inputへ変換して任意のUI Sinkへ順番どおり転送するAdapter
///
/// Queueと任意Downstream Sinkは本Objectが所有せず、本Objectより長く生存させる
/// attach、process_message、破棄はWindowsWindowのOwner Thread上で行う
class WindowsInputMessageSink final : public WindowsMessageSink
{
  public:
    /// @brief Portable Queueと任意の次段Sinkを非所有で関連付ける
    explicit WindowsInputMessageSink(InputEventQueue &a_queue, WindowsMessageSink *a_downstream = nullptr) noexcept;
    /// @brief 非所有参照を解放せずAdapterだけを破棄する
    ~WindowsInputMessageSink() noexcept override = default;

    /// @brief Portable Eventを先にFIFOへ格納し、その後DownstreamのHandled結果を返す
    [[nodiscard]] WindowsMessageResult process_message(const WindowsMessageView &a_message) noexcept override;
    /// @brief Client座標変換失敗を安全なDeviceResetへ変換した累積回数を返す
    [[nodiscard]] std::uint64_t conversion_failure_count() const noexcept;

  private:
    /// @brief Portable EventをQueueへ追加し、Overflow時のReset契約をQueueへ委ねる
    void enqueue(InputEvent a_event) noexcept;
    /// @brief Native変換失敗を累積し、Play側押下状態をResetするEventを追加する
    void record_conversion_failure() noexcept;

    InputEventQueue *m_queue;
    WindowsMessageSink *m_downstream;
    std::uint64_t m_conversionFailureCount = 0;
};
} // namespace cue
