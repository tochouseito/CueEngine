#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <DX12/DX12FrameGraphExecutor.h>
#include <Foundation/Result.h>
#include <FrameGraph/FrameGraph.h>
#include <RHI/GpuResourcePool.h>

namespace cue
{
class ICommandCompletion;
}

namespace cue::dx12
{
class DX12DescriptorAllocator;
class DX12FinalColorFrames;
class DX12GpuCommandContext;
class DX12RenderDevice;
class DX12SwapChain;

/// @brief 固定 Clear と表示の間へ任意の Pass を追加する
///
/// 追加する Pass は FrameGraph が所有し、Graph の記録と同じ Thread で実行する
using dx12MainGraphConfigure = std::function<Result<void>(FrameGraph&, FrameGraphResourceHandle)>;

/// @brief 旧 FrameGraphPass 契約で本番描画 Graph を構築・記録する
///
/// SwapChain と Allocator を借用する。Present と Queue 提出は呼出側が行う
class DX12MainFrameGraph final : public IFrameGraphRecorder
{
    struct CreateToken final
    {
    };

public:
    /// @brief Graph と枠ごとの物理 Resource の所有権を受け取る
    DX12MainFrameGraph(CreateToken, std::unique_ptr<FrameGraph> a_graph,
                       FrameGraphResourceHandle a_finalColor, FrameGraphResourceHandle a_backBuffer,
                       std::unique_ptr<DX12FinalColorFrames> a_frames, DX12SwapChain& a_swapChain);

    /// @brief SwapChain と同じ形状の FinalColorTexture を枠ごとに用意する
    [[nodiscard]] static Result<std::unique_ptr<DX12MainFrameGraph>> create(
        DX12RenderDevice& a_device, DX12SwapChain& a_swapChain, std::uint32_t a_frameCount,
        DX12DescriptorAllocator& a_rtvAllocator, DX12DescriptorAllocator& a_srvAllocator,
        std::array<float, 4> a_clearColor, dx12MainGraphConfigure a_configure = {});

    /// @brief GPU 完了後に資源を解放する
    ~DX12MainFrameGraph() override;

    DX12MainFrameGraph(const DX12MainFrameGraph&) = delete;
    DX12MainFrameGraph& operator=(const DX12MainFrameGraph&) = delete;

    /// @brief Pass を依存順に実行し、終了 State まで記録する
    ///
    /// 手動提出時は mark_submitted、未提出時は discard_unsubmitted を必ず呼ぶ
    [[nodiscard]] Result<void> record(std::uint32_t a_frameIndex, DX12GpuCommandContext& a_context);

    /// @brief 基底 Command を DX12 Context に検証して記録する
    [[nodiscard]] Result<void> record(std::uint32_t a_frameIndex, ICommandContext& a_context) override;

    /// @brief FrameGraph が Command Pool から借用、提出する
    [[nodiscard]] Result<bool> execute(std::uint32_t a_frameIndex, ICommandPool& a_commandPool,
                                       std::function<bool()> a_shouldCancel = {});

    /// @brief QueuePool の Compute／Copy Queue を必要な Pass に貸し、依存順に提出する
    [[nodiscard]] Result<bool> execute(std::uint32_t a_frameIndex, ICommandPool& a_commandPool,
                                       IQueuePool& a_queuePool, std::function<bool()> a_shouldCancel = {});

    /// @brief 提出の GPU 完了点を枠へ登録する
    [[nodiscard]] Result<void> mark_submitted(std::uint32_t a_frameIndex,
                                              std::shared_ptr<ICommandCompletion> a_completion) override;

    /// @brief 提出しなかった Graph 記録の Pool Lease を返す
    void discard_unsubmitted(std::uint32_t a_frameIndex) noexcept override;

    /// @brief 構築済み Plan を本体の生存中だけ返す
    [[nodiscard]] const FrameGraphPlan& plan() const noexcept;

    /// @brief 枠の GPU 完了後に物理 Resource を破棄する
    [[nodiscard]] Result<void> shutdown();

private:
    /// @brief 枠の Pool Lease と外部 Binding を初回 Pass より前に保持する
    [[nodiscard]] Result<void> prepare_frame(std::uint32_t a_frameIndex);

    /// @brief 準備済み枠の指定 Pass と終了 Barrier を記録する
    [[nodiscard]] Result<void> record_range(std::uint32_t a_frameIndex, DX12GpuCommandContext& a_context,
                                            std::size_t a_firstPass, std::size_t a_passCount,
                                            bool a_includeFinal);

    /// @brief 未提出または GPU 完了後の枠の借用を返す
    void clear_frame(std::uint32_t a_frameIndex) noexcept;

    std::unique_ptr<FrameGraph> m_graph;
    FrameGraphResourceHandle m_finalColor;
    FrameGraphResourceHandle m_backBuffer;
    std::unique_ptr<DX12FinalColorFrames> m_frames;
    std::vector<std::vector<gpuResourceLease>> m_poolLeases;
    std::vector<std::vector<DX12FrameGraphExternalResource>> m_externalBindings;
    std::vector<bool> m_isPrepared;
    DX12SwapChain* m_swapChain = nullptr;
};
} // namespace cue::dx12
