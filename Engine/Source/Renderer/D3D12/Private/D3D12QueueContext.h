#pragma once

#include "D3D12DeviceContext.h"

#include <Cue/Renderer/RHI/GpuExecution.h>

#include <cstdint>
#include <memory>

namespace cue::detail
{
/// @brief 一種類の GPU Queue と完了確認用 Fence を一意所有する
class D3D12QueueContext final
{
public:
    /// @brief Device 借用前の空の Queue Context を作る
    D3D12QueueContext() = default;

    /// @brief 待機 Event を閉じる
    ~D3D12QueueContext();

    D3D12QueueContext(const D3D12QueueContext&) = delete;
    D3D12QueueContext& operator=(const D3D12QueueContext&) = delete;

    /// @brief Device より短い寿命の指定種類の Queue を生成する
    [[nodiscard]] static Result<std::unique_ptr<D3D12QueueContext>> create(
        D3D12DeviceContext& a_device, GpuQueueType a_type = GpuQueueType::Graphics);

    /// @brief 指定 Fence 値の GPU 完了を確認する
    [[nodiscard]] Result<void> wait_for(std::uint64_t a_value) const;

    /// @brief Queue の先行する全処理を待つ
    [[nodiscard]] Result<void> wait_idle();

    /// @brief Queue 上の現在位置を Fence に記録する
    [[nodiscard]] Result<std::uint64_t> signal();

    /// @brief Queue Context の存続中だけ Queue を借用する
    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept;

    /// @brief 同一 Device の別 Queue が発行した Fence まで GPU 上で待つ
    [[nodiscard]] Result<void> wait_on(const D3D12QueueContext& a_source, std::uint64_t a_value);

    /// @brief この Queue で発行済みの Fence 値かを確認する
    [[nodiscard]] bool has_issued(std::uint64_t a_value) const noexcept;

    /// @brief Command Context と Queue の種類を照合する
    [[nodiscard]] GpuQueueType type() const noexcept;

private:
    // Device Context は Queue Context より長く生存し、Device Lost の診断にだけ使用する
    ID3D12Device* m_device = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    std::uint64_t m_nextFenceValue = 1;
    GpuQueueType m_type = GpuQueueType::Graphics;
};
} // namespace cue::detail
