#pragma once

#include <Platform/Clock.h>

namespace cue
{
class ICommandPool;
class IQueuePool;
} // namespace cue

namespace cue::dx12
{
class DX12RenderDevice;
class DX12ViewManager;
class DX12PipelineManager;

/// @brief Resource 生成に必要な Backend Object を非所有で束ねる
///
/// 参照先の Owner が構築し、利用者を停止してから参照先を破棄する
/// 参照の組合せは生成後に変更せず、各 Object の Thread 契約に従う
struct DX12ResourceContext final
{
    // Native 資源を作る Device を Backend の生存中だけ借用する
    DX12RenderDevice &device;
    // RTV / SRV の予約と書込みを担当する Manager を借用する
    DX12ViewManager &views;
    // Shader・Root・PSO の生成と寿命を担当する Manager を借用する
    DX12PipelineManager &pipelines;
    // 指定時は他の Context 参照先と同じ期間借用する
    Clock &clock = get_default_clock();

    /// @brief CPU 計測に使う共通の単調時計を借用する
    [[nodiscard]] Clock &get_clock() const noexcept
    {
        return clock;
    }

    /// @brief Graph の生成物を所有する Manager を借用する
    [[nodiscard]] DX12PipelineManager &get_pipeline_manager() const noexcept
    {
        return pipelines;
    }

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
    // 記録と提出に使う Command の貸出元
    ICommandPool &commands;
    // Graphics・Compute・Copy Queue の貸出元
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
