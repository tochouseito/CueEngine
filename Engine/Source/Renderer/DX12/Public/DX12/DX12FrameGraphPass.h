#pragma once

#include <array>
#include <cstdint>

#include <d3d12.h>

#include <DX12/DX12FrameGraphExecutor.h>
#include <Foundation/Result.h>
#include <FrameGraph/FrameGraph.h>

namespace cue::dx12
{
/// @brief DX12 の Native Command と Resource を Pass 実行中だけ貸す
class DX12FrameGraphContext final : public FrameGraphContext
{
public:
    /// @brief 記録中の Graphics List と物理 Resource 対応表を借用する
    DX12FrameGraphContext(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_frameIndex,
                          DX12GpuCommandContext& a_command, const DX12FrameGraphPassContext& a_resources,
                          D3D12_CPU_DESCRIPTOR_HANDLE a_finalColorRtv) noexcept;

    /// @brief DX12 の Command List を借用する
    [[nodiscard]] ID3D12GraphicsCommandList& command_list() const noexcept;

    /// @brief 論理 Handle の Native Resource を借用する
    [[nodiscard]] ID3D12Resource* resource(FrameGraphResourceHandle a_handle) const noexcept;

    /// @brief FinalColorTexture の RTV を返す
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE final_color_rtv() const noexcept;

private:
    DX12GpuCommandContext* m_command = nullptr;
    const DX12FrameGraphPassContext* m_resources = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE m_finalColorRtv{};
};

/// @brief FinalColorTexture の初回 Write を宣言して色をクリアする
class DX12ClearFinalColorPass final : public FrameGraphPass
{
public:
    /// @brief Clear Color を複写し、Resource は setup で名前から取得する
    explicit DX12ClearFinalColorPass(std::array<float, 4> a_clearColor) noexcept;

    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] QueueType type() const noexcept override;
    [[nodiscard]] Result<void> setup(FrameGraphBuilder& a_builder) override;
    [[nodiscard]] Result<void> describe_resources(FrameGraphBuilder& a_builder) override;
    [[nodiscard]] Result<void> execute(FrameGraphContext& a_context) override;

private:
    FrameGraphResourceHandle m_finalColor;
    std::array<float, 4> m_clearColor{};
};

/// @brief FinalColorTexture を Back Buffer へ複写する表示 Pass
class DX12PresentToSwapChainPass final : public FrameGraphPass
{
public:
    /// @brief 表示元と Back Buffer は setup で名前から取得する
    DX12PresentToSwapChainPass() noexcept = default;

    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] QueueType type() const noexcept override;
    [[nodiscard]] Result<void> setup(FrameGraphBuilder& a_builder) override;
    [[nodiscard]] Result<void> describe_resources(FrameGraphBuilder& a_builder) override;
    [[nodiscard]] Result<void> execute(FrameGraphContext& a_context) override;

private:
    FrameGraphResourceHandle m_finalColor;
    FrameGraphResourceHandle m_backBuffer;
};
} // namespace cue::dx12
