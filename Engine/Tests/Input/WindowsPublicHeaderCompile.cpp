#include <Cue/Input/Windows/WindowsInputMessageSink.h>

/// @brief Windows Input Adapter Public HeaderがTarget依存だけでCompile可能か検証する
int main()
{
    cue::InputEventQueue queue;
    cue::WindowsInputMessageSink sink(queue);
    return sink.conversion_failure_count() == 0 ? 0 : 1;
}
