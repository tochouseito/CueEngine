#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include <RHI/Command.h>

namespace cue
{
/// @brief 完了 Token を最大 8 件保持し、Frame 数で GPU への先行を制限する
///
/// Host が所有し、Render が操作を直列化する。待機中も Token を保持する
/// Frame 全 Queue の合流 Completion を登録する。CPU 完了や Present 成功を代用しない
class GpuFrameQueue final
{
  public:
    /// @brief 確保済み固定容量の内側で未完了上限を指定する
    explicit GpuFrameQueue(std::uint32_t a_limit = 2) noexcept : m_limit(a_limit)
    {
    }

    /// @brief 完了済み Token を回収し、上限到達時だけ通知可能な待機を呼ぶ
    /// a_wait は短時間で戻り、a_shouldCancel を再確認できるようにする
    [[nodiscard]] Result<bool> wait_for_slot(const std::function<void()> &a_wait,
                                            const std::function<bool()> &a_shouldCancel)
    {
        if (m_limit == 0 || m_limit > m_entries.size() || !a_wait || !a_shouldCancel)
            return Result<bool>::failure({ErrorCategory::InvalidArgument, "GpuFrameQueue.config"});
        while (true)
        {
            if (a_shouldCancel())
                return Result<bool>::success(false);
            // 全 Token の検証が成功するまで所有参照の配置を変更しない
            std::array<bool, 8> completed{};
            for (std::uint32_t index = 0; index < m_count; ++index)
            {
                auto status = m_entries[index]->completion_status();
                if (!status.has_value())
                    return Result<bool>::failure(*status.try_error());
                completed[index] = *status.try_value();
            }
            std::uint32_t remaining = 0;
            for (std::uint32_t index = 0; index < m_count; ++index)
            {
                if (!completed[index])
                {
                    if (remaining != index)
                        m_entries[remaining] = std::move(m_entries[index]);
                    ++remaining;
                }
            }
            for (auto index = remaining; index < m_count; ++index)
                m_entries[index].reset();
            m_count = remaining;
            if (m_count < m_limit)
                return Result<bool>::success(true);
            a_wait();
        }
    }

    /// @brief 提出前に空きを確保した Frame の完了点を割当なしで保持する
    [[nodiscard]] Result<void> submitted(std::shared_ptr<ICommandCompletion> a_completion)
    {
        if (!a_completion || m_count >= m_limit || m_count >= m_entries.size())
            return Result<void>::failure({ErrorCategory::InvalidState, "GpuFrameQueue.submitted"});
        m_entries[m_count++] = std::move(a_completion);
        return Result<void>::success();
    }

    /// @brief 未完了 Frame 数を操作する Render Thread から取得する
    [[nodiscard]] std::uint32_t pending() const noexcept
    {
        return m_count;
    }

  private:
    std::array<std::shared_ptr<ICommandCompletion>, 8> m_entries;
    std::uint32_t m_limit;
    std::uint32_t m_count = 0;
};
} // namespace cue
