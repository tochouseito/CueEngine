#include <type_traits>

#include <Platform/Clock.h>
#include <Platform/Diagnostics.h>
#include <Platform/Thread.h>
#include <Platform/Waiter.h>
#include <Platform/Window.h>
#include <Platform/WindowEvent.h>
#include <Platform/WindowSystem.h>

#ifdef _WINDOWS_
#error Platform の公開 Header は Windows SDK へ依存してはならない
#endif

/// @brief Platform 公開 Header が Win32 型なしで単体 Compile できることを確認する
int main()
{
    static_assert(std::is_abstract_v<cue::Clock>);
    static_assert(std::is_abstract_v<cue::Waiter>);
    static_assert(std::is_abstract_v<cue::Thread>);
    static_assert(std::is_abstract_v<cue::ThreadFactory>);
    cue::WindowDescriptor descriptor{"CueEngine", {1280, 720}};
    return descriptor.clientSize.width == 1280 ? 0 : 1;
}
