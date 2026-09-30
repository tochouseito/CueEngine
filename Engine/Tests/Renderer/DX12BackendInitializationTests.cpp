#include <Cue/Renderer/DX12/DX12Backend.h>
#include <Cue/Renderer/FrameGraph/FrameGraphRuntime.h>

#include <Cue/Platform/Windows/WindowsPlatform.h>

namespace
{
/// @brief Backend 外で構築した Graph から現行 Surface を Clear する
class ExternalClearPass final : public cue::FrameGraphPass
{
public:
    /// @brief 計画と GPU 診断で使う Pass 名を返す
    [[nodiscard]] const char* name() const noexcept override { return "ExternalClear"; }

    /// @brief 表示 Surface の書込みと終了時 COMMON への復帰を宣言する
    [[nodiscard]] cue::Result<cue::GraphPassHandle> setup(cue::FrameGraphBuilder& a_builder) const override
    {
        return a_builder.add_pass(name(), {{m_color, cue::GraphResourceState::RenderTarget,
                                             cue::GraphAccess::Write}});
    }

    /// @brief Resize 後も Context が渡す当該 Frame の RTV を使う
    [[nodiscard]] cue::Result<void> execute(cue::FrameGraphContext& a_context) const override
    {
        return a_context.commands().clear_color(a_context.surface_color_view(),
                                                 {0.0f, 0.25f, 0.5f, 1.0f});
    }

    /// @brief 構築済み Surface Resource Handle を保持する
    explicit ExternalClearPass(cue::GraphResourceHandle a_color) noexcept : m_color(a_color) {}

private:
    cue::GraphResourceHandle m_color;
};
} // namespace

/// @brief 利用可能な Adapter で実 Window の DX12 資源を生成し GPU 待機後に解放する
int main()
{
    // 無効なHandleはDevice生成前に拒否する
    auto invalid = cue::DX12Backend::create(nullptr, {640, 480});
    if (invalid.has_value() || invalid.try_error()->category != cue::ErrorCategory::InvalidArgument)
    {
        return 1;
    }

    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 2;
    }
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"CueEngine Renderer Initialization", {640, 480}});
    if (!windowResult.has_value())
    {
        return 3;
    }
    auto window = windowResult.take_value();
    auto handleResult = cue::borrow_windows_window_handle(*window);
    if (!handleResult.has_value())
    {
        return 4;
    }

    // 実 Window に対して選択された Adapter で Frame Resource を生成する
    auto rendererResult = cue::DX12Backend::create(handleResult.take_value(), window->client_size());
    if (!rendererResult.has_value())
    {
        return 5;
    }
    auto renderer = rendererResult.take_value();
    // Backend が所有する Manager 経由で Buffer、Texture、View を作成し寿命順に解放する
    auto* buffers = renderer->get_buffer_manager();
    auto* textures = renderer->get_texture_manager();
    auto* views = renderer->get_view_manager();
    if (!buffers || !textures || !views || !renderer->get_queue_pool() || !renderer->get_render_device() ||
        !renderer->get_pipeline_manager() || !renderer->get_command_pool() ||
        renderer->get_render_device()->is_software_adapter() != renderer->is_warp())
    {
        return 25;
    }
    auto bufferResult = buffers->create_buffer({256, cue::GpuMemory::Upload, false, "TestBuffer"});
    auto textureResult = textures->create_texture({8, 8, cue::GpuTextureFormat::Rgba8Unorm,
                                                    false, "TestTexture"});
    if (!bufferResult.has_value() || !textureResult.has_value())
    {
        return 26;
    }
    const auto buffer = bufferResult.take_value();
    const auto texture = textureResult.take_value();
    auto namedBuffer = buffers->get_buffer("TestBuffer");
    auto namedTexture = textures->get_texture("TestTexture");
    if (!namedBuffer.has_value() || !namedTexture.has_value() ||
        namedBuffer.try_value()->generation != buffer.generation ||
        namedTexture.try_value()->generation != texture.generation ||
        buffers->create_buffer({256, cue::GpuMemory::Upload, false, "TestBuffer"}).has_value() ||
        textures->create_texture({8, 8, cue::GpuTextureFormat::Rgba8Unorm,
                                  false, "TestTexture"}).has_value())
    {
        return 30;
    }
    if (buffers->destroy_buffer(texture).has_value() || textures->destroy_texture(buffer).has_value())
    {
        return 29;
    }
    cue::GpuViewDesc viewDesc{cue::GpuViewKind::RenderTarget};
    viewDesc.name = "TestView";
    auto viewResult = views->create_view(texture, viewDesc);
    auto namedView = views->get_view("TestView");
    if (!viewResult.has_value() || !namedView.has_value() ||
        namedView.try_value()->generation != viewResult.try_value()->generation ||
        views->create_view(texture, viewDesc).has_value() ||
        textures->destroy_texture(texture).has_value() ||
        !textures->get_texture("TestTexture").has_value() ||
        !views->destroy_view(viewResult.take_value()).has_value() ||
        !buffers->destroy_buffer(buffer).has_value() || !textures->destroy_texture(texture).has_value())
    {
        return 27;
    }
    if (buffers->get_buffer("TestBuffer").has_value() ||
        textures->get_texture("TestTexture").has_value() ||
        views->get_view("TestView").has_value())
    {
        return 31;
    }
    // Bufferを繰り返し使用し、同一Frameの二重Submitを拒否する
    if (!window->show().has_value() || !renderer->render_frame(0).has_value() ||
        !renderer->render_frame(1).has_value() || !renderer->render_frame(2).has_value())
    {
        return 8;
    }
    auto duplicate = renderer->render_frame(2);
    if (duplicate.has_value() || duplicate.try_error()->category != cue::ErrorCategory::InvalidState)
    {
        return 9;
    }

    // Resize連打の最終Sizeが適用され、最小化中はPresent数が増えないことを確認する
    if (!renderer->request_surface({800, 600}, false).has_value() ||
        !renderer->request_surface({1024, 768}, false).has_value() ||
        !renderer->render_frame(3).has_value())
    {
        return 10;
    }
    auto resized = renderer->progress();
    if (!resized.has_value() || resized.try_value()->surfaceSize.width != 1024 ||
        resized.try_value()->surfaceSize.height != 768 || resized.try_value()->presentedFrames != 4)
    {
        return 12;
    }
    if (!renderer->request_surface({}, true).has_value() || !renderer->render_frame(4).has_value())
    {
        return 13;
    }
    auto minimized = renderer->progress();
    if (!minimized.has_value() || minimized.try_value()->presentedFrames != 4)
    {
        return 14;
    }
    if (!renderer->request_surface({640, 480}, false).has_value() || !renderer->render_frame(5).has_value())
    {
        return 15;
    }
    auto restored = renderer->progress();
    if (!restored.has_value() || restored.try_value()->surfaceSize.width != 640 ||
        restored.try_value()->surfaceSize.height != 480 || restored.try_value()->presentedFrames != 5)
    {
        return 16;
    }
    // Client Sizeが0の間はSwap Chainを触らず、非0へ戻った後に描画を再開する
    if (!renderer->request_surface({}, false).has_value() || !renderer->render_frame(6).has_value())
    {
        return 11;
    }
    auto zeroSize = renderer->progress();
    if (!zeroSize.has_value() || zeroSize.try_value()->presentedFrames != 5)
    {
        return 17;
    }
    if (!renderer->request_surface({640, 480}, false).has_value() ||
        !renderer->render_frame(7).has_value())
    {
        return 18;
    }
    auto resumed = renderer->progress();
    if (!resumed.has_value() || resumed.try_value()->presentedFrames != 6)
    {
        return 19;
    }

    // 外部 Graph は同じ論理 Handle で Resize 後の物理 Surface を再解決する
    cue::FrameGraph externalGraph;
    auto externalColorResult = externalGraph.builder().create_resource(
        "ExternalSurface", cue::GraphResourceLifetime::Transient,
        cue::GraphResourceState::Common, cue::GraphResourceState::Common);
    if (!externalColorResult.has_value())
    {
        return 32;
    }
    const auto externalColor = externalColorResult.take_value();
    if (!externalGraph.bind_surface_color(externalColor).has_value() ||
        !externalGraph.add_pass(std::make_unique<ExternalClearPass>(externalColor)).has_value() ||
        !renderer->render_graph_frame(8, externalGraph).has_value() ||
        !renderer->request_surface({800, 600}, false).has_value() ||
        !renderer->render_graph_frame(9, externalGraph).has_value())
    {
        return 33;
    }
    auto externalProgress = renderer->progress();
    const auto externalStats = externalGraph.execution_stats_copy();
    if (!externalProgress.has_value() || externalProgress.try_value()->surfaceSize.width != 800 ||
        externalProgress.try_value()->presentedFrames != 8 ||
        externalStats.passStats.size() != 1 || externalStats.passStats[0].name != "ExternalClear" ||
        externalStats.totalExecuteMs <= 0.0)
    {
        return 34;
    }
    // 不正な Depth 最終状態は Submit 前に拒否し、既存 Graph へ戻せる
    cue::FrameGraph invalidDepthGraph;
    auto invalidColor = invalidDepthGraph.builder().create_resource(
        "InvalidColor", cue::GraphResourceLifetime::Transient,
        cue::GraphResourceState::Common, cue::GraphResourceState::Common);
    auto invalidDepth = invalidDepthGraph.builder().create_resource(
        "InvalidDepth", cue::GraphResourceLifetime::Persistent,
        cue::GraphResourceState::Common, cue::GraphResourceState::DepthWrite);
    if (!invalidColor.has_value() || !invalidDepth.has_value())
    {
        return 35;
    }
    const auto invalidColorHandle = invalidColor.take_value();
    if (!invalidDepthGraph.bind_surface_color(invalidColorHandle).has_value() ||
        !invalidDepthGraph.bind_surface_depth(invalidDepth.take_value()).has_value() ||
        !invalidDepthGraph.add_pass(std::make_unique<ExternalClearPass>(invalidColorHandle)).has_value() ||
        renderer->render_graph_frame(10, invalidDepthGraph).has_value() ||
        !renderer->render_frame(10).has_value())
    {
        return 36;
    }

    // Windowより先にGPU資源を停止し、停止の再呼出を許す
    if (!renderer->shutdown().has_value() || !renderer->shutdown().has_value())
    {
        return 7;
    }
    if (renderer->get_buffer_manager() || renderer->get_texture_manager() || renderer->get_view_manager() ||
        renderer->get_queue_pool() || renderer->get_render_device() || renderer->get_pipeline_manager() ||
        renderer->get_command_pool())
    {
        return 28;
    }
    auto stoppedFrame = renderer->render_frame(11);
    if (stoppedFrame.has_value() || stoppedFrame.try_error()->category != cue::ErrorCategory::InvalidState)
    {
        return 20;
    }

    // 明示停止を省いても GPU 完了を待ってから Window 依存資源を回収する
    auto implicitHandle = cue::borrow_windows_window_handle(*window);
    if (!implicitHandle.has_value())
    {
        return 21;
    }
    auto implicitResult = cue::DX12Backend::create(implicitHandle.take_value(), window->client_size());
    if (!implicitResult.has_value())
    {
        return 22;
    }
    auto implicitRenderer = implicitResult.take_value();
    if (!implicitRenderer->render_frame(0).has_value())
    {
        return 23;
    }
    implicitRenderer.reset();
    if (!window->destroy().has_value())
    {
        return 24;
    }
    window.reset();
    system.reset();
    return 0;
}
