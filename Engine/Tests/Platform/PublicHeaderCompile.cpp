#include <Cue/Platform/Clock.h>
#include <Cue/Platform/Thread.h>
#include <Cue/Platform/Waiter.h>
#include <Cue/Platform/Window.h>
#include <Cue/Platform/WindowEvent.h>
#include <Cue/Platform/WindowSystem.h>

#include <type_traits>

#ifdef _WINDOWS_
#error Platformの公開HeaderはWindows SDKへ依存してはならない
#endif

/// @brief Platform公開HeaderがWin32型なしで単体Compileできることを確認する
int main()
{
    static_assert(std::is_abstract_v<cue::Clock>);
    static_assert(std::is_abstract_v<cue::Waiter>);
    static_assert(std::is_abstract_v<cue::Thread>);
    static_assert(std::is_abstract_v<cue::ThreadFactory>);
    cue::WindowDescriptor descriptor{"CueEngine", {1280, 720}};
    return descriptor.clientSize.width == 1280 ? 0 : 1;
}
