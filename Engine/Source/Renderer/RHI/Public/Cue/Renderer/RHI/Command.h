#pragma once

#include <Cue/Renderer/RHI/Queue.h>

#include <cstdint>

namespace cue
{
class ICommandPool;

/// @brief Pool の世代と Index で一回の Command Context 貸出を識別する
struct CommandContextLease final
{
    std::uint32_t slot = 0;
    std::uint32_t contextIndex = 0;
    std::uint64_t generation = 0;
    const ICommandPool* owner = nullptr;
};

/// @brief 一つの Command Allocator と List の状態を借用する契約
/// @details Pool と Queue より短く生存し、記録中または GPU 完了待機中に他 Thread から操作しない
class ICommandContext
{
public:
    virtual ~ICommandContext() = default;

    /// @brief この Context が記録できる Queue 種別を返す
    [[nodiscard]] virtual GpuQueueType type() const noexcept = 0;

    /// @brief Backend 内部でのみ利用する Native Command List を一時的に貸す
    [[nodiscard]] virtual void* native_command_list() const noexcept = 0;

    /// @brief 最後の Submit が GPU で完了したかを非待機で返す
    [[nodiscard]] virtual bool is_pending_fence_complete() const noexcept = 0;

    /// @brief 最後の Submit の GPU 完了を待つ
    [[nodiscard]] virtual Result<void> wait_for_pending_fence() const = 0;

    /// @brief Queue 種別が Timestamp Query に対応するか返す
    [[nodiscard]] virtual bool supports_timestamps() const noexcept = 0;

    /// @brief 記録中 List の指定 Query に GPU Timestamp を記録する
    [[nodiscard]] virtual Result<void> write_timestamp(std::uint32_t a_queryIndex) = 0;

    /// @brief 記録した Query を Readback Buffer へ転送する
    [[nodiscard]] virtual Result<void> resolve_timestamps(std::uint32_t a_firstQuery,
                                                            std::uint32_t a_queryCount) = 0;

    /// @brief Submit の Fence 完了後に Timestamp を読む
    [[nodiscard]] virtual Result<std::uint64_t> read_timestamp(std::uint32_t a_queryIndex) const = 0;

    /// @brief GPU Capture の Event Scope を記録する
    virtual void begin_event(const char* a_name) = 0;
    virtual void end_event() = 0;
};

/// @brief Queue 種別ごとに Command Context の貸出と返却を管理する契約
/// @details Lease は Pool と Queue が生存する間だけ有効。Submit 後は必ず retire して Fence を記録する
class ICommandPool
{
public:
    virtual ~ICommandPool() = default;

    /// @brief Fence 完了後に Frame Slot の Context を記録状態で貸す
    [[nodiscard]] virtual Result<CommandContextLease> acquire_context(IQueueContext& a_queue,
                                                                        std::uint32_t a_slot) = 0;

    /// @brief 生存 Lease の Context を Pool の寿命内だけ貸す
    [[nodiscard]] virtual Result<ICommandContext*> context(CommandContextLease a_lease) = 0;

    /// @brief 記録済み List を対応 Queue へ投入する
    [[nodiscard]] virtual Result<void> submit_context(IQueueContext& a_queue,
                                                       CommandContextLease a_lease) = 0;

    /// @brief Submit 前に失敗した記録を取り消す
    [[nodiscard]] virtual Result<void> abort_context(CommandContextLease a_lease) = 0;

    /// @brief Submit 済み List に Fence 値を設定して再利用可能な状態へ戻す
    [[nodiscard]] virtual Result<std::uint64_t> retire_context(IQueueContext& a_queue,
                                                                CommandContextLease a_lease) = 0;
};
} // namespace cue
