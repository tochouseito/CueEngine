#pragma once

#include "DX12RenderDevice.h"

#include <Cue/Renderer/RHI/Queue.h>

#include <cstdint>
#include <memory>

namespace cue::detail
{
/// @brief 一種類の GPU Queue と完了確認用 Fence を一意所有する
class DX12GpuCommandQueue final : public IQueueContext
{
public:
    /// @brief Device 借用前の空の Queue Context を作る
    DX12GpuCommandQueue() = default;

    /// @brief 待機 Event を閉じる
    ~DX12GpuCommandQueue() override;

    DX12GpuCommandQueue(const DX12GpuCommandQueue&) = delete;
    DX12GpuCommandQueue& operator=(const DX12GpuCommandQueue&) = delete;

    /// @brief Device より短い寿命の指定種類の Queue を生成する
    [[nodiscard]] static Result<std::unique_ptr<DX12GpuCommandQueue>> create(
        DX12RenderDevice& a_device, GpuQueueType a_type = GpuQueueType::Graphics);

    /// @brief 指定 Fence 値の GPU 完了を確認する
    [[nodiscard]] Result<void> wait_for(std::uint64_t a_value) const override;

    /// @brief Queue の先行する全処理を待つ
    [[nodiscard]] Result<void> wait_idle() override;

    /// @brief Queue 上の現在位置を Fence に記録する
    [[nodiscard]] Result<std::uint64_t> signal() override;

    /// @brief Queue Context の存続中だけ Queue を借用する
    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept;

    /// @brief Command Pool が同じ Device の Queue か検証するための借用を返す
    [[nodiscard]] ID3D12Device* device() const noexcept;

    /// @brief 同一 Device の別 Queue が発行した Fence まで GPU 上で待つ
    [[nodiscard]] Result<void> wait_on(const DX12GpuCommandQueue& a_source, std::uint64_t a_value);

    /// @brief この Queue で発行済みの Fence 値かを確認する
    [[nodiscard]] bool has_issued(std::uint64_t a_value) const noexcept;

    /// @brief CPU を待機させずに Fence 完了を調べる
    [[nodiscard]] bool is_complete(std::uint64_t a_value) const noexcept override;

    /// @brief 同じ Device の発行元 Queue の現在位置を GPU 待機へ接続する
    [[nodiscard]] Result<void> wait_for_queue(IQueueContext& a_source) override;

    /// @brief DX12 Queue の timestamp frequency を返す
    [[nodiscard]] Result<std::uint64_t> timestamp_frequency() const override;

    /// @brief Command Context と Queue の種類を照合する
    [[nodiscard]] GpuQueueType type() const noexcept override;

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
