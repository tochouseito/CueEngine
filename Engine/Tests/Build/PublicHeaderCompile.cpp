#include <Cue/Build/DiagnosticBundle.h>
#include <Cue/Build/Toolchain.h>

/// @brief Cue.Build公開Headerを単独ConsumerとしてCompileできるか検証する
int main()
{
    cue::BuildEnvironmentInventory inventory;
    return inventory.candidates.empty() ? 0 : 1;
}
