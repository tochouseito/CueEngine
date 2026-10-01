#include <type_traits>

#include <RHI/Queue.h>
#include <RHI/Command.h>

#ifdef _WINDOWS_
#error RHI の公開 Header は Windows SDK へ依存してはならない
#endif

/// @brief Queue の公開契約が Win32 型を露出せず単独 Compile できることを確認する
int main()
{
    static_assert(std::is_abstract_v<cue::IQueueContext>);
    static_assert(std::is_abstract_v<cue::IQueuePool>);
    static_assert(std::is_abstract_v<cue::ICommandContext>);
    static_assert(std::is_abstract_v<cue::ICommandPool>);
    return 0;
}
