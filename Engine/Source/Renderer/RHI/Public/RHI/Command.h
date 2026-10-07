#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <Foundation/Result.h>
#include <RHI/Queue.h>

namespace cue
{
/// @brief GPU Command の記録状態を表す
enum class CommandState
{
    Recording,
    Closed,
    Submitted,
    Failed,
    SubmissionUnknown,
};

/// @brief 一つの Command Allocator と List の記録契約
///
/// Lease が記録中の所有権を持つ。同じ Context の操作は呼出側で直列化する
class ICommandContext
{
public:
    /// @brief 派生 Context を基底 Pointer から安全に破棄する
    virtual ~ICommandContext() = default;

    ICommandContext(const ICommandContext&) = delete;
    ICommandContext& operator=(const ICommandContext&) = delete;

    /// @brief 作成時に固定された Queue の種類を返す
    [[nodiscard]] virtual QueueType type() const noexcept = 0;

    /// @brief 現在の記録状態を返す
    [[nodiscard]] virtual CommandState state() const noexcept = 0;

    /// @brief 記録を閉じ、提出可能な状態にする
    [[nodiscard]] virtual Result<void> close() = 0;

protected:
    ICommandContext() = default;
};

/// @brief Pool の共有状態を保持し、破棄時に Context を自動返却する
using commandLease = std::unique_ptr<ICommandContext, std::function<void(ICommandContext*)>>;

/// @brief 提出した Queue の完了点を、Queue Lease と独立して保持する
///
/// Token は呼出側が所有し、GPU Resource の解放前に is_complete または wait で完了を確認する
class ICommandCompletion
{
public:
    /// @brief 派生 Token を基底 Pointer から安全に破棄する
    virtual ~ICommandCompletion() = default;

    ICommandCompletion(const ICommandCompletion&) = delete;
    ICommandCompletion& operator=(const ICommandCompletion&) = delete;

    /// @brief 提出した Queue の種類を返す
    [[nodiscard]] virtual QueueType type() const noexcept = 0;

    /// @brief 提出先 Fence Timeline の識別子を返す。0 は未対応を表す
    [[nodiscard]] virtual std::uint64_t queue_identity() const noexcept
    {
        return 0;
    }

    /// @brief 提出と同じ操作で発行した Fence 値を返す。0 は未対応を表す
    [[nodiscard]] virtual std::uint64_t fence_value() const noexcept
    {
        return 0;
    }

    /// @brief 対応する GPU 作業が正常に完了したか返す
    [[nodiscard]] virtual bool is_complete() const noexcept = 0;

    /// @brief 対応する GPU 作業の完了または Device Lost を待つ
    [[nodiscard]] virtual Result<void> wait() = 0;

protected:
    ICommandCompletion() = default;
};

/// @brief 提出先 Queue の Fence を所有する完了 Token
using commandCompletion = std::unique_ptr<ICommandCompletion>;

/// @brief Command Context の貸出、提出、Fence 後再利用を管理する契約
///
/// Pool は Backend が所有する。Lease は Pool 停止後も Resource の寿命を保つ
/// Pool の公開操作は直列化される。同じ Context の記録と提出は呼出側で直列化する
class ICommandPool
{
public:
    /// @brief 派生 Pool を基底 Pointer から安全に破棄する
    virtual ~ICommandPool() = default;

    ICommandPool(const ICommandPool&) = delete;
    ICommandPool& operator=(const ICommandPool&) = delete;

    /// @brief 記録可能な Context を借用し、容量到達時は返却済みの最古 GPU 提出を待つ
    ///
    /// 全 Slot が CPU 借用中なら待たずに InvalidState を返す。GPU 完了前の Reset は行わない
    [[nodiscard]] virtual Result<commandLease> acquire(QueueType a_type) = 0;

    /// @brief Close 済みの借用 Context を指定 Queue に投入し、対応 Fence 値を記録する
    ///
    /// 呼出側は提出中、指定 Queue の Lease を保持する。提出後は Slot が Fence の寿命を保持する
    /// Signal 失敗時は投入済みの可能性があるため、GPU 完了を確認できなければ終了する
    [[nodiscard]] virtual Result<commandCompletion> submit(IQueueContext& a_queue, ICommandContext& a_context) = 0;

    /// @brief 新規貸出を止め、貸出がなければ GPU 完了を待つ
    [[nodiscard]] virtual Result<void> shutdown() = 0;

protected:
    ICommandPool() = default;
};
} // namespace cue
