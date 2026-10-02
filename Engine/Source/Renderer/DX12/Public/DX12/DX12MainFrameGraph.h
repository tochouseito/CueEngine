#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <DX12/DX12FrameGraphExecutor.h>
#include <Foundation/Result.h>
#include <FrameGraph/FrameGraphBuilder.h>

namespace cue
{
class ICommandCompletion;
}

namespace cue::dx12
{
class DX12DescriptorAllocator;
class DX12FinalColorFrames;
class DX12FullscreenPipeline;
class DX12GpuCommandContext;
class DX12RenderDevice;
class DX12SwapChain;

/// @brief 固定 Pass の間へ追加する描画 Pass と記録 Callback
///
/// Callback が借用する外部資源は Graph の記録完了まで呼出側が保持する
struct DX12MainGraphPass final
{
    FrameGraphPassHandle handle;
    dx12FrameGraphPassCallback callback;
};

/// @brief FinalColorTexture を受け取り、追加 Pass と Callback を登録する
using dx12MainGraphConfigure =
    std::function<Result<void>(FrameGraphBuilder &, FrameGraphResourceHandle, std::vector<DX12MainGraphPass> &)>;

/// @brief FinalColor Clear と Back Buffer 表示を一つの本番 Graph に固定する
///
/// SwapChain と Descriptor Allocator は借用し、本体より長く生存させる
/// 記録、提出完了の登録、停止は Thread をまたぐ場合も直列に呼ぶ
/// Present は本体が呼ばず、呼出側が記録済み Command の提出後に呼ぶ
class DX12MainFrameGraph final
{
    struct CreateToken final
    {
    };

  public:
    /// @brief 検証済み Plan と物理 Resource の所有権を受け取る
    DX12MainFrameGraph(CreateToken, FrameGraphPlan a_plan, FrameGraphResourceHandle a_finalColor,
                       FrameGraphResourceHandle a_backBuffer, FrameGraphPassHandle a_clearPass,
                       FrameGraphPassHandle a_displayPass, std::vector<dx12FrameGraphPassCallback> a_customCallbacks,
                       std::unique_ptr<DX12FinalColorFrames> a_frames,
                       std::unique_ptr<DX12FullscreenPipeline> a_pipeline, DX12SwapChain &a_swapChain,
                       DX12DescriptorAllocator &a_srvAllocator, std::array<float, 4> a_clearColor,
                       std::uint32_t a_width, std::uint32_t a_height) noexcept;

    /// @brief SwapChain の形状から二つの固定 Pass と枠ごとの FinalColor を生成する
    ///
    /// Resize 後は旧 Graph の GPU 完了と shutdown を確認してから再生成する
    /// a_configure は Clear と表示の間へ Pass を加え、全追加 Pass の Callback を登録する
    /// 失敗時は部分生成物を公開しない
    [[nodiscard]] static Result<std::unique_ptr<DX12MainFrameGraph>> create(
        DX12RenderDevice &a_device, DX12SwapChain &a_swapChain, std::uint32_t a_frameCount,
        DX12DescriptorAllocator &a_rtvAllocator, DX12DescriptorAllocator &a_srvAllocator,
        std::array<float, 4> a_clearColor, dx12MainGraphConfigure a_configure = {});

    /// @brief GPU 完了を待って所有 Resource を解放する
    ~DX12MainFrameGraph();

    DX12MainFrameGraph(const DX12MainFrameGraph &) = delete;
    DX12MainFrameGraph &operator=(const DX12MainFrameGraph &) = delete;

    /// @brief Graph の Barrier、Clear、全画面 Draw と Present State 復帰を記録する
    ///
    /// 現在の Back Buffer を借用する。成功後は Command を同じ Graphics Queue に提出する
    /// 失敗時は Command List を提出せず、破棄または Reset する
    [[nodiscard]] Result<void> record(std::uint32_t a_frameIndex, DX12GpuCommandContext &a_context);

    /// @brief 提出済み Command の完了点を枠へ登録し、再利用と停止の待機に使う
    [[nodiscard]] Result<void> mark_submitted(std::uint32_t a_frameIndex,
                                              std::shared_ptr<ICommandCompletion> a_completion);

    /// @brief 固定 Pass と Barrier の検証済み計画を本体の生存中だけ借用する
    [[nodiscard]] const FrameGraphPlan &plan() const noexcept;

    /// @brief 全枠の GPU 完了後に FinalColor と Pipeline を解放する
    ///
    /// 待機失敗時は Resource を保持し、後から再試行できる
    [[nodiscard]] Result<void> shutdown();

  private:
    FrameGraphPlan m_plan;
    FrameGraphResourceHandle m_finalColor;
    FrameGraphResourceHandle m_backBuffer;
    FrameGraphPassHandle m_clearPass;
    FrameGraphPassHandle m_displayPass;
    std::vector<dx12FrameGraphPassCallback> m_customCallbacks;
    std::unique_ptr<DX12FinalColorFrames> m_frames;
    std::unique_ptr<DX12FullscreenPipeline> m_pipeline;
    DX12SwapChain *m_swapChain = nullptr;
    DX12DescriptorAllocator *m_srvAllocator = nullptr;
    std::array<float, 4> m_clearColor{};
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};
} // namespace cue::dx12
