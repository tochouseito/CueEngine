#include <Cue/Input/FrameInputSnapshot.h>
#include <Cue/Input/InputEvent.h>
#include <Cue/Input/InputEventQueue.h>
#include <Cue/Input/InputState.h>

/// @brief Portable Input Public Headerが単独TargetでCompile可能か検証する
int main()
{
    cue::InputEventQueue queue;
    cue::InputState state;
    state.begin_frame({});
    return queue.size() == 0 && state.snapshot().has_focus() ? 0 : 1;
}
