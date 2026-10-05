#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include <Foundation/Result.h>
#include <RHI/Command.h>

namespace cue::dx12
{
class DX12RenderDevice;
class DX12PipelineManager;

/// @brief DX12 Allocator と Command List を一組として保持する
///
/// Pool が所有し、Lease の生存中だけ呼出側へ借用する。記録操作は呼出側で直列化する
class DX12GpuCommandContext final : public ICommandContext
{
    struct CreateToken final
    {
    };

public:
    /// @brief create だけが部分生成状態を構築する
    explicit DX12GpuCommandContext(CreateToken) noexcept;

    /// @brief 種類が一致する Allocator と List を生成する
    [[nodiscard]] static Result<std::unique_ptr<DX12GpuCommandContext>> create(ID3D12Device& a_device,
                                                                                QueueType a_type,
                                                                                std::uint32_t a_index);

    /// @brief COM Resource を解放する
    ~DX12GpuCommandContext() override = default;

    DX12GpuCommandContext(const DX12GpuCommandContext&) = delete;
    DX12GpuCommandContext& operator=(const DX12GpuCommandContext&) = delete;

    /// @brief List が受け付ける Queue の種類を返す
    [[nodiscard]] QueueType type() const noexcept override;

    /// @brief 現在の記録状態を返す
    [[nodiscard]] CommandState state() const noexcept override;

    /// @brief 記録中の List を Close する。失敗時は再利用しない
    [[nodiscard]] Result<void> close() override;

    /// @brief Lease の生存中だけ記録用 Native List を借用する
    ///
    /// 呼出側は Reset、Close、ExecuteCommandLists を直接呼ばない
    [[nodiscard]] ID3D12GraphicsCommandList* command_list() const noexcept;

private:
    friend class DX12CommandPool;
    friend class DX12PipelineManager;

    /// @brief 記録中の PSO と Root を Reset または GPU 完了後の破棄まで保持する
    [[nodiscard]] Result<void> retain_pipeline(std::shared_ptr<const void> a_pipeline);

    /// @brief GPU 完了確認後または未提出のときだけ Allocator と List を再記録可能にする
    [[nodiscard]] Result<void> reset_for_recording();

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_submissionFence;
    std::vector<std::shared_ptr<const void>> m_pipelineReferences;
    CommandState m_state = CommandState::Recording;
    bool m_isFatalFailure = false;
    std::uint64_t m_fenceValue = 0;
    QueueType m_type = QueueType::Graphics;
};

/// @brief 各 Queue 種類の Allocator と Command List を Fence 完了まで保留して再利用する
///
/// Queue は所有せず、提出済み Slot に Fence を保持して最大32 Contextずつ生成する
class DX12CommandPool final : public ICommandPool
{
    struct CreateToken final
    {
    };

public:
    /// @brief create だけが共有状態を構築する
    explicit DX12CommandPool(CreateToken) noexcept;

    /// @brief Device から三種類の Command Context の貸出経路を作る
    ///
    /// a_gpuLifetime は最後の Command Lease が GPU 完了を確認するまで共有所有する
    [[nodiscard]] static Result<std::unique_ptr<DX12CommandPool>> create(
        DX12RenderDevice& a_device, std::shared_ptr<const void> a_gpuLifetime = {});

    /// @brief 停止を試み、残る Lease に Resource の寿命を委ねる
    ~DX12CommandPool() override;

    DX12CommandPool(const DX12CommandPool&) = delete;
    DX12CommandPool& operator=(const DX12CommandPool&) = delete;

    /// @brief 完了済み Slot を優先して貸し出し、必要なときだけ新規生成する
    [[nodiscard]] Result<commandLease> acquire(QueueType a_type) override;

    /// @brief 指定された同一 Device・同種類の Queue に Close 済み Context を投入する
    [[nodiscard]] Result<commandCompletion> submit(IQueueContext& a_queue, ICommandContext& a_context) override;

    /// @brief 新規貸出を止め、貸出がなければ各 Slot の Fence 完了を待つ
    [[nodiscard]] Result<void> shutdown() override;

private:
    struct State;
    std::shared_ptr<State> m_state;
};
} // namespace cue::dx12
