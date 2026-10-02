#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>

#include <DX12/DX12DescriptorAllocator.h>
#include <Foundation/Result.h>
#include <FrameGraph/FrameGraphBuilder.h>
#include <RHI/Command.h>

namespace cue::dx12
{
class DX12FrameGraphResources;
class DX12GpuResource;
class DX12RenderDevice;

/// @brief 本番 Graph の FinalColorTexture を描画枠ごとに物理化して所有する
///
/// RTV と Shader 可視 SRV は外部 Allocator から借りる。Allocator は本体の shutdown より長く生存させる
/// 公開操作は一つの制御 Thread から直列化し、枠の再利用前に begin_frame で前回 GPU 完了を待つ
class DX12FinalColorFrames final
{
    struct CreateToken final
    {
    };

public:
    /// @brief create の内部でだけ未生成状態を構築する
    explicit DX12FinalColorFrames(CreateToken) noexcept;

    /// @brief 同じ Graph Plan から枠ごとに独立した Texture、RTV、SRV を作る
    [[nodiscard]] static Result<std::unique_ptr<DX12FinalColorFrames>> create(
        DX12RenderDevice& a_device, const FrameGraphPlan& a_plan, FrameGraphResourceHandle a_finalColor,
        std::uint32_t a_frameCount, DX12DescriptorAllocator& a_rtvAllocator,
        DX12DescriptorAllocator& a_srvAllocator);

    /// @brief GPU 完了後に枠の Resource と Descriptor を回収する
    ~DX12FinalColorFrames();

    DX12FinalColorFrames(const DX12FinalColorFrames&) = delete;
    DX12FinalColorFrames& operator=(const DX12FinalColorFrames&) = delete;

    /// @brief 次の記録前に同じ枠の前回 GPU 作業を待つ
    [[nodiscard]] Result<void> begin_frame(std::uint32_t a_frameIndex);

    /// @brief 提出済み Graph の GPU 完了点を枠へ登録する
    [[nodiscard]] Result<void> mark_submitted(std::uint32_t a_frameIndex,
                                              std::shared_ptr<ICommandCompletion> a_completion);

    /// @brief 指定枠の FinalColorTexture を本体の生存中だけ借用する
    [[nodiscard]] DX12GpuResource* resource(std::uint32_t a_frameIndex) const noexcept;

    /// @brief 指定枠の Graph Resource を提出完了まで借用する
    [[nodiscard]] DX12FrameGraphResources* graph_resources(std::uint32_t a_frameIndex) const noexcept;

    /// @brief 指定枠の RTV を返す
    [[nodiscard]] Result<D3D12_CPU_DESCRIPTOR_HANDLE> rtv(std::uint32_t a_frameIndex) const;

    /// @brief 指定枠の Shader 可視 SRV を返す
    [[nodiscard]] Result<D3D12_GPU_DESCRIPTOR_HANDLE> srv(std::uint32_t a_frameIndex) const;

    /// @brief 描画枠の個数を返す
    [[nodiscard]] std::size_t frame_count() const noexcept;

    /// @brief 全枠の GPU 完了を待って Resource と Descriptor を解放する
    ///
    /// 待機失敗時は所有状態を維持し、再試行できる
    [[nodiscard]] Result<void> shutdown();

private:
    struct Frame final
    {
        std::unique_ptr<DX12FrameGraphResources> graph;
        DX12DescriptorHandle rtvHandle;
        DX12DescriptorHandle srvHandle;
        std::shared_ptr<ICommandCompletion> completion;
    };

    std::vector<Frame> m_frames;
    DX12DescriptorAllocator* m_rtvAllocator = nullptr;
    DX12DescriptorAllocator* m_srvAllocator = nullptr;
    FrameGraphResourceHandle m_finalColor;
    bool m_isClosed = false;
};
} // namespace cue::dx12
