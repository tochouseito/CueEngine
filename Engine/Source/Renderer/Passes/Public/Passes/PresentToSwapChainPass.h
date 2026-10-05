#pragma once

#include <utility>

#include <FrameGraph/FrameGraph.h>

namespace cue
{
/// @brief FinalColorTexture を全画面三角形で BackBuffer へ描画する表示 Pass
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

    /// @brief 表示元と描画先を取得し、Root、HLSL と Graphics Pipeline の生成を依頼する
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
        auto targetDesc = a_builder.texture_desc(m_backBuffer);
        if (!targetDesc.has_value())
            return Result<void>::failure(*targetDesc.try_error());
        RootSignatureDesc rootDesc;
        rootDesc.name = "PresentToSwapChain.Root";
        rootDesc.parameters.push_back({RootParameterType::SrvTable, ShaderVisibility::Pixel, 0});
        rootDesc.samplers.push_back({0, 0, ShaderVisibility::Pixel, SamplerFilter::Linear, SamplerAddressMode::Clamp});
        auto rootResult = a_builder.create_root_signature(std::move(rootDesc));
        if (!rootResult.has_value())
            return Result<void>::failure(*rootResult.try_error());
        auto vsResult = a_builder.create_shader_blob(
            {"PresentToSwapChain.VS", "Hlsl/FullscreenTriangle.hlsl", "vs_main", ShaderStage::Vertex});
        if (!vsResult.has_value())
            return Result<void>::failure(*vsResult.try_error());
        auto psResult = a_builder.create_shader_blob(
            {"PresentToSwapChain.PS", "Hlsl/FullscreenTriangle.hlsl", "ps_main", ShaderStage::Pixel});
        if (!psResult.has_value())
            return Result<void>::failure(*psResult.try_error());
        GraphicsPipelineStateDesc pipelineDesc;
        pipelineDesc.name = "PresentToSwapChain.Pipeline";
        pipelineDesc.rootSignature = rootResult.take_value();
        pipelineDesc.vertexShader = vsResult.take_value();
        pipelineDesc.pixelShader = psResult.take_value();
        pipelineDesc.renderTargetFormat = targetDesc.try_value()->format;
        auto pipelineResult = a_builder.create_graphics_pipeline(std::move(pipelineDesc));
        if (!pipelineResult.has_value())
            return Result<void>::failure(*pipelineResult.try_error());
        m_pipeline = pipelineResult.take_value();
        return Result<void>::success();
    }

    /// @brief ShaderRead と RenderTarget の Access と State を宣言する
    [[nodiscard]] Result<void> describe_resources(FrameGraphBuilder& a_builder) override
    {
        auto sourceResult = a_builder.use(m_finalColor, FrameGraphAccess::Read,
                                          FrameGraphResourceState::ShaderRead);
        if (!sourceResult.has_value())
        {
            return sourceResult;
        }
        return a_builder.use(m_backBuffer, FrameGraphAccess::Write,
                             FrameGraphResourceState::RenderTarget);
    }

    /// @brief 生成済み Pipeline と宣言済み Texture を使い、全画面三角形の Draw を記録する
    [[nodiscard]] Result<void> execute(FrameGraphContext& a_context) override
    {
        auto pipeline = a_context.set_graphics_pipeline(m_pipeline);
        if (!pipeline.has_value())
            return pipeline;
        auto target = a_context.set_render_target(m_backBuffer);
        if (!target.has_value())
            return target;
        auto binding = a_context.bind_texture2d(m_finalColor, 0);
        if (!binding.has_value())
            return binding;
        auto viewport = a_context.set_viewport_scissor(a_context.width(), a_context.height());
        if (!viewport.has_value())
            return viewport;
        return a_context.draw_instanced(3, 1);
    }

private:
  PipelineStateHandle m_pipeline;
  FrameGraphResourceHandle m_finalColor;
  FrameGraphResourceHandle m_backBuffer;
};
} // namespace cue
