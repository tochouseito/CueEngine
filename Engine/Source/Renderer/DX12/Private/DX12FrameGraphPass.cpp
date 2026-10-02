#include <DX12/DX12FrameGraphPass.h>

#include <utility>

#include <DX12/DX12CommandPool.h>

namespace cue::dx12
{
/// @brief Pass が参照する情報を記録期間へ限定する
DX12FrameGraphContext::DX12FrameGraphContext(
    std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_frameIndex,
    DX12GpuCommandContext& a_command, const DX12FrameGraphPassContext& a_resources,
    D3D12_CPU_DESCRIPTOR_HANDLE a_finalColorRtv) noexcept
    : FrameGraphContext(a_width, a_height, a_frameIndex, a_command), m_command(&a_command),
      m_resources(&a_resources), m_finalColorRtv(a_finalColorRtv)
{
}

/// @brief 記録中の List を返す
ID3D12GraphicsCommandList& DX12FrameGraphContext::command_list() const noexcept
{
    return *m_command->command_list();
}

/// @brief 検証済み対応表から Native Resource を返す
ID3D12Resource* DX12FrameGraphContext::resource(FrameGraphResourceHandle a_handle) const noexcept
{
    return m_resources->resource(a_handle);
}

/// @brief 枠ごとの RTV を返す
D3D12_CPU_DESCRIPTOR_HANDLE DX12FrameGraphContext::final_color_rtv() const noexcept
{
    return m_finalColorRtv;
}

/// @brief Clear に必要な値を保持する
DX12ClearFinalColorPass::DX12ClearFinalColorPass(std::array<float, 4> a_clearColor) noexcept
    : m_clearColor(a_clearColor)
{
}

/// @brief 計測と診断に使う Pass 名を返す
const char* DX12ClearFinalColorPass::name() const noexcept
{
    return "ClearFinalColor";
}

/// @brief RTV の操作に Graphics Queue を選ぶ
QueueType DX12ClearFinalColorPass::type() const noexcept
{
    return QueueType::Graphics;
}

/// @brief Legacy の setup と同様、名前付き Texture を取得する
Result<void> DX12ClearFinalColorPass::setup(FrameGraphBuilder& a_builder)
{
    auto result = a_builder.get_texture("FinalColorTexture");
    if (!result.has_value())
    {
        return Result<void>::failure(*result.try_error());
    }
    m_finalColor = result.take_value();
    return Result<void>::success();
}

/// @brief 初回 Write に RenderTarget State を要求する
Result<void> DX12ClearFinalColorPass::describe_resources(FrameGraphBuilder& a_builder)
{
    return a_builder.use(m_finalColor, FrameGraphAccess::Write,
                         FrameGraphResourceState::RenderTarget);
}

/// @brief 枠の RTV が論理 Texture と対応する場合だけ Clear を記録する
Result<void> DX12ClearFinalColorPass::execute(FrameGraphContext& a_context)
{
    auto* dx12Context = dynamic_cast<DX12FrameGraphContext*>(&a_context);
    if (!dx12Context || !dx12Context->resource(m_finalColor) ||
        dx12Context->final_color_rtv().ptr == 0)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12ClearFinalColorPass.execute"});
    }
    dx12Context->command_list().ClearRenderTargetView(dx12Context->final_color_rtv(),
                                                       m_clearColor.data(), 0, nullptr);
    return Result<void>::success();
}

/// @brief Present 元と Back Buffer の論理 Handle を保持する
/// @brief Legacy と同じ表示 Pass 名を返す
const char* DX12PresentToSwapChainPass::name() const noexcept
{
    return "PresentToSwapChain";
}

/// @brief Back Buffer を扱う Graphics Queue を選ぶ
QueueType DX12PresentToSwapChainPass::type() const noexcept
{
    return QueueType::Graphics;
}

/// @brief Legacy の setup と同様、表示元と Back Buffer を名前で取得する
Result<void> DX12PresentToSwapChainPass::setup(FrameGraphBuilder& a_builder)
{
    auto colorResult = a_builder.get_texture("FinalColorTexture");
    auto backResult = a_builder.get_texture("BackBuffer");
    if (!colorResult.has_value() || !backResult.has_value())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12PresentToSwapChainPass.setup"});
    }
    m_finalColor = colorResult.take_value();
    m_backBuffer = backResult.take_value();
    return Result<void>::success();
}

/// @brief 表示元と Back Buffer の Copy 状態を Graph に宣言する
Result<void> DX12PresentToSwapChainPass::describe_resources(FrameGraphBuilder& a_builder)
{
    auto sourceResult = a_builder.use(m_finalColor, FrameGraphAccess::Read,
                                      FrameGraphResourceState::CopySource);
    if (!sourceResult.has_value())
    {
        return sourceResult;
    }
    return a_builder.use(m_backBuffer, FrameGraphAccess::Write,
                         FrameGraphResourceState::CopyDestination);
}

/// @brief Graph が状態を遷移させた同一形状の Texture を複写する
Result<void> DX12PresentToSwapChainPass::execute(FrameGraphContext& a_context)
{
    auto* dx12Context = dynamic_cast<DX12FrameGraphContext*>(&a_context);
    if (!dx12Context)
    {
        return Result<void>::failure({ErrorCategory::InvalidArgument, "DX12PresentToSwapChainPass.execute"});
    }
    auto* source = dx12Context->resource(m_finalColor);
    auto* destination = dx12Context->resource(m_backBuffer);
    if (!source || !destination || source == destination)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12PresentToSwapChainPass.resources"});
    }
    const auto sourceDesc = source->GetDesc();
    const auto destinationDesc = destination->GetDesc();
    if (sourceDesc.Width != destinationDesc.Width || sourceDesc.Height != destinationDesc.Height ||
        sourceDesc.Format != destinationDesc.Format || sourceDesc.SampleDesc.Count != destinationDesc.SampleDesc.Count)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "DX12PresentToSwapChainPass.shape"});
    }
    dx12Context->command_list().CopyResource(destination, source);
    return Result<void>::success();
}
} // namespace cue::dx12
