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

    // Windowより先にGPU資源を停止し、停止の再呼出を許す
    if (!renderer->shutdown().has_value() || !renderer->shutdown().has_value() ||
        !window->destroy().has_value())
    {
        return 7;
    }
    window.reset();
    system.reset();
    return 0;
}
