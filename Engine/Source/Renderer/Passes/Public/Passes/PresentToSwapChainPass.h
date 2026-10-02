#pragma once

#include <FrameGraph/FrameGraph.h>

namespace cue
{
/// @brief FinalColorTexture を BackBuffer へ複写する表示 Pass
///
/// SwapChain の Present は Graph 提出後に Host が行う
class PresentToSwapChainPass final : public FrameGraphPass
{
public:
    /// @brief 表示元と BackBuffer は setup で名前から取得する
    PresentToSwapChainPass() noexcept = default;

    /// @brief 診断用の名前を返す
    [[nodiscard]] const char* name() const noexcept override
    {
        return "PresentToSwapChain";
    }

    /// @brief BackBuffer を扱う Queue を返す
    [[nodiscard]] QueueType type() const noexcept override
    {
        return QueueType::Graphics;
    }

    /// @brief 表示元と BackBuffer を名前で取得する
    [[nodiscard]] Result<void> setup(FrameGraphBuilder& a_builder) override
    {
        auto colorResult = a_builder.get_texture("FinalColorTexture");
        auto backResult = a_builder.get_texture("BackBuffer");
        if (!colorResult.has_value() || !backResult.has_value())
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "PresentToSwapChainPass.setup"});
        }
        m_finalColor = colorResult.take_value();
        m_backBuffer = backResult.take_value();
        return Result<void>::success();
    }

    /// @brief Copy 元と先の Access と State を宣言する
    [[nodiscard]] Result<void> describe_resources(FrameGraphBuilder& a_builder) override
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

    /// @brief Backend Context に Copy を記録する
    [[nodiscard]] Result<void> execute(FrameGraphContext& a_context) override
    {
        return a_context.copy_texture2d(m_finalColor, m_backBuffer);
    }

private:
    FrameGraphResourceHandle m_finalColor;
    FrameGraphResourceHandle m_backBuffer;
};
} // namespace cue
