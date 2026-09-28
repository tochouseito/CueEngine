#pragma once

#include <Cue/Foundation/Result.h>

#include <cstdint>

namespace cue
{
enum class GpuQueueType : std::uint8_t
{
    Graphics,
    Compute,
    Copy
};

class IGpuExecution;

/// @brief Queue の時系列上の位置を表し、発行元の実行基盤が生存する間だけ有効
struct GpuFencePoint final
{
    GpuQueueType queue = GpuQueueType::Graphics;
    std::uint64_t value = 0;
    const IGpuExecution* owner = nullptr;
};

/// @brief GPU Queue と Fence の同期契約。実装と Device は呼出側より長く生存させる
/// @details Render Thread から順番に呼び、同時呼出や再入は行わない。失敗時は結果を伝播し、GPU 待機を成功扱いしない
class IGpuExecution
{
public:
    virtual ~IGpuExecution() = default;

    /// @brief 指定 Queue の先行コマンドの完了位置を記録する
    [[nodiscard]] virtual Result<GpuFencePoint> signal(GpuQueueType a_queue) = 0;

    /// @brief CPU で発行済み Fence の完了を待つ
    [[nodiscard]] virtual Result<void> wait_cpu(GpuFencePoint a_point) = 0;

    /// @brief 指定 Queue に別 Queue の発行済み Fence 待機を追加する
    [[nodiscard]] virtual Result<void> wait_gpu(GpuQueueType a_waitingQueue, GpuFencePoint a_point) = 0;

    /// @brief 全 Queue の先行コマンド完了を CPU で待つ
    [[nodiscard]] virtual Result<void> wait_idle() = 0;
};
} // namespace cue
