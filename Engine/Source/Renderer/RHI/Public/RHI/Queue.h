#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <Foundation/Result.h>

namespace cue
{
/// @brief GPU Queue が受け付ける Command の種類
enum class QueueType
{
    Graphics,
    Compute,
    Copy,
};

/// @brief Backend が所有する GPU Queue の同期契約
///
/// QueuePool から Lease で借用し、Backend 停止後も Lease の破棄まで有効とする
/// 実装は同一 Queue への投入と Fence 発行を直列化する。待機循環は呼出側で防ぐ
class IQueueContext
{
public:
    /// @brief 派生 Queue を基底 Pointer から安全に破棄する
    virtual ~IQueueContext() = default;

    IQueueContext(const IQueueContext&) = delete;
    IQueueContext& operator=(const IQueueContext&) = delete;

    /// @brief Queue が受け付ける Command の種類を返す
    [[nodiscard]] virtual QueueType type() const noexcept = 0;

    /// @brief 既に投入した GPU 作業の完了点を記録して Fence 値を返す
    [[nodiscard]] virtual Result<std::uint64_t> signal() = 0;

    /// @brief 指定した自 Queue の Fence 値まで CPU を待機させる
    ///
    /// 未発行の値は InvalidArgument を返す。Device Lost 時は PlatformFailure を返す
    [[nodiscard]] virtual Result<void> wait_for_fence(std::uint64_t a_fenceValue) = 0;

    /// @brief 指定した自 Queue の Fence 値が完了したか返す
    [[nodiscard]] virtual bool is_fence_complete(std::uint64_t a_fenceValue) const noexcept = 0;

    /// @brief 別 Queue の指定した Fence 完了を、この Queue の GPU 作業に先行させる
    ///
    /// 同一 Device の Queue だけを受け付ける。呼出側は循環待機を作らない
    [[nodiscard]] virtual Result<void> wait_for_queue(IQueueContext& a_queue, std::uint64_t a_fenceValue) = 0;

    /// @brief GPU Timestamp の毎秒 Tick 数を返す
    [[nodiscard]] virtual Result<std::uint64_t> get_timestamp_frequency() const = 0;

protected:
    IQueueContext() = default;
};

/// @brief Queue と共有所有する Pool 状態を保持し、破棄時に借用を返す
///
/// Pool や Backend の停止後も Native Queue の寿命を保つ
using queueLease = std::unique_ptr<IQueueContext, std::function<void(IQueueContext*)>>;

/// @brief Backend が所有する複数 Queue の借用契約
///
/// 借用 Lease は破棄時に自動返却する。停止後は新規貸出を拒否する
/// Pool 自体の生 Pointer は Backend 停止で失効する
class IQueuePool
{
public:
    /// @brief 派生 Pool を基底 Pointer から安全に破棄する
    virtual ~IQueuePool() = default;

    IQueuePool(const IQueuePool&) = delete;
    IQueuePool& operator=(const IQueuePool&) = delete;

    /// @brief 空き Queue を借用する。容量不足または停止後は InvalidState を返す
    [[nodiscard]] virtual Result<queueLease> acquire(QueueType a_type) = 0;

    /// @brief 貸出がなければ全 Queue の発行済み作業が完了するまで CPU を待機させる
    [[nodiscard]] virtual Result<void> wait_idle() = 0;

    /// @brief 新規貸出を止め、貸出がなければ GPU 完了を待って停止する
    ///
    /// 貸出が残る場合は InvalidState を返し、Lease が Queue の寿命を維持する
    [[nodiscard]] virtual Result<void> shutdown() = 0;

protected:
    IQueuePool() = default;
};
} // namespace cue
