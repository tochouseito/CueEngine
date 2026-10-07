#pragma once

#include <memory>
#include <utility>

#include <RHI/Command.h>

namespace cue
{
/// @brief 一つの提出完了点を複数 Resource と Graph の返却 Token で共有する
///
/// set は公開前に一度だけ呼ぶ。公開後は共有された元 Token と同じ同期契約に従う
class SharedCommandCompletion final : public ICommandCompletion
{
  public:
    /// @brief 元 Token の割当を提出前に準備できる空の共有 Wrapper を作る
    SharedCommandCompletion() = default;

    /// @brief 公開前に成功した提出の完了 Token を一意所有へ移す
    void set(commandCompletion a_completion) noexcept
    {
        m_completion = std::move(a_completion);
    }

    /// @brief 元の提出先 Queue 種類を返す
    [[nodiscard]] QueueType type() const noexcept override
    {
        return m_completion ? m_completion->type() : QueueType::Graphics;
    }

    /// @brief 元の提出先 Timeline を返す
    [[nodiscard]] std::uint64_t queue_identity() const noexcept override
    {
        return m_completion ? m_completion->queue_identity() : 0;
    }

    /// @brief 元の提出と同時に発行した Fence 値を返す
    [[nodiscard]] std::uint64_t fence_value() const noexcept override
    {
        return m_completion ? m_completion->fence_value() : 0;
    }

    /// @brief 元 Token が正常完了したか確認する
    [[nodiscard]] bool is_complete() const noexcept override
    {
        return m_completion && m_completion->is_complete();
    }

    /// @brief 元 Token の完了を待ち、未設定なら公開前状態の誤用を返す
    [[nodiscard]] Result<void> wait() override
    {
        return m_completion
                   ? m_completion->wait()
                   : Result<void>::failure({ErrorCategory::InvalidState, "SharedCommandCompletion.wait.empty"});
    }

  private:
    commandCompletion m_completion;
};
} // namespace cue
