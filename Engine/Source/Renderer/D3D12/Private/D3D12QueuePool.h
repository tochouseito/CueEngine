#pragma once

#include "D3D12QueueContext.h"

#include <Cue/Renderer/RHI/GpuExecution.h>

#include <array>
#include <memory>

namespace cue::detail
{
/// @brief Device の Graphics、Compute、Copy Queue と Fence を一括所有する
class D3D12QueuePool final : public IGpuExecution
{
public:
    /// @brief 三種類の Queue をすべて生成してから公開する
    [[nodiscard]] static Result<std::unique_ptr<D3D12QueuePool>> create(D3D12DeviceContext& a_device);

    /// @brief Queue Pool の生存中だけ指定 Queue を借用する
    [[nodiscard]] D3D12QueueContext& context(GpuQueueType a_type) noexcept;

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

    std::array<std::unique_ptr<D3D12QueueContext>, 3> m_contexts;
};
} // namespace cue::detail
