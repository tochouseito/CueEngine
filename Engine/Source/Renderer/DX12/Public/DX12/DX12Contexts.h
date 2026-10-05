#pragma once

namespace cue
{
class ICommandPool;
class IQueuePool;
} // namespace cue

namespace cue::dx12
{
class DX12RenderDevice;
class DX12ViewManager;

/// @brief Resource 生成に必要な Backend Object を非所有で束ねる
///
/// 参照先の Owner が構築し、利用者を停止してから参照先を破棄する
/// 参照の組合せは生成後に変更せず、各 Object の Thread 契約に従う
struct DX12ResourceContext final
{
    DX12RenderDevice &device;
    DX12ViewManager &views;

    /// @brief Context の利用期間中だけ Device を借用する
    [[nodiscard]] DX12RenderDevice &get_render_device() const noexcept
    {
        return device;
    }

    /// @brief Context の利用期間中だけ ViewManager を借用する
    [[nodiscard]] DX12ViewManager &get_view_manager() const noexcept
    {
        return views;
    }
};

/// @brief GPU 実行に必要な Pool を非所有で束ねる
///
/// Owner は実行を停止してから Pool を破棄する。Context 自体は同期や所有を追加しない
struct DX12ExecutionContext final
{
    ICommandPool &commands;
    IQueuePool &queues;

    /// @brief 記録と提出に使う Pool を利用期間中だけ借用する
    [[nodiscard]] ICommandPool &get_command_pool() const noexcept
    {
        return commands;
    }

    /// @brief Queue の貸出に使う Pool を利用期間中だけ借用する
    [[nodiscard]] IQueuePool &get_queue_pool() const noexcept
    {
        return queues;
    }
};
} // namespace cue::dx12
