#include <Cue/Platform/Window.h>
#include <Cue/Platform/WindowEvent.h>
#include <Cue/Platform/WindowSystem.h>

#ifdef _WINDOWS_
#error Platformの公開HeaderはWindows SDKへ依存してはならない
#endif

/// @brief Platform公開HeaderがWin32型なしで単体Compileできることを確認する
int main()
{
    cue::WindowDescriptor descriptor{"CueEngine", {1280, 720}};
    return descriptor.clientSize.width == 1280 ? 0 : 1;
}
