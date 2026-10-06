#include <EditorHost/ImGuiPass.h>
#include <Passes/MainFrameGraph.h>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

#include "../Renderer/TestPipelineManager.h"

namespace
{
/// @brief Pass の Execute 契約を GPU なしで確認する Command
class TestCommand final : public cue::ICommandContext
{
  public:
    /// @brief UI を記録する Graphics Command として扱う
    [[nodiscard]] cue::QueueType type() const noexcept override
    {
        return cue::QueueType::Graphics;
    }
    /// @brief Pass の実行中は記録状態を維持する
    [[nodiscard]] cue::CommandState state() const noexcept override
    {
        return cue::CommandState::Recording;
    }
    /// @brief GPU を持たない Command の記録終了を受け付ける
    [[nodiscard]] cue::Result<void> close() override
    {
        return cue::Result<void>::success();
    }
};

/// @brief 基底 Context の Frame 情報を検査する
class TestContext final : public cue::FrameGraphContext
{
  public:
    /// @brief テスト用 Command を記録中だけ借用する
    TestContext(cue::ICommandContext &a_command) noexcept : FrameGraphContext(8, 8, 1, a_command)
    {
    }

    /// @brief Native API を使わずに Clear の宣言内容を記録する
    [[nodiscard]] cue::Result<void> clear_render_target(cue::FrameGraphResourceHandle a_target,
                                                        const std::array<float, 4> &a_color) override
    {
        clearTarget = a_target;
        clearColor = a_color;
        ++clearCount;
        return cue::Result<void>::success();
    }

    /// @brief 抽象 Context の描画先指定を GPU なしで受け付ける
    [[nodiscard]] cue::Result<void> set_render_target(cue::FrameGraphResourceHandle a_target) override
    {
        drawTarget = a_target;
        return cue::Result<void>::success();
    }

    /// @brief 抽象 Context の Shader 入力指定を GPU なしで受け付ける
    [[nodiscard]] cue::Result<void> bind_texture2d(cue::FrameGraphResourceHandle a_source, std::uint32_t) override
    {
        drawSource = a_source;
        return cue::Result<void>::success();
    }

    /// @brief 表示 Pass の Pipeline 設定を確認する
    [[nodiscard]] cue::Result<void> set_graphics_pipeline(cue::PipelineStateHandle a_pipeline) override
    {
        return a_pipeline.is_valid()
                   ? cue::Result<void>::success()
                   : cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "Test.pipeline"});
    }
    /// @brief Graph 全体の Viewport 指定を確認する
    [[nodiscard]] cue::Result<void> set_viewport_scissor(std::uint32_t a_width, std::uint32_t a_height) override
    {
        return a_width == width() && a_height == height()
                   ? cue::Result<void>::success()
                   : cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "Test.viewport"});
    }
    /// @brief 全画面表示の三頂点 Draw を確認する
    [[nodiscard]] cue::Result<void> draw_instanced(std::uint32_t a_vertices, std::uint32_t a_instances, std::uint32_t,
                                                   std::uint32_t) override
    {
        ++drawCount;
        return a_vertices == 3 && a_instances == 1
                   ? cue::Result<void>::success()
                   : cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "Test.draw"});
    }

    /// @brief Native API を使わずに Copy の宣言内容を記録する
    [[nodiscard]] cue::Result<void> copy_texture2d(cue::FrameGraphResourceHandle a_source,
                                                   cue::FrameGraphResourceHandle a_destination) override
    {
        copySource = a_source;
        copyDestination = a_destination;
        ++copyCount;
        return cue::Result<void>::success();
    }

    cue::FrameGraphResourceHandle clearTarget;
    cue::FrameGraphResourceHandle copySource;
    cue::FrameGraphResourceHandle copyDestination;
    cue::FrameGraphResourceHandle drawSource;
    cue::FrameGraphResourceHandle drawTarget;
    std::array<float, 4> clearColor{};
    int clearCount = 0;
    int copyCount = 0;
    int drawCount = 0;
};

/// @brief UI Owner の借用先と Adapter の Graph 所有・失敗伝播を検査する
class TestImGuiRenderer final : public cue::IImGuiRenderer
{
  public:
    /// @brief Pass 破棄より長く生存する検査値を借用する
    TestImGuiRenderer(int &a_destroyed, bool &a_fails) noexcept : m_destroyed(&a_destroyed), m_fails(&a_fails)
    {
    }

    /// @brief Graph 停止で Adapter が一度だけ回収されることを確認する
    ~TestImGuiRenderer() override
    {
        ++*m_destroyed;
    }

    /// @brief UI 記録入口で Target の準備を確認し、失敗時は元の Error を返す
    [[nodiscard]] cue::Result<void> record_draw_data(cue::FrameGraphContext &a_context) override
    {
        auto &context = static_cast<TestContext &>(a_context);
        if (!context.drawTarget.is_valid() || context.clearTarget.index != context.drawTarget.index ||
            context.clearCount != 1)
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.ui_target"});
        }
        return *m_fails ? cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Test.ui_record"})
                        : cue::Result<void>::success();
    }

  private:
    int *m_destroyed;
    bool *m_fails;
};

/// @brief Native UI 型なしで ImGuiPass の Plan、失敗伝播と Adapter 回収を確認する
int test_imgui_pass()
{
    TestPipelineManager pipelines;
    cue::GpuTexture2DDesc color{8, 8};
    color.isRenderTarget = true;
    color.clearColor = {0.2f, 0.4f, 0.6f, 1.0f};
    int destroyed = 0;
    bool fails = false;
    cue::MainFrameGraphConfig config;
    config.displayPass = std::make_unique<cue::ImGuiPass>(std::make_unique<TestImGuiRenderer>(destroyed, fails));
    auto built = cue::create_main_frame_graph(color, {pipelines}, std::move(config));
    if (!built.has_value())
    {
        return 1;
    }
    auto graph = built.take_value();
    const auto &pass = graph.graph->plan()->passes().back();
    if (pass.name != "ImGui" || pass.uses.size() != 1 || pass.uses[0].resource.index != graph.backBuffer.index ||
        pass.uses[0].access != cue::FrameGraphAccess::Write ||
        pass.uses[0].state != cue::FrameGraphResourceState::RenderTarget)
    {
        return 2;
    }
    TestCommand command;
    TestContext context(command);
    if (!graph.graph->pass(pass.handle)->execute(context).has_value() || context.clearColor != color.clearColor)
    {
        return 3;
    }
    fails = true;
    TestContext failedContext(command);
    auto recorded = graph.graph->pass(pass.handle)->execute(failedContext);
    if (recorded.has_value() || recorded.try_error()->operation != "Test.ui_record")
    {
        return 4;
    }
    graph.graph.reset();
    cue::MainFrameGraphConfig missing;
    missing.displayPass = std::make_unique<cue::ImGuiPass>(nullptr);
    auto invalid = cue::create_main_frame_graph(color, {pipelines}, std::move(missing));
    if (destroyed != 1 || invalid.has_value() || invalid.try_error()->operation != "ImGuiPass.renderer")
    {
        return 5;
    }
    return 0;
}
} // namespace

/// @brief Editor 所有の Pass を抽象 Graph へ注入する契約を検証する
int main()
{
    return test_imgui_pass();
}
