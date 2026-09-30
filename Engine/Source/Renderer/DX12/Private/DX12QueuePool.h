#pragma once

#include "DX12GpuCommandQueue.h"

#include <Cue/Renderer/RHI/Queue.h>

#include <array>
#include <memory>
#include <vector>

namespace cue::detail
{
/// @brief Device の Graphics、Compute、Copy Queue と Fence を一括所有する
class DX12QueuePool final : public IQueuePool
{
public:
    /// @brief 三種類の Queue をすべて生成してから公開する
    [[nodiscard]] static Result<std::unique_ptr<DX12QueuePool>> create(DX12RenderDevice& a_device);

    /// @brief Queue Pool の生存中だけ指定 Queue を借用する
    [[nodiscard]] DX12GpuCommandQueue& context(GpuQueueType a_type) noexcept override;

    [[nodiscard]] Result<QueueContextLease> acquire_context(GpuQueueType a_type) override;
    [[nodiscard]] Result<IQueueContext*> context(QueueContextLease a_lease) override;
    [[nodiscard]] Result<void> return_context(QueueContextLease a_lease) override;
    [[nodiscard]] Result<GpuFencePoint> signal_context(QueueContextLease a_lease) override;

    /// @brief Swap Chain と Back Buffer 描画に共用する Direct Queue を返す
    [[nodiscard]] IQueueContext& present_context() noexcept override;

    /// @brief Graphics Queue の先行処理を CPU で待つ
    [[nodiscard]] Result<void> wait_for_graphics_queue() override;

    /// @brief 指定 Queue の先行処理に Fence を発行する
    [[nodiscard]] Result<GpuFencePoint> signal(GpuQueueType a_queue) override;

    /// @brief 発行元 Queue の Fence を CPU で待つ
    [[nodiscard]] Result<void> wait_cpu(GpuFencePoint a_point) override;

    /// @brief Queue 間の待機を GPU に記録する
    [[nodiscard]] Result<void> wait_gpu(GpuQueueType a_waitingQueue, GpuFencePoint a_point) override;

    /// @brief 全 Queue の先行処理を完了させる
    [[nodiscard]] Result<void> wait_idle() override;

private:
    /// @brief 無効な enum 値を配列へ使わないための検査
    [[nodiscard]] static bool is_valid(GpuQueueType a_type) noexcept;

    struct LeaseState final
    {
        bool isLeased = false;
        std::uint64_t generation = 0;
    };

    [[nodiscard]] bool owns(QueueContextLease a_lease) const noexcept;

    std::array<std::vector<std::unique_ptr<DX12GpuCommandQueue>>, 3> m_contexts;
    std::array<std::vector<LeaseState>, 3> m_leases;
};
} // namespace cue::detail
