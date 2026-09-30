#pragma once

#include <Cue/Renderer/RHI/GpuExecution.h>

#include <cstdint>

namespace cue
{
class IQueuePool;

/// @brief Queue Pool の一回の貸出を世代で識別する
struct QueueContextLease final
{
    GpuQueueType type = GpuQueueType::Graphics;
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
    const IQueuePool* owner = nullptr;
};

/// @brief 一種類の Queue と Fence を操作する契約
/// @details Context と Device は呼出側より長く生存させ、Submit と Fence 操作は直列に行う
class IQueueContext
{
public:
    virtual ~IQueueContext() = default;

    /// @brief Queue に記録できる Command 種別を返す
    [[nodiscard]] virtual GpuQueueType type() const noexcept = 0;

    /// @brief Queue の現在位置を Fence に記録する
    [[nodiscard]] virtual Result<std::uint64_t> signal() = 0;

    /// @brief 発行済み Fence 値まで CPU で待つ
    [[nodiscard]] virtual Result<void> wait_for(std::uint64_t a_value) const = 0;

    /// @brief Queue の先行作業を CPU で待つ
    [[nodiscard]] virtual Result<void> wait_idle() = 0;

    /// @brief 発行済み Fence の GPU 完了を非待機で確認する
    [[nodiscard]] virtual bool is_complete(std::uint64_t a_value) const noexcept = 0;

    /// @brief 別 Queue の現在位置を Signal し、この Queue の後続 GPU 作業を待機させる
    [[nodiscard]] virtual Result<void> wait_for_queue(IQueueContext& a_source) = 0;

    /// @brief GPU timestamp の 1 秒あたりの tick 数を返す
    [[nodiscard]] virtual Result<std::uint64_t> timestamp_frequency() const = 0;
};

/// @brief Graphics、Compute、Copy の Queue Context と横断同期を提供する契約
class IQueuePool : public IGpuExecution
{
public:
    ~IQueuePool() override = default;

    /// @brief Pool が生存する間だけ指定 Queue を貸す
    [[nodiscard]] virtual IQueueContext& context(GpuQueueType a_type) noexcept = 0;

    /// @brief 指定種類の未貸出 Queue を取得する
    [[nodiscard]] virtual Result<QueueContextLease> acquire_context(GpuQueueType a_type) = 0;

    /// @brief 生存中の貸出 Queue を Pool の寿命内だけ借用する
    [[nodiscard]] virtual Result<IQueueContext*> context(QueueContextLease a_lease) = 0;

    /// @brief GPU 完了後に Queue の貸出を返す
    [[nodiscard]] virtual Result<void> return_context(QueueContextLease a_lease) = 0;

    /// @brief 貸出 Queue の現在位置を Queue 識別子付き Fence に記録する
    [[nodiscard]] virtual Result<GpuFencePoint> signal_context(QueueContextLease a_lease) = 0;

    /// @brief Present に使う Graphics Queue を Pool の寿命内だけ貸す
    [[nodiscard]] virtual IQueueContext& present_context() noexcept = 0;

    /// @brief Graphics Queue の先行処理を完了させる
    [[nodiscard]] virtual Result<void> wait_for_graphics_queue() = 0;
};
} // namespace cue
