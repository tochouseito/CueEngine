#include <FrameGraph/FrameGraph.h>
#include <Passes/ClearFinalColorPass.h>
#include <Passes/MainFrameGraph.h>
#include <Passes/PresentToSwapChainPass.h>

#include <memory>

#include "TestPipelineManager.h"

namespace
{
/// @brief 先行 Pass が作った Pipeline を残さず Build 失敗で回収するための Pass
class FailingPass final : public cue::FrameGraphPass
{
  public:
    /// @brief 診断用の名前を返す
    [[nodiscard]] const char *name() const noexcept override
    {
        return "Failing";
    }
    /// @brief Graphics Graph と同じ Queue を指定する
    [[nodiscard]] cue::QueueType type() const noexcept override
    {
        return cue::QueueType::Graphics;
    }
    /// @brief setup は成功し、後続の Access 宣言失敗を検証する
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder &) override
    {
        return cue::Result<void>::success();
    }
    /// @brief Pipeline 生成後の失敗を Graph の Build 境界へ返す
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder &) override
    {
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "Test.describe.failure"});
    }
    /// @brief Build 失敗した Pass は実行されない
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext &) override
    {
        return cue::Result<void>::success();
    }
};

/// @brief Build 前の生成拒否、Pass 設定の伝達と失敗時の即時回収を確認する
int run_tests()
{
    cue::GpuTexture2DDesc color{8, 8};
    color.isRenderTarget = true;
    color.format = cue::GpuTextureFormat::Bgra8Unorm;
    TestPipelineManager manager;
    auto builderResult = cue::FrameGraphBuilder::create({manager});
    if (!builderResult.has_value())
        return 1;
    auto builder = builderResult.take_value();
    if (builder->create_root_signature({}).has_value() || !manager.roots.empty())
        return 2;
    for (int phase = 0; phase < 3; ++phase)
    {
        TestPipelineManager failing;
        failing.failShaderCall = phase < 2 ? phase + 1 : 0;
        failing.shouldFailPipeline = phase == 2;
        auto result = cue::create_main_frame_graph(color, {failing});
        if (result.has_value() || failing.activeRoots || failing.activeShaders || failing.activePipelines)
            return 3 + phase;
    }
    auto result = cue::create_main_frame_graph(color, {manager});
    if (!result.has_value() || manager.activeRoots != 1 || manager.activeShaders != 2 || manager.activePipelines != 1 ||
        manager.roots[0].parameters[0].type != cue::RootParameterType::SrvTable ||
        manager.roots[0].samplers.size() != 1 || manager.shaders[0].stage != cue::ShaderStage::Vertex ||
        manager.shaders[1].stage != cue::ShaderStage::Pixel || manager.graphics[0].renderTargetFormat != color.format)
        return 6;
    auto composition = result.take_value();
    composition.graph.reset();
    if (manager.activeRoots || manager.activeShaders || manager.activePipelines)
        return 7;
    TestPipelineManager rollback;
    const cue::FrameGraphBuildContext context{rollback};
    auto rollbackBuilder = cue::FrameGraphBuilder::create_main(color, &context);
    if (!rollbackBuilder.has_value())
        return 8;
    auto ownedBuilder = rollbackBuilder.take_value();
    if (!ownedBuilder
             ->import_texture2d("BackBuffer", color, cue::FrameGraphResourceState::Present,
                                cue::FrameGraphResourceState::Present)
             .has_value())
        return 9;
    auto graphResult = cue::FrameGraph::create(std::move(ownedBuilder), 8, 8);
    if (!graphResult.has_value())
        return 10;
    auto graph = graphResult.take_value();
    if (!graph->add_pass(std::make_unique<cue::ClearFinalColorPass>(color.clearColor)).has_value() ||
        !graph->add_pass(std::make_unique<cue::PresentToSwapChainPass>()).has_value() ||
        !graph->add_pass(std::make_unique<FailingPass>()).has_value())
        return 11;
    auto failed = graph->build();
    if (failed.has_value() || failed.try_error()->operation != "Test.describe.failure" || graph->plan() ||
        rollback.activeRoots || rollback.activeShaders || rollback.activePipelines || rollback.graphics.size() != 1 ||
        graph->build().has_value())
        return 12;
    return 0;
}
} // namespace

/// @brief 抽象層だけで Pipeline の生成責任と回収境界を検証する
int main()
{
    return run_tests();
}
