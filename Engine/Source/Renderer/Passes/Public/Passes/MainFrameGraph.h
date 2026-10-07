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
///
/// 初回構築時と Resize 時に、それぞれの呼出 Thread で実行される。毎回新しい Pass を作る
using frameGraphConfigure = std::function<Result<void>(FrameGraph&, FrameGraphResourceHandle)>;

/// @brief Host が追加描画と最後の表示 Pass を選ぶ Backend 非依存の構築設定
///
/// displayPass の所有権は構築先へ移る。未指定なら標準の全画面表示を使う
/// Pass が借用する UI 等は Graph の停止と破棄より長く生存させる
/// displayPassFactory と Pass の setup は構築の呼出 Thread、旧 Pass の破棄は Resize の呼出 Thread で行われる
/// 呼出側は Graph の execute を静止させ、構築と Resize を直列化する
struct MainFrameGraphConfig final
{
    frameGraphConfigure configure;
    std::unique_ptr<FrameGraphPass> displayPass;
    // 再構築する Graph ごとに新しい Pass を渡す。displayPass 指定時は初回の実体を優先する
    // 生成した Pass の setup / describe_resources も構築の呼出 Thread で実行される
    std::function<std::unique_ptr<FrameGraphPass>()> displayPassFactory;
};

/// @brief 表示 Pass を所有する Graph と、Backend が物理化する Resource Handle
struct FrameGraphComposition final
{
    std::unique_ptr<FrameGraph> graph;
    FrameGraphResourceHandle finalColor;
    FrameGraphResourceHandle backBuffer;
};

/// @brief FinalColorTexture、BackBuffer、Clear と表示 Pass を抽象層で組み立てる
///
/// 呼出側が Graph を一意所有し、Resource の物理化と Queue 提出は Backend が担う
/// 構築は同一 Thread で行い、失敗時は注入した Pass と途中生成物も破棄する
[[nodiscard]] inline Result<FrameGraphComposition> create_main_frame_graph(GpuTexture2DDesc a_colorDesc,
                                                                           const FrameGraphBuildContext &a_context,
                                                                           MainFrameGraphConfig a_config = {})
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
        auto clear = std::make_unique<ClearFinalColorPass>(a_colorDesc.clearColor);
        const auto* clearPass = clear.get();
        auto clearResult = graph->add_pass(std::move(clear));
        if (!clearResult.has_value())
        {
            return GraphResult::failure(*clearResult.try_error());
        }
        // Clear、呼出側の追加描画、選択した表示の順に一つの Graph へ所有権を移す
        if (a_config.configure)
        {
            auto configureResult = a_config.configure(*graph, finalColor);
            if (!configureResult.has_value())
            {
                return GraphResult::failure(*configureResult.try_error());
            }
        }
        if (!a_config.displayPass && a_config.displayPassFactory)
        {
            a_config.displayPass = a_config.displayPassFactory();
            if (!a_config.displayPass)
            {
                return GraphResult::failure({ErrorCategory::InvalidState, "create_main_frame_graph.display_factory"});
            }
        }
        if (!a_config.displayPass)
        {
            a_config.displayPass = std::make_unique<PresentToSwapChainPass>();
        }
        // 具体型や名前には依存せず、選択した Pass 自体が最後に残ることを検証する
        const auto* displayPass = a_config.displayPass.get();
        auto presentResult = graph->add_pass(std::move(a_config.displayPass));
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
        if (!plan || plan->passes().size() < 2 ||
            graph->pass(plan->passes().front().handle) != clearPass ||
            graph->pass(plan->passes().back().handle) != displayPass)
        {
            return GraphResult::failure({ErrorCategory::InvalidState, "create_main_frame_graph.plan"});
        }
        // 表示先は Graphics の BackBuffer 書込みとし、Present への復帰は Graph の終了 Barrier に任せる
        const auto& display = plan->passes().back();
        bool writesBackBuffer = false;
        for (const auto& use : display.uses)
        {
            if (use.resource.graphId == backBuffer.graphId && use.resource.index == backBuffer.index &&
                use.access == FrameGraphAccess::Write && use.state == FrameGraphResourceState::RenderTarget)
            {
                writesBackBuffer = true;
            }
        }
        if (display.queue != QueueType::Graphics || !writesBackBuffer)
        {
            return GraphResult::failure({ErrorCategory::InvalidArgument, "create_main_frame_graph.display"});
        }
        return GraphResult::success({std::move(graph), finalColor, backBuffer});
    }
    catch (const std::bad_alloc&)
    {
        return GraphResult::failure({ErrorCategory::PlatformFailure, "create_main_frame_graph.allocation"});
    }
    catch (...)
    {
        return GraphResult::failure({ErrorCategory::PlatformFailure, "create_main_frame_graph.callback.exception"});
    }
}
} // namespace cue
