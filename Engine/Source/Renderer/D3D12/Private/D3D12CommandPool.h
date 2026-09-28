#pragma once

#include "D3D12DeviceContext.h"
#include "D3D12Presentation.h"
#include "D3D12QueueContext.h"

#include <array>
#include <cstdint>
#include <memory>

namespace cue::detail
{
/// @brief 一回の記録中だけ借用する Queue 対応の Command Context
struct CommandLease final
{
    UINT slot = 0;
    std::uint64_t generation = 0;
    ID3D12GraphicsCommandList* list = nullptr;
};

/// @brief Queue 種別と Buffer Slot ごとの Allocator、List、Fence 再利用条件を所有する
class D3D12CommandPool final
{
public:
    /// @brief 使用前の Command Context Owner を作る
    D3D12CommandPool() = default;

    D3D12CommandPool(const D3D12CommandPool&) = delete;
    D3D12CommandPool& operator=(const D3D12CommandPool&) = delete;

    /// @brief Back Buffer 数の指定 Queue 用 Context を生成する
    [[nodiscard]] static Result<std::unique_ptr<D3D12CommandPool>> create(
        D3D12DeviceContext& a_device, GpuQueueType a_type = GpuQueueType::Graphics);

    /// @brief 対応する Fence 完了後だけ Context を貸し出す
    [[nodiscard]] Result<CommandLease> acquire(D3D12QueueContext& a_queue, UINT a_slot);

    /// @brief 記録済み Context を閉じて対応 Queue へ投入する
    [[nodiscard]] Result<void> submit(D3D12QueueContext& a_queue, CommandLease a_lease);

    /// @brief Submit 前の記録失敗時に貸出 List を閉じる
    [[nodiscard]] Result<void> abort(CommandLease a_lease);

    /// @brief Present 後の Queue 位置を Context の再利用条件として記録する
    [[nodiscard]] Result<void> retire(D3D12QueueContext& a_queue, CommandLease a_lease);

    /// @brief GPU 完了後に旧 Back Buffer を参照し得る List を全て解放する
    void release_for_resize() noexcept;

    /// @brief Resize 後に Context の Command List を再生成する
    [[nodiscard]] Result<void> recreate_lists(D3D12DeviceContext& a_device);

    /// @brief Submit 後の Queue 完了がまだ確認されていないか返す
    [[nodiscard]] bool has_pending_gpu() const noexcept;

    /// @brief Queue 全体の完了後に Fence 条件を解除する
    void mark_idle() noexcept;

private:
    enum class ContextStatus
    {
        Idle,
        Recording,
        Submitted
    };

    struct FrameContext final
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t fenceValue = 0;
        std::uint64_t generation = 0;
        ContextStatus status = ContextStatus::Idle;
    };

    /// @brief Slot と借用 List が現在の貸出状態に一致するか確認する
    [[nodiscard]] bool is_lease(CommandLease a_lease, ContextStatus a_status) const noexcept;

    std::array<FrameContext, k_backBufferCount> m_contexts;
    GpuQueueType m_type = GpuQueueType::Graphics;
    bool m_hasPendingGpu = false;
};
} // namespace cue::detail
