#include <Cue/Renderer/D3D12/D3D12Renderer.h>

#include <Cue/Platform/Windows/WindowsPlatform.h>

/// @brief WARPで実WindowのD3D12資源を生成しGPU待機後に解放する
int main()
{
    // 無効なHandleはDevice生成前に拒否する
    auto invalid = cue::D3D12Renderer::create(nullptr, {640, 480}, true);
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

    // 実Windowに対して明示WARP経路とFrame Resourceの生成を確認する
    auto rendererResult = cue::D3D12Renderer::create(handleResult.take_value(), window->client_size(), true);
    if (!rendererResult.has_value())
    {
        return 5;
    }
    auto renderer = rendererResult.take_value();
    if (!renderer->is_warp())
    {
        return 6;
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

    // Windowより先にGPU資源を停止し、停止の再呼出を許す
    if (!renderer->shutdown().has_value() || !renderer->shutdown().has_value())
    {
        return 7;
    }
    auto stoppedFrame = renderer->render_frame(8);
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
    auto implicitResult = cue::D3D12Renderer::create(implicitHandle.take_value(), window->client_size(), true);
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
