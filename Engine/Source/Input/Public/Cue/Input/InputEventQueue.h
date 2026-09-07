#pragma once

#include <Cue/Input/InputEvent.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace cue
{
/// @brief Owner Thread上でNative AdapterとFrame構築を分離する固定容量FIFO Queue
///
/// Queue Overflow時は保持中Eventを破棄してDeviceResetを先頭へ置き、入力押下状態の残留を防ぐ
/// 全操作は同じOwner Threadから呼び、並行Accessしない
class InputEventQueue final
{
  public:
    static constexpr std::size_t k_capacity = 512;

    /// @brief 空のQueueを生成する
    InputEventQueue() noexcept = default;
    /// @brief Session-local Queueの複製を禁止する
    InputEventQueue(const InputEventQueue &) = delete;
    /// @brief Session-local Queueの複製代入を禁止する
    InputEventQueue &operator=(const InputEventQueue &) = delete;
    /// @brief 値だけを保持するQueueを破棄する
    ~InputEventQueue() noexcept = default;

    /// @brief EventをFIFO末尾へ追加し、Overflowなしで保持できた場合にtrueを返す
    /// @details Overflow時もDeviceResetと今回Eventを順に保持し、falseを返す
    [[nodiscard]] bool push(InputEvent a_event) noexcept;
    /// @brief FIFO先頭Eventを取得し、空の場合は出力を変更せずfalseを返す
    [[nodiscard]] bool try_pop(InputEvent &a_event) noexcept;
    /// @brief 現在保持するEvent数を返す
    [[nodiscard]] std::size_t size() const noexcept;
    /// @brief Queue Overflowが発生した累積回数を返す
    [[nodiscard]] std::uint64_t overflow_count() const noexcept;

  private:
    std::array<InputEvent, k_capacity> m_events = {};
    std::size_t m_readIndex = 0;
    std::size_t m_count = 0;
    std::uint64_t m_overflowCount = 0;
};
} // namespace cue
