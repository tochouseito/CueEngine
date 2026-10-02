#pragma once

#include <array>
#include <utility>

#include <FrameGraph/FrameGraph.h>

namespace cue
{
/// @brief FinalColorTexture の初回 Write を宣言して色をクリアする
class ClearFinalColorPass final : public FrameGraphPass
{
public:
    /// @brief Clear Color を保持し、Resource は setup で名前から取得する
    explicit ClearFinalColorPass(std::array<float, 4> a_clearColor) noexcept
        : m_clearColor(std::move(a_clearColor))
    {
    }

    /// @brief 診断用の名前を返す
    [[nodiscard]] const char* name() const noexcept override
    {
        return "ClearFinalColor";
    }

    /// @brief RenderTarget を扱う Queue を返す
    [[nodiscard]] QueueType type() const noexcept override
    {
        return QueueType::Graphics;
    }

    /// @brief 名前付き FinalColorTexture を取得する
    [[nodiscard]] Result<void> setup(FrameGraphBuilder& a_builder) override
    {
        auto result = a_builder.get_texture("FinalColorTexture");
        if (!result.has_value())
        {
            return Result<void>::failure(*result.try_error());
        }
        m_finalColor = result.take_value();
        return Result<void>::success();
    }

    /// @brief Clear 対象の Write と State を宣言する
    [[nodiscard]] Result<void> describe_resources(FrameGraphBuilder& a_builder) override
    {
        return a_builder.use(m_finalColor, FrameGraphAccess::Write,
                             FrameGraphResourceState::RenderTarget);
    }

    /// @brief Backend Context に Clear を記録する
    [[nodiscard]] Result<void> execute(FrameGraphContext& a_context) override
    {
        return a_context.clear_render_target(m_finalColor, m_clearColor);
    }

private:
    FrameGraphResourceHandle m_finalColor;
    std::array<float, 4> m_clearColor{};
};
} // namespace cue
