#include <FrameGraph/FrameGraph.h>
#include <Passes/ClearFinalColorPass.h>
#include <Passes/MainFrameGraph.h>
#include <Passes/PresentToSwapChainPass.h>

#include <array>
#include <cstdint>
#include <memory>

namespace
{
/// @brief Pass の Execute 契約を GPU なしで確認する Command
class TestCommand final : public cue::ICommandContext
{
public:
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Graphics; }
    [[nodiscard]] cue::CommandState state() const noexcept override { return cue::CommandState::Recording; }
    [[nodiscard]] cue::Result<void> close() override { return cue::Result<void>::success(); }
};

/// @brief 基底 Context の Frame 情報を検査する
class TestContext final : public cue::FrameGraphContext
{
public:
    /// @brief テスト用 Command を記録中だけ借用する
    TestContext(cue::ICommandContext& a_command) noexcept
        : FrameGraphContext(8, 8, 1, a_command)
    {
    }

    /// @brief Native API を使わずに Clear の宣言内容を記録する
    [[nodiscard]] cue::Result<void> clear_render_target(
        cue::FrameGraphResourceHandle a_target, const std::array<float, 4>& a_color) override
    {
        clearTarget = a_target;
        clearColor = a_color;
        ++clearCount;
        return cue::Result<void>::success();
    }

    /// @brief 抽象 Context の描画先指定を GPU なしで受け付ける
    [[nodiscard]] cue::Result<void> set_render_target(cue::FrameGraphResourceHandle) override
    {
        return cue::Result<void>::success();
    }

    /// @brief 抽象 Context の Shader 入力指定を GPU なしで受け付ける
    [[nodiscard]] cue::Result<void> bind_texture2d(cue::FrameGraphResourceHandle,
                                                   std::uint32_t) override
    {
        return cue::Result<void>::success();
    }

    /// @brief Native API を使わずに Copy の宣言内容を記録する
    [[nodiscard]] cue::Result<void> copy_texture2d(
        cue::FrameGraphResourceHandle a_source, cue::FrameGraphResourceHandle a_destination) override
    {
        copySource = a_source;
        copyDestination = a_destination;
        ++copyCount;
        return cue::Result<void>::success();
    }

    cue::FrameGraphResourceHandle clearTarget;
    cue::FrameGraphResourceHandle copySource;
    cue::FrameGraphResourceHandle copyDestination;
    std::array<float, 4> clearColor{};
    int clearCount = 0;
    int copyCount = 0;
};

/// @brief 名前付き Buffer を構築し、後続 Pass の読み取り元にする
class SeedPass final : public cue::FrameGraphPass
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "Seed"; }
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Graphics; }
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder& a_builder) override
    {
        auto result = a_builder.create_transient_buffer("Scratch", {64});
        if (!result.has_value())
        {
            return cue::Result<void>::failure(*result.try_error());
        }
        m_buffer = result.take_value();
        return cue::Result<void>::success();
    }
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder& a_builder) override
    {
        return a_builder.use(m_buffer, cue::FrameGraphAccess::Write,
                             cue::FrameGraphResourceState::CopyDestination);
    }
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext& a_context) override
    {
        return a_context.width() == 8 && a_context.height() == 8 && a_context.frame_index() == 1 ?
            cue::Result<void>::success() :
            cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "SeedPass.execute"});
    }

private:
    cue::FrameGraphResourceHandle m_buffer;
};

/// @brief 先行 Pass が名前を付けた Buffer を参照する
class ReadPass final : public cue::FrameGraphPass
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "Read"; }
    [[nodiscard]] cue::QueueType type() const noexcept override { return cue::QueueType::Graphics; }
    [[nodiscard]] cue::Result<void> setup(cue::FrameGraphBuilder& a_builder) override
    {
        auto result = a_builder.get_buffer("Scratch");
        if (!result.has_value())
        {
            return cue::Result<void>::failure(*result.try_error());
        }
        m_buffer = result.take_value();
        return cue::Result<void>::success();
    }
    [[nodiscard]] cue::Result<void> describe_resources(cue::FrameGraphBuilder& a_builder) override
    {
        return a_builder.use(m_buffer, cue::FrameGraphAccess::Read,
                             cue::FrameGraphResourceState::CopySource);
    }
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext&) override
    {
        return cue::Result<void>::success();
    }

private:
    cue::FrameGraphResourceHandle m_buffer;
};
} // namespace

/// @brief Legacy Pass の二段階宣言が名前と Hazard に結びつくことを確認する
int main()
{
    auto builderResult = cue::FrameGraphBuilder::create();
    if (!builderResult.has_value())
    {
        return 1;
    }
    auto graphResult = cue::FrameGraph::create(builderResult.take_value(), 8, 8);
    if (!graphResult.has_value())
    {
        return 2;
    }
    auto graph = graphResult.take_value();
    if (!graph->add_pass(std::make_unique<SeedPass>()).has_value() ||
        !graph->add_pass(std::make_unique<ReadPass>()).has_value() ||
        !graph->build().has_value())
    {
        return 3;
    }
    const auto* plan = graph->plan();
    if (!plan || plan->passes().size() != 2 || plan->resources().size() != 1 ||
        plan->resources()[0].name != "Scratch" || plan->passes()[1].dependencies.size() != 1 ||
        plan->passes()[1].dependencies[0].index != plan->passes()[0].handle.index)
    {
        return 4;
    }
    TestCommand command;
    TestContext context(command);
    auto* seed = graph->pass(plan->passes()[0].handle);
    if (!seed || !seed->execute(context).has_value() ||
        graph->pass({plan->passes()[0].handle.graphId + 1, 0}) ||
        graph->add_pass(std::make_unique<ReadPass>()).has_value())
    {
        return 5;
    }
    auto aliasBuilderResult = cue::FrameGraphBuilder::create();
    if (!aliasBuilderResult.has_value())
    {
        return 6;
    }
    auto aliasBuilder = aliasBuilderResult.take_value();
    auto firstResourceResult = aliasBuilder->create_transient_buffer({64});
    auto secondResourceResult = aliasBuilder->create_transient_buffer({64});
    auto firstPassResult = aliasBuilder->add_pass("First", cue::QueueType::Graphics);
    auto secondPassResult = aliasBuilder->add_pass("Second", cue::QueueType::Copy);
    if (!firstResourceResult.has_value() || !secondResourceResult.has_value() ||
        !firstPassResult.has_value() || !secondPassResult.has_value())
    {
        return 7;
    }
    const auto firstResource = firstResourceResult.take_value();
    const auto secondResource = secondResourceResult.take_value();
    const auto firstPass = firstPassResult.take_value();
    const auto secondPass = secondPassResult.take_value();
    if (!aliasBuilder->use(firstPass, firstResource, cue::FrameGraphAccess::Write,
                           cue::FrameGraphResourceState::CopyDestination).has_value() ||
        !aliasBuilder->use(secondPass, secondResource, cue::FrameGraphAccess::Write,
                           cue::FrameGraphResourceState::CopyDestination).has_value())
    {
        return 8;
    }
    auto aliasPlanResult = aliasBuilder->build();
    if (!aliasPlanResult.has_value())
    {
        return 9;
    }
    const auto aliasPlan = aliasPlanResult.take_value();
    if (aliasPlan.alias_slots().size() != 1 || aliasPlan.alias_slots()[0].resources.size() != 2 ||
        aliasPlan.passes()[1].dependencies.size() != 1 ||
        aliasPlan.passes()[1].dependencies[0].index != firstPass.index)
    {
        return 10;
    }
    cue::GpuTexture2DDesc colorDesc{8, 8};
    colorDesc.isRenderTarget = true;
    const std::array<float, 4> clearColor{0.2f, 0.4f, 0.6f, 1.0f};
    colorDesc.clearColor = clearColor;
    bool wasConfigured = false;
    auto compositionResult = cue::create_main_frame_graph(
        colorDesc, [&wasConfigured](cue::FrameGraph&, cue::FrameGraphResourceHandle a_color)
        {
            wasConfigured = a_color.is_valid();
            return cue::Result<void>::success();
        });
    if (!compositionResult.has_value())
    {
        return 11;
    }
    auto composition = compositionResult.take_value();
    if (!wasConfigured || !composition.graph)
    {
        return 12;
    }
    const auto* portablePlan = composition.graph->plan();
    if (!portablePlan || portablePlan->passes().size() != 2 ||
        portablePlan->passes()[0].uses[0].state != cue::FrameGraphResourceState::RenderTarget ||
        portablePlan->passes()[1].uses[0].state != cue::FrameGraphResourceState::CopySource ||
        portablePlan->passes()[1].uses[1].state != cue::FrameGraphResourceState::CopyDestination)
    {
        return 13;
    }
    TestContext portableContext(command);
    auto* clearPass = composition.graph->pass(portablePlan->passes()[0].handle);
    auto* presentPass = composition.graph->pass(portablePlan->passes()[1].handle);
    if (!clearPass || !presentPass ||
        !clearPass->execute(portableContext).has_value() ||
        !presentPass->execute(portableContext).has_value() ||
        portableContext.clearCount != 1 || portableContext.copyCount != 1 ||
        portableContext.clearTarget.graphId != composition.finalColor.graphId ||
        portableContext.clearTarget.index != composition.finalColor.index ||
        portableContext.copySource.index != composition.finalColor.index ||
        portableContext.copyDestination.index != composition.backBuffer.index ||
        portableContext.clearColor != clearColor)
    {
        return 14;
    }
    return 0;
}
