#include <Project/Project.h>

#ifdef _WINDOWS_
#error Project の公開 Header は Windows SDK へ依存してはならない
#endif

/// @brief Foundation だけで公開設定を利用でき、Editor / Renderer の型を要求しないことを確認する
int main()
{
    cue::ProjectDescriptor descriptor;
    return descriptor.formatVersion == 1 ? 0 : 1;
}
