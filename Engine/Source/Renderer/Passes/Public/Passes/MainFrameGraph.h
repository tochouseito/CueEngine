#pragma once

#include <functional>
#include <memory>
#include <new>
#include <utility>

#include <Passes/ClearFinalColorPass.h>
#include <Passes/PresentToSwapChainPass.h>

namespace cue
{
/// @brief Clear と表示の間に Pass を追加する Backend 非依存の設定 Callback
using frameGraphConfigure = std::function<Result<void>(FrameGraph&, FrameGraphResourceHandle)>;

/// @brief 固定 Pass を持つ Graph と、Backend が物理化する Resource Handle
struct FrameGraphComposition final
{
    std::unique_ptr<FrameGraph> graph;
    FrameGraphResourceHandle finalColor;
    FrameGraphResourceHandle backBuffer;
};

/// @brief FinalColorTexture、BackBuffer、Clear と表示 Pass を抽象層で組み立てる
///
/// 呼出側が Graph を一意所有し、Resource の物理化と Queue 提出は Backend が担う
[[nodiscard]] inline Result<FrameGraphComposition> create_main_frame_graph(GpuTexture2DDesc a_colorDesc,
                                                                           const FrameGraphBuildContext &a_context,
                                                                           frameGraphConfigure a_configure = {})
{
    using GraphResult = Result<FrameGraphComposition>;
    auto builderResult = FrameGraphBuilder::create_main(a_colorDesc, &a_context);
    if (!builderResult.has_value())
    {
        return GraphResult::failure(*builderResult.try_error());
    }
    auto builder = builderResult.take_value();
    const auto finalColor = builder->final_color();
    auto backResult = builder->import_texture2d(
        "BackBuffer", a_colorDesc, FrameGraphResourceState::Present, FrameGraphResourceState::Present);
    if (!backResult.has_value())
    {
        return GraphResult::failure(*backResult.try_error());
    }
    const auto backBuffer = backResult.take_value();
    auto graphResult = FrameGraph::create(std::move(builder), a_colorDesc.width, a_colorDesc.height);
    if (!graphResult.has_value())
    {
        return GraphResult::failure(*graphResult.try_error());
    }
    auto graph = graphResult.take_value();
    try
    {
        auto clearResult = graph->add_pass(std::make_unique<ClearFinalColorPass>(a_colorDesc.clearColor));
        if (!clearResult.has_value())
        {
            return GraphResult::failure(*clearResult.try_error());
        }
        if (a_configure)
        {
            auto configureResult = a_configure(*graph, finalColor);
            if (!configureResult.has_value())
            {
                return GraphResult::failure(*configureResult.try_error());
            }
        }
        auto presentResult = graph->add_pass(std::make_unique<PresentToSwapChainPass>());
        if (!presentResult.has_value())
        {
            return GraphResult::failure(*presentResult.try_error());
        }
        auto buildResult = graph->build();
        if (!buildResult.has_value())
        {
            return GraphResult::failure(*buildResult.try_error());
        }
        const auto* plan = graph->plan();
        if (!plan || plan->passes().size() < 2 || plan->passes().front().name != "ClearFinalColor" ||
            plan->passes().back().name != "PresentToSwapChain")
        {
            return GraphResult::failure({ErrorCategory::InvalidState, "create_main_frame_graph.plan"});
        }
        return GraphResult::success({std::move(graph), finalColor, backBuffer});
    }
    catch (const std::bad_alloc&)
    {
        return GraphResult::failure({ErrorCategory::PlatformFailure, "create_main_frame_graph.allocation"});
    }
}
} // namespace cue
