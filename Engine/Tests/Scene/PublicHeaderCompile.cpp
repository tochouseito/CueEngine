#include <Scene/Scene.h>

/// @brief Scene の保存用データは Runtime / ECS / Renderer の Header を要求しない
int main()
{
    cue::SceneDescriptor descriptor;
    return descriptor.formatVersion == 1 ? 0 : 1;
}
