#pragma once

#include "DX12RenderDevice.h"
#include "DX12SwapChain.h"
#include "DX12GpuCommandQueue.h"

#include <Cue/Renderer/RHI/Command.h>

#include <cstdint>
#include <deque>
#include <memory>

namespace cue::detail
{
/// @brief 一回の記録中だけ借用する Queue 対応の Command Context
struct CommandLease final
{
    UINT slot = 0;
    UINT contextIndex = 0;
    std::uint64_t generation = 0;
    ID3D12GraphicsCommandList* list = nullptr;
};

enum class ContextStatus
{
    Idle,
    Recording,
    Submitted
};

/// @brief Legacy と同じく一つの Allocator と List、再利用用 Fence を所有する
/// @details Pool が貸出と返却を制御し、GPU 完了前の Reset を許さない
class DX12GpuCommandContext final : public ICommandContext
{
    friend class DX12CommandPool;

public:
    /// @brief Pool に登録する前の空の Context を作る
    DX12GpuCommandContext() = default;

    DX12GpuCommandContext(const DX12GpuCommandContext&) = delete;
    DX12GpuCommandContext& operator=(const DX12GpuCommandContext&) = delete;
    DX12GpuCommandContext(DX12GpuCommandContext&&) noexcept = default;
    DX12GpuCommandContext& operator=(DX12GpuCommandContext&&) noexcept = default;

    /// @brief Context が記録する Queue 種別を返す
    [[nodiscard]] GpuQueueType type() const noexcept override { return m_type; }

    /// @brief List は Lease が有効な間だけ借用可能とする
    [[nodiscard]] void* native_command_list() const noexcept override { return list.Get(); }

    /// @brief Queue の Fence 完了値を非待機で照合する
    [[nodiscard]] bool is_pending_fence_complete() const noexcept override;

    /// @brief Queue の Fence 完了まで待つ
    [[nodiscard]] Result<void> wait_for_pending_fence() const override;

    [[nodiscard]] bool supports_timestamps() const noexcept override;
    [[nodiscard]] Result<void> write_timestamp(std::uint32_t a_queryIndex) override;
    [[nodiscard]] Result<void> resolve_timestamps(std::uint32_t a_firstQuery,
                                                    std::uint32_t a_queryCount) override;
    [[nodiscard]] Result<std::uint64_t> read_timestamp(std::uint32_t a_queryIndex) const override;
    void begin_event(const char* a_name) override;
    void end_event() override;

private:
    [[nodiscard]] Result<void> initialize_queries(ID3D12Device& a_device);

    static constexpr std::uint32_t k_maxTimestampQueries = 64;
    UINT slot = 0;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> queryReadback;
    std::uint64_t fenceValue = 0;
    std::uint64_t generation = 0;
    ContextStatus status = ContextStatus::Idle;
    GpuQueueType m_type = GpuQueueType::Graphics;
    DX12GpuCommandQueue* m_queue = nullptr;
};

/// @brief Queue 種別と Buffer Slot ごとの Allocator、List、Fence 再利用条件を所有する
class DX12CommandPool final : public ICommandPool
{
public:
    /// @brief 使用前の Command Context Owner を作る
    DX12CommandPool() = default;

    DX12CommandPool(const DX12CommandPool&) = delete;
    DX12CommandPool& operator=(const DX12CommandPool&) = delete;

    /// @brief Back Buffer 数の指定 Queue 用 Context を生成する
    [[nodiscard]] static Result<std::unique_ptr<DX12CommandPool>> create(
        DX12RenderDevice& a_device, GpuQueueType a_type = GpuQueueType::Graphics);

    /// @brief 抽象 Queue を検証して既存の Fence 付き貸出経路へ接続する
    [[nodiscard]] Result<CommandContextLease> acquire_context(IQueueContext& a_queue,
                                                                std::uint32_t a_slot) override;

    /// @brief Owner と世代が一致する Context だけを貸す
    [[nodiscard]] Result<ICommandContext*> context(CommandContextLease a_lease) override;

    /// @brief 貸出 List を閉じて Queue へ投入する
    [[nodiscard]] Result<void> submit_context(IQueueContext& a_queue, CommandContextLease a_lease) override;

    /// @brief 未投入 List の記録を取り消す
    [[nodiscard]] Result<void> abort_context(CommandContextLease a_lease) override;

    /// @brief Queue 位置を Fence として記録する
    [[nodiscard]] Result<std::uint64_t> retire_context(IQueueContext& a_queue,
                                                        CommandContextLease a_lease) override;

    /// @brief 対応する Fence 完了後だけ Context を貸し出す
    [[nodiscard]] Result<CommandLease> acquire(DX12GpuCommandQueue& a_queue, UINT a_slot);

    /// @brief 同一 Slot の複数 Pass を CPU 待機なしで記録できる Context を貸し出す
    [[nodiscard]] Result<CommandLease> acquire_batch(DX12GpuCommandQueue& a_queue, UINT a_slot);

    /// @brief 記録中 List に対応する物理 Context を一 Pass の間だけ貸す
    [[nodiscard]] ICommandContext* find_recording_context(ID3D12GraphicsCommandList* a_list) noexcept;

    /// @brief Frame Slot の前回使用が全て完了するまで待つ
    [[nodiscard]] Result<void> wait_for_slot(DX12GpuCommandQueue& a_queue, UINT a_slot) const;

    /// @brief 記録済み Context を閉じて対応 Queue へ投入する
    [[nodiscard]] Result<void> submit(DX12GpuCommandQueue& a_queue, CommandLease a_lease);

    /// @brief Submit 前の記録失敗時に貸出 List を閉じる
    [[nodiscard]] Result<void> abort(CommandLease a_lease);

    /// @brief Submit 後の Queue 位置を Context の再利用条件として記録する
    [[nodiscard]] Result<void> retire(DX12GpuCommandQueue& a_queue, CommandLease a_lease);

    /// @brief Submit した位置の Fence 値を返し、Queue 間依存へ使う
    [[nodiscard]] Result<std::uint64_t> retire_fence(DX12GpuCommandQueue& a_queue, CommandLease a_lease);

    /// @brief GPU 完了後に旧 Back Buffer を参照し得る List を全て解放する
    void release_for_resize() noexcept;

    /// @brief Resize 後に Context の Command List を再生成する
    [[nodiscard]] Result<void> recreate_lists(DX12RenderDevice& a_device);

    /// @brief Submit 後の Queue 完了がまだ確認されていないか返す
    [[nodiscard]] bool has_pending_gpu() const noexcept;

    /// @brief Queue 全体の完了後に Fence 条件を解除する
    void mark_idle() noexcept;

private:
    /// @brief Slot と借用 List が現在の貸出状態に一致するか確認する
    [[nodiscard]] bool is_lease(CommandLease a_lease, ContextStatus a_status) const noexcept;

    /// @brief 指定 Context を Reset して新しい Lease を返す
    [[nodiscard]] Result<CommandLease> reset_context(DX12GpuCommandQueue& a_queue, UINT a_index);

    /// @brief 追加の Pass 用 Context を生成する
    [[nodiscard]] Result<void> append_context(UINT a_slot);

    /// @brief 抽象 Lease を現在の Native List を持つ内部 Lease へ検証付きで戻す
    [[nodiscard]] Result<CommandLease> native_lease(CommandContextLease a_lease) const;

    std::deque<DX12GpuCommandContext> m_contexts;
    ID3D12Device* m_device = nullptr;
    DX12GpuCommandQueue* m_boundQueue = nullptr;
    GpuQueueType m_type = GpuQueueType::Graphics;
    bool m_hasPendingGpu = false;
};
} // namespace cue::detail
