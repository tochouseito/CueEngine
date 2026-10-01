#include <type_traits>

#include <RHI/Queue.h>

#ifdef _WINDOWS_
#error RHI の公開 Header は Windows SDK へ依存してはならない
#endif

/// @brief Queue の公開契約が Win32 型を露出せず単独 Compile できることを確認する
int main()
{
    static_assert(std::is_abstract_v<cue::IQueueContext>);
    static_assert(std::is_abstract_v<cue::IQueuePool>);
    return 0;
}
