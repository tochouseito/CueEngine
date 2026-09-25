#include <Cue/Renderer/Windows/WindowsRenderer.h>

#include <Cue/Platform/Windows/WindowsPlatform.h>

/// @brief WARPで実WindowのD3D12資源を生成しGPU待機後に解放する
int main()
{
    // 無効なHandleはDevice生成前に拒否する
    auto invalid = cue::WindowsRenderer::create(nullptr, {640, 480}, true);
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
    auto rendererResult = cue::WindowsRenderer::create(handleResult.take_value(), window->client_size(), true);
    if (!rendererResult.has_value())
    {
        return 5;
    }
    auto renderer = rendererResult.take_value();
    if (!renderer->is_warp())
    {
        return 6;
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
